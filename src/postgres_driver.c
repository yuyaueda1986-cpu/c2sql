/*
 * postgres_driver.c — PostgreSQL (libpq) implementation of SqlRDBDriver.
 *
 * Impedance model: SQLite binds incrementally then steps, whereas libpq takes
 * all parameters at once in PQexecParams. A PgStmt therefore accumulates bound
 * parameters and lazily executes on the first step(); subsequent step() calls
 * walk the result rows. reset() clears the result and bound params so the
 * cached statement can be rebound and re-executed.
 *
 * Parameters are sent in text format (the server infers the type from context),
 * except BLOBs which are sent in binary with an explicit bytea type OID. Result
 * rows are requested in text format; BLOB columns are decoded with
 * PQunescapeBytea on read.
 */
#include "postgres_driver.h"

#include <libpq-fe.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C2SQL_PG_BYTEAOID 17  /* pg_type.oid of bytea (stable catalog value) */

typedef struct {
    PGconn *conn;
    int     last_changes;  /* affected-row count of the last command */
} PgCtx;

typedef struct {
    PgCtx    *ctx;
    char     *sql;
    int       nparams;
    char    **values;   /* [nparams]; NULL entry => SQL NULL */
    int      *lengths;  /* [nparams]; used for binary params  */
    int      *formats;  /* [nparams]; 0=text, 1=binary         */
    Oid      *types;    /* [nparams]; 0=infer, else explicit   */
    bool     *owned;    /* [nparams]; whether values[i] is heap */
    PGresult *res;
    int       nrows;
    int       row;      /* current row, -1 before first step */
    bool      executed;
    void     *blob_tmp; /* last PQunescapeBytea buffer (PQfreemem) */
} PgStmt;

/* ------------------------------------------------------------------ */
/* Connection / exec                                                   */
/* ------------------------------------------------------------------ */

/* Swallow NOTICE/INFO chatter (e.g. "relation already exists, skipping") so
 * the library does not write to the client's stderr behind its back. */
static void pg_quiet_notice(void *arg, const char *message) {
    (void)arg;
    (void)message;
}

static SqlRDBResult pg_open(const char *dsn, void **out_ctx) {
    *out_ctx = NULL;
    PGconn *conn = PQconnectdb(dsn);
    if (!conn) return SQL_RDB_ERR_NO_MEMORY;
    if (PQstatus(conn) != CONNECTION_OK) {
        PQfinish(conn);
        return SQL_RDB_ERR_DB_OPEN;
    }
    PQsetNoticeProcessor(conn, pg_quiet_notice, NULL);
    PgCtx *c = calloc(1, sizeof(*c));
    if (!c) { PQfinish(conn); return SQL_RDB_ERR_NO_MEMORY; }
    c->conn = conn;
    *out_ctx = c;
    return SQL_RDB_OK;
}

static SqlRDBResult pg_close(void *ctx) {
    PgCtx *c = ctx;
    if (c) {
        if (c->conn) PQfinish(c->conn);
        free(c);
    }
    return SQL_RDB_OK;
}

static SqlRDBResult pg_exec(void *ctx, const char *sql) {
    PgCtx *c = ctx;
    PGresult *res = PQexec(c->conn, sql);
    if (!res) return SQL_RDB_ERR_NO_MEMORY;
    ExecStatusType st = PQresultStatus(res);
    SqlRDBResult r = (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK)
                     ? SQL_RDB_OK : SQL_RDB_ERR_DRIVER;
    if (r == SQL_RDB_OK) {
        const char *ct = PQcmdTuples(res);
        c->last_changes = (ct && *ct) ? atoi(ct) : 0;
    }
    PQclear(res);
    return r;
}

static SqlRDBResult pg_exec_fmt(void *ctx, const char *fmt, const char *name) {
    char sql[160];
    snprintf(sql, sizeof(sql), fmt, name);
    return pg_exec(ctx, sql);
}

