/*
 * migration.c — Table sync, schema-mismatch detection, and auto-migration.
 */
#define _POSIX_C_SOURCE 200809L
#include "migration.h"
#include "handle_internal.h"
#include "schema_registry.h"
#include "query_builder.h"
#include "error_ctx.h"
#include "logger.h"
#include "db_driver.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MIG_MAX_NAME 128

typedef struct {
    int        cid;
    char       name[MIG_MAX_NAME];
    SqlRDBType type_class;   /* INT64 (for any INTEGER-affinity), REAL, TEXT, BLOB */
    bool       notnull;
    bool       pk;
    bool       is_unique;    /* set by apply_unique_flags */
} DbCol;

/* Normalize: schema-side INT32/INT64 → SQLite INTEGER affinity. */
static SqlRDBType schema_type_class(SqlRDBType t) {
    if (t == SQL_TYPE_INT32 || t == SQL_TYPE_INT64) return SQL_TYPE_INT64;
    return t;
}

/* Map a raw type string from PRAGMA table_info to an affinity. */
static SqlRDBType type_class_from_str(const char *s) {
    if (!s) return SQL_TYPE_TEXT;
    char up[32];
    size_t n = 0;
    while (s[n] && n + 1 < sizeof(up)) { up[n] = (char)toupper((unsigned char)s[n]); n++; }
    up[n] = '\0';
    if (strstr(up, "INT"))                                          return SQL_TYPE_INT64;
    if (strstr(up, "REAL") || strstr(up, "FLOA") || strstr(up, "DOUB")) return SQL_TYPE_REAL;
    if (strstr(up, "BLOB"))                                         return SQL_TYPE_BLOB;
    return SQL_TYPE_TEXT;
}

/* ------------------------------------------------------------------ */
/* PRAGMA queries                                                      */
/* ------------------------------------------------------------------ */

static SqlRDBResult read_table_info(SqlRDBHandle *h, const char *table,
                                     DbCol **out_cols, size_t *out_n) {
    *out_cols = NULL;
    *out_n    = 0;

    char sql[256];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(\"%s\")", table);

    void *stmt = NULL;
    SqlRDBResult r = h->driver->prepare(h->driver_ctx, sql, &stmt);
    if (r != SQL_RDB_OK) return r;

    DbCol  *cols = NULL;
    size_t  cap  = 0;
    size_t  n    = 0;
    for (;;) {
        bool has_row = false;
        r = h->driver->step(stmt, &has_row);
        if (r != SQL_RDB_OK || !has_row) break;

        if (n == cap) {
            size_t new_cap = cap ? cap * 2 : 8;
            DbCol *p = realloc(cols, new_cap * sizeof(*cols));
            if (!p) { free(cols); h->driver->finalize(stmt); return SQL_RDB_ERR_NO_MEMORY; }
            cols = p; cap = new_cap;
        }
        memset(&cols[n], 0, sizeof(cols[n]));

        int32_t cid = 0;
        h->driver->column_int32(stmt, 0, &cid);
        cols[n].cid = cid;

        const char *name_p = NULL; size_t name_len = 0;
        h->driver->column_text(stmt, 1, &name_p, &name_len);
        if (name_p && name_len < sizeof(cols[n].name)) {
            memcpy(cols[n].name, name_p, name_len);
            cols[n].name[name_len] = '\0';
        }

        const char *type_p = NULL; size_t type_len = 0;
        h->driver->column_text(stmt, 2, &type_p, &type_len);
        char raw[32] = {0};
        if (type_p && type_len < sizeof(raw)) {
            memcpy(raw, type_p, type_len);
            raw[type_len] = '\0';
        }
        cols[n].type_class = type_class_from_str(raw);

        int32_t nn = 0; h->driver->column_int32(stmt, 3, &nn);
        cols[n].notnull = (nn != 0);

        int32_t pk = 0; h->driver->column_int32(stmt, 5, &pk);
        cols[n].pk = (pk != 0);

        n++;
    }
    h->driver->finalize(stmt);

    if (r != SQL_RDB_OK) { free(cols); return r; }
    *out_cols = cols;
    *out_n    = n;
    return SQL_RDB_OK;
}

