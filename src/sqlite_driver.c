/*
 * sqlite_driver.c — SQLite3 implementation of SqlRDBDriver vtable.
 *
 * Tasks 2.2 / 2.3: connection, transactions, prepared statements,
 * bind/column for all supported types.
 */
#include "sqlite_driver.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#if SQLITE_VERSION_NUMBER < 3035000
#  error "libc2sql requires SQLite3 >= 3.35.0"
#endif

/* ------------------------------------------------------------------ */
/* Error translation                                                   */
/* ------------------------------------------------------------------ */

static SqlRDBResult map_open_error(int rc) {
    switch (rc) {
    case SQLITE_OK:     return SQL_RDB_OK;
    case SQLITE_NOMEM:  return SQL_RDB_ERR_NO_MEMORY;
    default:            return SQL_RDB_ERR_DB_OPEN;
    }
}

static SqlRDBResult map_error(int rc) {
    switch (rc) {
    case SQLITE_OK:
    case SQLITE_DONE:
    case SQLITE_ROW:    return SQL_RDB_OK;
    case SQLITE_NOMEM:  return SQL_RDB_ERR_NO_MEMORY;
    default:            return SQL_RDB_ERR_DRIVER;
    }
}

/* ------------------------------------------------------------------ */
/* Task 2.2: connection and transaction functions                      */
/* ------------------------------------------------------------------ */

static SqlRDBResult sqlite_open(const char *dsn, void **out_ctx) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(dsn, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
        NULL);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close_v2(db);
        *out_ctx = NULL;
        return map_open_error(rc);
    }
    *out_ctx = db;
    return SQL_RDB_OK;
}

static SqlRDBResult sqlite_close(void *ctx) {
    return map_error(sqlite3_close_v2((sqlite3 *)ctx));
}

static SqlRDBResult sqlite_exec(void *ctx, const char *sql) {
    char *errmsg = NULL;
    int rc = sqlite3_exec((sqlite3 *)ctx, sql, NULL, NULL, &errmsg);
    if (errmsg) sqlite3_free(errmsg);
    return map_error(rc);
}

static SqlRDBResult exec_fmt(void *ctx, const char *fmt, const char *name) {
    char sql[256];
    snprintf(sql, sizeof(sql), fmt, name);
    return sqlite_exec(ctx, sql);
}

static SqlRDBResult sqlite_begin(void *ctx) {
    return sqlite_exec(ctx, "BEGIN");
}

static SqlRDBResult sqlite_commit(void *ctx) {
    return sqlite_exec(ctx, "COMMIT");
}

static SqlRDBResult sqlite_rollback(void *ctx) {
    return sqlite_exec(ctx, "ROLLBACK");
}

static SqlRDBResult sqlite_savepoint(void *ctx, const char *name) {
    return exec_fmt(ctx, "SAVEPOINT %s", name);
}

static SqlRDBResult sqlite_release_sp(void *ctx, const char *name) {
    return exec_fmt(ctx, "RELEASE SAVEPOINT %s", name);
}

static SqlRDBResult sqlite_rollback_sp(void *ctx, const char *name) {
    return exec_fmt(ctx, "ROLLBACK TO SAVEPOINT %s", name);
}

static int sqlite_changes(void *ctx) {
    return (int)sqlite3_changes((sqlite3 *)ctx);
}

/* ------------------------------------------------------------------ */
/* Task 2.3: prepared statements, bind, step, column                  */
/* ------------------------------------------------------------------ */

static SqlRDBResult sqlite_prepare(void *ctx, const char *sql, void **out_stmt) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2((sqlite3 *)ctx, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        *out_stmt = NULL;
        return SQL_RDB_ERR_DRIVER;
    }
    *out_stmt = stmt;
    return SQL_RDB_OK;
}

static SqlRDBResult sqlite_bind_int64(void *stmt, int index, int64_t value) {
    return map_error(sqlite3_bind_int64((sqlite3_stmt *)stmt, index, value));
}

static SqlRDBResult sqlite_bind_int32(void *stmt, int index, int32_t value) {
    return map_error(sqlite3_bind_int((sqlite3_stmt *)stmt, index, (int)value));
}

static SqlRDBResult sqlite_bind_real(void *stmt, int index, double value) {
    return map_error(sqlite3_bind_double((sqlite3_stmt *)stmt, index, value));
}