static SqlRDBResult pg_begin(void *ctx)    { return pg_exec(ctx, "BEGIN"); }
static SqlRDBResult pg_commit(void *ctx)   { return pg_exec(ctx, "COMMIT"); }
static SqlRDBResult pg_rollback(void *ctx) { return pg_exec(ctx, "ROLLBACK"); }

static SqlRDBResult pg_savepoint(void *ctx, const char *name) {
    return pg_exec_fmt(ctx, "SAVEPOINT %s", name);
}
static SqlRDBResult pg_release_sp(void *ctx, const char *name) {
    return pg_exec_fmt(ctx, "RELEASE SAVEPOINT %s", name);
}
static SqlRDBResult pg_rollback_sp(void *ctx, const char *name) {
    return pg_exec_fmt(ctx, "ROLLBACK TO SAVEPOINT %s", name);
}

static int pg_changes(void *ctx) {
    return ((PgCtx *)ctx)->last_changes;
}

/* ------------------------------------------------------------------ */
/* Prepared statements                                                 */
/* ------------------------------------------------------------------ */

/* Largest $N parameter index referenced by the SQL text. */
static int pg_count_params(const char *sql) {
    int max = 0;
    for (const char *p = sql; *p; p++) {
        if (*p == '$' && isdigit((unsigned char)p[1])) {
            int v = 0;
            p++;
            while (isdigit((unsigned char)*p)) { v = v * 10 + (*p - '0'); p++; }
            if (v > max) max = v;
            p--; /* compensate for the loop's p++ */
        }
    }
    return max;
}

static SqlRDBResult pg_prepare(void *ctx, const char *sql, void **out_stmt) {
    *out_stmt = NULL;
    PgStmt *s = calloc(1, sizeof(*s));
    if (!s) return SQL_RDB_ERR_NO_MEMORY;
    s->ctx = ctx;
    s->row = -1;
    s->sql = malloc(strlen(sql) + 1);
    if (!s->sql) { free(s); return SQL_RDB_ERR_NO_MEMORY; }
    memcpy(s->sql, sql, strlen(sql) + 1);

    s->nparams = pg_count_params(sql);
    if (s->nparams > 0) {
        s->values  = (char **)calloc((size_t)s->nparams, sizeof(char *));
        s->lengths = calloc((size_t)s->nparams, sizeof(int));
        s->formats = calloc((size_t)s->nparams, sizeof(int));
        s->types   = calloc((size_t)s->nparams, sizeof(Oid));
        s->owned   = calloc((size_t)s->nparams, sizeof(bool));
        if (!s->values || !s->lengths || !s->formats || !s->types || !s->owned) {
            free((void *)s->values); free(s->lengths); free(s->formats);
            free(s->types); free(s->owned); free(s->sql); free(s);
            return SQL_RDB_ERR_NO_MEMORY;
        }
    }
    *out_stmt = s;
    return SQL_RDB_OK;
}

/* Store a copy of `len` bytes (binary or already-NUL-terminated text). */
static SqlRDBResult store_param(PgStmt *s, int index, const void *data,
                                int len, int fmt, Oid oid) {
    if (index < 1 || index > s->nparams) return SQL_RDB_ERR_INVALID_ARG;
    int i = index - 1;
    if (s->owned[i]) free(s->values[i]);
    s->values[i] = NULL;
    s->owned[i]  = false;
    s->lengths[i] = 0;
    s->formats[i] = fmt;
    s->types[i]   = oid;
    if (data == NULL) return SQL_RDB_OK;  /* SQL NULL */

    char *buf = malloc(len > 0 ? (size_t)len : 1);
    if (!buf) return SQL_RDB_ERR_NO_MEMORY;
    memcpy(buf, data, (size_t)len);
    s->values[i]  = buf;
    s->owned[i]   = true;
    s->lengths[i] = len;
    return SQL_RDB_OK;
}