/*
 * Mark cols[] entries with is_unique=true when they belong to a single-column
 * UNIQUE constraint index (origin='u'). PRIMARY KEY indexes are intentionally
 * ignored — PK is checked separately against the dc.pk field.
 */
static SqlRDBResult apply_unique_flags(SqlRDBHandle *h, const char *table,
                                        DbCol *cols, size_t n) {
    char sql[256];
    snprintf(sql, sizeof(sql), "PRAGMA index_list(\"%s\")", table);

    void *stmt = NULL;
    SqlRDBResult r = h->driver->prepare(h->driver_ctx, sql, &stmt);
    if (r != SQL_RDB_OK) return r;

    char idx_names[64][96];
    int  idx_count = 0;
    for (;;) {
        bool has_row = false;
        r = h->driver->step(stmt, &has_row);
        if (r != SQL_RDB_OK || !has_row) break;

        int32_t uniq = 0;
        h->driver->column_int32(stmt, 2, &uniq);
        if (!uniq) continue;

        const char *origin_p = NULL; size_t origin_len = 0;
        h->driver->column_text(stmt, 3, &origin_p, &origin_len);
        if (!(origin_p && origin_len == 1 && origin_p[0] == 'u')) continue;

        const char *name_p = NULL; size_t name_len = 0;
        h->driver->column_text(stmt, 1, &name_p, &name_len);
        if (idx_count < (int)(sizeof(idx_names)/sizeof(idx_names[0])) &&
            name_p && name_len < sizeof(idx_names[0])) {
            memcpy(idx_names[idx_count], name_p, name_len);
            idx_names[idx_count][name_len] = '\0';
            idx_count++;
        }
    }
    h->driver->finalize(stmt);
    if (r != SQL_RDB_OK) return r;

    for (int i = 0; i < idx_count; i++) {
        snprintf(sql, sizeof(sql), "PRAGMA index_info(\"%s\")", idx_names[i]);
        r = h->driver->prepare(h->driver_ctx, sql, &stmt);
        if (r != SQL_RDB_OK) return r;

        /* Collect column names referenced by this index */
        char ref_cols[8][MIG_MAX_NAME];
        int  ref_count = 0;
        for (;;) {
            bool has_row = false;
            r = h->driver->step(stmt, &has_row);
            if (r != SQL_RDB_OK || !has_row) break;
            const char *cn_p = NULL; size_t cn_len = 0;
            h->driver->column_text(stmt, 2, &cn_p, &cn_len);
            if (cn_p && ref_count < (int)(sizeof(ref_cols)/sizeof(ref_cols[0])) &&
                cn_len < sizeof(ref_cols[0])) {
                memcpy(ref_cols[ref_count], cn_p, cn_len);
                ref_cols[ref_count][cn_len] = '\0';
                ref_count++;
            }
        }
        h->driver->finalize(stmt);
        if (r != SQL_RDB_OK) return r;

        /* Only single-column UNIQUE indexes set the per-column flag (the
         * design treats per-column UNIQUE specifically; composite indexes
         * are not modeled in SqlRDBColumnDef). */
        if (ref_count == 1) {
            for (size_t k = 0; k < n; k++) {
                if (strcmp(cols[k].name, ref_cols[0]) == 0) {
                    cols[k].is_unique = true;
                    break;
                }
            }
        }
    }
    return SQL_RDB_OK;
}