static SqlRDBResult sqlite_bind_text(void *stmt, int index,
                                     const char *value, int len) {
    return map_error(sqlite3_bind_text(
        (sqlite3_stmt *)stmt, index, value, len, SQLITE_TRANSIENT));
}

static SqlRDBResult sqlite_bind_blob(void *stmt, int index,
                                     const void *value, size_t len) {
    return map_error(sqlite3_bind_blob(
        (sqlite3_stmt *)stmt, index, value, (int)len, SQLITE_TRANSIENT));
}

static SqlRDBResult sqlite_bind_null(void *stmt, int index) {
    return map_error(sqlite3_bind_null((sqlite3_stmt *)stmt, index));
}

static SqlRDBResult sqlite_step(void *stmt, bool *out_has_row) {
    int rc = sqlite3_step((sqlite3_stmt *)stmt);
    if (rc == SQLITE_ROW) {
        *out_has_row = true;
        return SQL_RDB_OK;
    }
    if (rc == SQLITE_DONE) {
        *out_has_row = false;
        return SQL_RDB_OK;
    }
    return SQL_RDB_ERR_DRIVER;
}

static SqlRDBResult sqlite_column_int64(void *stmt, int index, int64_t *out) {
    *out = sqlite3_column_int64((sqlite3_stmt *)stmt, index);
    return SQL_RDB_OK;
}

static SqlRDBResult sqlite_column_int32(void *stmt, int index, int32_t *out) {
    /* Range check (WARN_TRUNCATED) is the responsibility of the type-mapping layer */
    *out = (int32_t)sqlite3_column_int64((sqlite3_stmt *)stmt, index);
    return SQL_RDB_OK;
}

static SqlRDBResult sqlite_column_real(void *stmt, int index, double *out) {
    *out = sqlite3_column_double((sqlite3_stmt *)stmt, index);
    return SQL_RDB_OK;
}

static SqlRDBResult sqlite_column_text(void *stmt, int index,
                                       const char **out_ptr, size_t *out_len) {
    *out_ptr = (const char *)sqlite3_column_text((sqlite3_stmt *)stmt, index);
    *out_len = (size_t)sqlite3_column_bytes((sqlite3_stmt *)stmt, index);
    return SQL_RDB_OK;
}

static SqlRDBResult sqlite_column_blob(void *stmt, int index,
                                       const void **out_ptr, size_t *out_len) {
    *out_ptr = sqlite3_column_blob((sqlite3_stmt *)stmt, index);
    *out_len = (size_t)sqlite3_column_bytes((sqlite3_stmt *)stmt, index);
    return SQL_RDB_OK;
}

static SqlRDBResult sqlite_column_isnull(void *stmt, int index, bool *out) {
    *out = (sqlite3_column_type((sqlite3_stmt *)stmt, index) == SQLITE_NULL);
    return SQL_RDB_OK;
}

static SqlRDBResult sqlite_reset(void *stmt) {
    return map_error(sqlite3_reset((sqlite3_stmt *)stmt));
}

static SqlRDBResult sqlite_finalize(void *stmt) {
    return map_error(sqlite3_finalize((sqlite3_stmt *)stmt));
}

/* ------------------------------------------------------------------ */
/* Exported driver instance                                            */
/* ------------------------------------------------------------------ */

const SqlRDBDriver g_sqlite3_driver = {
    .name         = "sqlite3",
    .dialect      = C2SQL_DIALECT_SQLITE,
    .open         = sqlite_open,
    .close        = sqlite_close,
    .exec         = sqlite_exec,
    .prepare      = sqlite_prepare,
    .bind_int64   = sqlite_bind_int64,
    .bind_int32   = sqlite_bind_int32,
    .bind_real    = sqlite_bind_real,
    .bind_text    = sqlite_bind_text,
    .bind_blob    = sqlite_bind_blob,
    .bind_null    = sqlite_bind_null,
    .step         = sqlite_step,
    .column_int64 = sqlite_column_int64,
    .column_int32 = sqlite_column_int32,
    .column_real  = sqlite_column_real,
    .column_text  = sqlite_column_text,
    .column_blob  = sqlite_column_blob,
    .column_isnull= sqlite_column_isnull,
    .reset        = sqlite_reset,
    .finalize     = sqlite_finalize,
    .begin        = sqlite_begin,
    .commit       = sqlite_commit,
    .rollback     = sqlite_rollback,
    .savepoint    = sqlite_savepoint,
    .release_sp   = sqlite_release_sp,
    .rollback_sp  = sqlite_rollback_sp,
    .changes      = sqlite_changes,
};