static SqlRDBResult pg_bind_int64(void *stmt, int index, int64_t value) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%lld", (long long)value);
    return store_param(stmt, index, tmp, n + 1, 0, 0);
}

static SqlRDBResult pg_bind_int32(void *stmt, int index, int32_t value) {
    char tmp[16];
    int n = snprintf(tmp, sizeof(tmp), "%d", (int)value);
    return store_param(stmt, index, tmp, n + 1, 0, 0);
}

static SqlRDBResult pg_bind_real(void *stmt, int index, double value) {
    char tmp[40];
    int n = snprintf(tmp, sizeof(tmp), "%.17g", value);
    return store_param(stmt, index, tmp, n + 1, 0, 0);
}

static SqlRDBResult pg_bind_text(void *stmt, int index, const char *value, int len) {
    PgStmt *s = stmt;
    if (index < 1 || index > s->nparams) return SQL_RDB_ERR_INVALID_ARG;
    int i = index - 1;
    if (s->owned[i]) free(s->values[i]);
    s->values[i] = NULL; s->owned[i] = false;
    s->lengths[i] = 0; s->formats[i] = 0; s->types[i] = 0;
    if (!value) return SQL_RDB_OK;

    size_t slen = (len < 0) ? strlen(value) : (size_t)len;
    char *buf = malloc(slen + 1);
    if (!buf) return SQL_RDB_ERR_NO_MEMORY;
    memcpy(buf, value, slen);
    buf[slen] = '\0';
    s->values[i] = buf; s->owned[i] = true; s->lengths[i] = (int)slen;
    return SQL_RDB_OK;
}

static SqlRDBResult pg_bind_blob(void *stmt, int index, const void *value, size_t len) {
    return store_param(stmt, index, value, (int)len, 1, C2SQL_PG_BYTEAOID);
}

static SqlRDBResult pg_bind_null(void *stmt, int index) {
    return store_param(stmt, index, NULL, 0, 0, 0);
}

static SqlRDBResult pg_step(void *stmt, bool *out_has_row) {
    PgStmt *s = stmt;
    if (!s->executed) {
        s->res = PQexecParams(s->ctx->conn, s->sql, s->nparams,
                              s->nparams ? s->types : NULL,
                              (const char *const *)s->values,
                              s->nparams ? s->lengths : NULL,
                              s->nparams ? s->formats : NULL,
                              0 /* text result format */);
        ExecStatusType st = s->res ? PQresultStatus(s->res) : PGRES_FATAL_ERROR;
        if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
            if (s->res) { PQclear(s->res); s->res = NULL; }
            return SQL_RDB_ERR_DRIVER;
        }
        const char *ct = PQcmdTuples(s->res);
        s->ctx->last_changes = (ct && *ct) ? atoi(ct) : 0;
        s->nrows = PQntuples(s->res);
        s->row = 0;
        s->executed = true;
    } else {
        s->row++;
    }
    *out_has_row = (s->row < s->nrows);
    return SQL_RDB_OK;
}

static SqlRDBResult pg_column_int64(void *stmt, int index, int64_t *out) {
    const PgStmt *s = stmt;
    *out = PQgetisnull(s->res, s->row, index)
           ? 0 : (int64_t)strtoll(PQgetvalue(s->res, s->row, index), NULL, 10);
    return SQL_RDB_OK;
}

static SqlRDBResult pg_column_int32(void *stmt, int index, int32_t *out) {
    const PgStmt *s = stmt;
    *out = PQgetisnull(s->res, s->row, index)
           ? 0 : (int32_t)strtoll(PQgetvalue(s->res, s->row, index), NULL, 10);
    return SQL_RDB_OK;
}

static SqlRDBResult pg_column_real(void *stmt, int index, double *out) {
    const PgStmt *s = stmt;
    *out = PQgetisnull(s->res, s->row, index)
           ? 0.0 : strtod(PQgetvalue(s->res, s->row, index), NULL);
    return SQL_RDB_OK;
}