/* Detect whether the CREATE TABLE statement of `table` includes the STRICT keyword. */
static SqlRDBResult check_strict(SqlRDBHandle *h, const char *table, bool *out_strict) {
    *out_strict = false;
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT sql FROM sqlite_schema WHERE type='table' AND name='%s'", table);
    void *stmt = NULL;
    SqlRDBResult r = h->driver->prepare(h->driver_ctx, sql, &stmt);
    if (r != SQL_RDB_OK) return r;

    bool has_row = false;
    r = h->driver->step(stmt, &has_row);
    if (r != SQL_RDB_OK || !has_row) { h->driver->finalize(stmt); return SQL_RDB_OK; }

    const char *p = NULL; size_t plen = 0;
    h->driver->column_text(stmt, 0, &p, &plen);
    if (p) {
        /* Scan the post-')' tail for the STRICT keyword (case-insensitive). */
        const char *last_paren = NULL;
        for (size_t i = 0; i < plen; i++) if (p[i] == ')') last_paren = p + i;
        if (last_paren) {
            const char *q   = last_paren + 1;
            const char *end = p + plen;
            while (q < end) {
                while (q < end && (*q == ' ' || *q == '\t' || *q == ',' || *q == '\n')) q++;
                if (q + 6 > end) break;
                if (strncasecmp(q, "STRICT", 6) == 0 &&
                    (q + 6 == end || !isalnum((unsigned char)q[6]))) {
                    *out_strict = true;
                    break;
                }
                while (q < end && (isalnum((unsigned char)*q) || *q == '_')) q++;
            }
        }
    }
    h->driver->finalize(stmt);
    return SQL_RDB_OK;
}

/* ------------------------------------------------------------------ */
/* Comparison                                                          */
/* ------------------------------------------------------------------ */

/* Returns non-zero with a sentinel describing the first mismatched aspect. */
static int compare_columns(const SqlRDBColumnDef *sc, const DbCol *dc) {
    if (strcmp(sc->name, dc->name) != 0)                    return 1; /* name */
    if (schema_type_class(sc->type) != dc->type_class)      return 2; /* type */
    bool sc_notnull = !(sc->flags & SQL_COL_FLAG_NULLABLE);
    if (sc_notnull != dc->notnull)                          return 3; /* NOT NULL */
    bool sc_pk = (sc->flags & SQL_COL_FLAG_PRIMARY_KEY) != 0;
    if (sc_pk != dc->pk)                                    return 4; /* PRIMARY KEY */
    bool sc_unique = (sc->flags & SQL_COL_FLAG_UNIQUE) != 0;
    if (sc_pk) sc_unique = false; /* PK implies uniqueness; DDL suppresses UNIQUE */
    if (sc_unique && !dc->is_unique)                        return 5; /* UNIQUE missing */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public entry                                                        */
/* ------------------------------------------------------------------ */

SqlRDBResult c2sql_internal_migrate_table(SqlRDBHandle *h, const SqlRDBSchema *schema) {
    /* CREATE TABLE IF NOT EXISTS — idempotent for existing tables. */
    char           *create_sql = NULL;
    SqlRDBQuerySpec cspec      = { .op = C2SQL_QB_CREATE, .schema = schema };
    SqlRDBResult    r          = c2sql_internal_qb_build(&cspec, &create_sql, NULL);
    if (r != SQL_RDB_OK) return r;
    r = h->driver->exec(h->driver_ctx, create_sql);
    free(create_sql);
    if (r != SQL_RDB_OK) {
        c2sql_internal_err_set(&h->error, r,
                               "Migrate: CREATE TABLE failed for '%s'", schema->name);
        return r;
    }

    /* Read existing layout (must always be populated after CREATE). */
    DbCol *db_cols = NULL;
    size_t db_n    = 0;
    r = read_table_info(h, schema->name, &db_cols, &db_n);
    if (r != SQL_RDB_OK) { free(db_cols); return r; }
    if (db_n == 0) { free(db_cols); return SQL_RDB_ERR_INTERNAL; }

    r = apply_unique_flags(h, schema->name, db_cols, db_n);
    if (r != SQL_RDB_OK) { free(db_cols); return r; }

    /* STRICT detection: warn (or reject if require_strict) but do not abort here
     * — let the column-by-column comparison run too so the error reflects the
     * primary failure cause. */
    bool is_strict = false;
    check_strict(h, schema->name, &is_strict);
    if (!is_strict) {
        if (h->config.require_strict) {
            c2sql_internal_err_set(&h->error, SQL_RDB_ERR_SCHEMA_MISMATCH,
                "Migrate: '%s' is non-STRICT but require_strict=true", schema->name);
            free(db_cols);
            return SQL_RDB_ERR_SCHEMA_MISMATCH;
        }
        c2sql_internal_log(&h->logger, SQL_RDB_OK, NULL,
                           "Migrate: table is non-STRICT (allowed)");
    }

    /* Prefix comparison */
    size_t common = (db_n < schema->col_count) ? db_n : schema->col_count;
    for (size_t i = 0; i < common; i++) {
        int diff = compare_columns(&schema->cols[i], &db_cols[i]);
        if (diff) {
            c2sql_internal_err_set(&h->error, SQL_RDB_ERR_SCHEMA_MISMATCH,
                "Migrate: column %zu of '%s' mismatch (kind=%d)",
                i, schema->name, diff);
            free(db_cols);
            return SQL_RDB_ERR_SCHEMA_MISMATCH;
        }
    }

    if (db_n > schema->col_count) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_SCHEMA_MISMATCH,
            "Migrate: '%s' has %zu columns, schema has %zu",
            schema->name, db_n, schema->col_count);
        free(db_cols);
        return SQL_RDB_ERR_SCHEMA_MISMATCH;
    }

    if (db_n < schema->col_count) {
        if (!h->config.auto_migrate) {
            c2sql_internal_err_set(&h->error, SQL_RDB_ERR_SCHEMA_MISMATCH,
                "Migrate: '%s' missing %zu trailing column(s); set auto_migrate=true",
                schema->name, schema->col_count - db_n);
            free(db_cols);
            return SQL_RDB_ERR_SCHEMA_MISMATCH;
        }

        for (size_t i = db_n; i < schema->col_count; i++) {
            SqlRDBColumnDef new_col = schema->cols[i];
            char           *alter_sql = NULL;
            SqlRDBQuerySpec aspec = {
                .op      = C2SQL_QB_ALTER_ADD,
                .schema  = schema,
                .new_col = &new_col,
            };
            r = c2sql_internal_qb_build(&aspec, &alter_sql, NULL);
            if (r != SQL_RDB_OK) { free(db_cols); return r; }
            r = h->driver->exec(h->driver_ctx, alter_sql);
            free(alter_sql);
            if (r != SQL_RDB_OK) {
                c2sql_internal_err_set(&h->error, r,
                    "Migrate: ALTER TABLE ADD COLUMN '%s' failed", new_col.name);
                free(db_cols);
                return r;
            }
        }

        /* Re-validate post-migration */
        free(db_cols);
        db_cols = NULL;
        db_n    = 0;
        r = read_table_info(h, schema->name, &db_cols, &db_n);
        if (r != SQL_RDB_OK) { free(db_cols); return r; }
        r = apply_unique_flags(h, schema->name, db_cols, db_n);
        if (r != SQL_RDB_OK) { free(db_cols); return r; }

        if (db_n != schema->col_count) {
            c2sql_internal_err_set(&h->error, SQL_RDB_ERR_SCHEMA_MISMATCH,
                "Migrate: post-migration column count mismatch (%zu vs %zu)",
                db_n, schema->col_count);
            free(db_cols);
            return SQL_RDB_ERR_SCHEMA_MISMATCH;
        }
        for (size_t i = 0; i < db_n; i++) {
            int diff = compare_columns(&schema->cols[i], &db_cols[i]);
            if (diff) {
                c2sql_internal_err_set(&h->error, SQL_RDB_ERR_SCHEMA_MISMATCH,
                    "Migrate: post-migration column %zu mismatch (kind=%d)", i, diff);
                free(db_cols);
                return SQL_RDB_ERR_SCHEMA_MISMATCH;
            }
        }
    }

    free(db_cols);
    return SQL_RDB_OK;
}