static SqlRDBResult pg_column_text(void *stmt, int index,
                                   const char **out_ptr, size_t *out_len) {
    const PgStmt *s = stmt;
    *out_ptr = PQgetvalue(s->res, s->row, index);
    *out_len = (size_t)PQgetlength(s->res, s->row, index);
    return SQL_RDB_OK;
}

static SqlRDBResult pg_column_blob(void *stmt, int index,
                                   const void **out_ptr, size_t *out_len) {
    PgStmt *s = stmt;
    if (s->blob_tmp) { PQfreemem(s->blob_tmp); s->blob_tmp = NULL; }
    if (PQgetisnull(s->res, s->row, index)) {
        *out_ptr = NULL;
        *out_len = 0;
        return SQL_RDB_OK;
    }
    size_t blen = 0;
    unsigned char *bin = PQunescapeBytea(
        (const unsigned char *)PQgetvalue(s->res, s->row, index), &blen);
    if (!bin) return SQL_RDB_ERR_NO_MEMORY;
    s->blob_tmp = bin;
    *out_ptr = bin;
    *out_len = blen;
    return SQL_RDB_OK;
}

static SqlRDBResult pg_column_isnull(void *stmt, int index, bool *out) {
    const PgStmt *s = stmt;
    *out = PQgetisnull(s->res, s->row, index) != 0;
    return SQL_RDB_OK;
}

static void clear_results(PgStmt *s) {
    if (s->res) { PQclear(s->res); s->res = NULL; }
    if (s->blob_tmp) { PQfreemem(s->blob_tmp); s->blob_tmp = NULL; }
    s->executed = false;
    s->row = -1;
    s->nrows = 0;
}

static void clear_params(PgStmt *s) {
    for (int i = 0; i < s->nparams; i++) {
        if (s->owned[i]) free(s->values[i]);
        s->values[i] = NULL;
        s->owned[i] = false;
        s->lengths[i] = 0;
        s->formats[i] = 0;
        s->types[i] = 0;
    }
}

static SqlRDBResult pg_reset(void *stmt) {
    PgStmt *s = stmt;
    clear_results(s);
    clear_params(s);
    return SQL_RDB_OK;
}

static SqlRDBResult pg_finalize(void *stmt) {
    PgStmt *s = stmt;
    if (!s) return SQL_RDB_OK;
    clear_results(s);
    clear_params(s);
    free((void *)s->values);
    free(s->lengths);
    free(s->formats);
    free(s->types);
    free(s->owned);
    free(s->sql);
    free(s);
    return SQL_RDB_OK;
}

/* ------------------------------------------------------------------ */
/* Exported driver instance                                            */
/* ------------------------------------------------------------------ */

const SqlRDBDriver g_postgres_driver = {
    .name          = "postgres",
    .dialect       = C2SQL_DIALECT_POSTGRES,
    .open          = pg_open,
    .close         = pg_close,
    .exec          = pg_exec,
    .prepare       = pg_prepare,
    .bind_int64    = pg_bind_int64,
    .bind_int32    = pg_bind_int32,
    .bind_real     = pg_bind_real,
    .bind_text     = pg_bind_text,
    .bind_blob     = pg_bind_blob,
    .bind_null     = pg_bind_null,
    .step          = pg_step,
    .column_int64  = pg_column_int64,
    .column_int32  = pg_column_int32,
    .column_real   = pg_column_real,
    .column_text   = pg_column_text,
    .column_blob   = pg_column_blob,
    .column_isnull = pg_column_isnull,
    .reset         = pg_reset,
    .finalize      = pg_finalize,
    .begin         = pg_begin,
    .commit        = pg_commit,
    .rollback      = pg_rollback,
    .savepoint     = pg_savepoint,
    .release_sp    = pg_release_sp,
    .rollback_sp   = pg_rollback_sp,
    .changes       = pg_changes,
};
