/*
 * c2sql.c — Public API implementation for libc2sql.
 *
 * Task 3:  SqlRDBLastError, SqlRDBSetLogger, SqlRDBFreeResult
 * Task 9:  SqlRDBInit, SqlRDBClose, SqlRDBSetConfig
 * Task 10: SqlRDBRegisterStruct, Write/WriteMany, Read/ReadMany, Delete
 * Task 11: SqlRDBBeginTx, SqlRDBCommitTx, SqlRDBRollbackTx
 * Task 12: SqlRDBWriteBlobField, SqlRDBReadBlobField
 */
#define _POSIX_C_SOURCE 200809L
#include "c2sql.h"
#include "handle_internal.h"
#include "sqlite_driver.h"
#include "query_builder.h"
#include "type_mapping.h"
#include "condition_ast.h"
#include "error_ctx.h"
#include "migration.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* SqlRDBStmt — opaque multi-row iterator (Task 10.3)                 */
/* ------------------------------------------------------------------ */

struct SqlRDBStmt {
    SqlRDBHandle       *handle;    /* back-reference for mutex in StmtNext/Free */
    void               *raw_stmt;  /* driver-level prepared+bound statement    */
    const SqlRDBSchema *schema;    /* column layout for reading rows            */
};

/* Compile-time SQLite3 version guard. CMake also checks at configure time,
 * but this #error catches cases where sqlite3.h is swapped out manually. */
#if SQLITE_VERSION_NUMBER < 3035000
#  error "libc2sql requires SQLite3 >= 3.35.0 (UPSERT + STRICT table support)"
#endif

/* ------------------------------------------------------------------ */
/* Default configuration values                                        */
/* ------------------------------------------------------------------ */

static const SqlRDBConfig DEFAULT_CONFIG = {
    .threadsafe       = true,
    .stmt_cache_size  = 64,
    .auto_migrate     = false,
    .multirow_default = 0,
    .require_strict   = false,
};

/* ------------------------------------------------------------------ */
/* Live handle registry (Req 1.5)                                      */
/*                                                                     */
/* After SqlRDBClose() frees the handle struct, the user's pointer is  */
/* dangling. Reading h->magic to detect a double-close is undefined    */
/* behavior (AddressSanitizer flags it as heap-use-after-free).        */
/*                                                                     */
/* The registry below tracks the set of currently-open handle pointers */
/* so that SqlRDBClose can recognize "this pointer is no longer ours"  */
/* without touching freed memory. Lookup is O(n) over a growable array */
/* — acceptable given the API contract that each process holds at most */
/* a handful of long-lived handles (UC-1, UC-6).                       */
/* ------------------------------------------------------------------ */

#include <pthread.h>

static pthread_mutex_t   g_live_mutex    = PTHREAD_MUTEX_INITIALIZER;
static SqlRDBHandle    **g_live_handles  = NULL;
static size_t            g_live_count    = 0;
static size_t            g_live_capacity = 0;

static bool live_register(SqlRDBHandle *h) {
    pthread_mutex_lock(&g_live_mutex);
    if (g_live_count == g_live_capacity) {
        size_t new_cap = g_live_capacity ? g_live_capacity * 2 : 8;
        SqlRDBHandle **p = (SqlRDBHandle **)realloc(
            (void *)g_live_handles, new_cap * sizeof(*p));
        if (!p) { pthread_mutex_unlock(&g_live_mutex); return false; }
        g_live_handles  = p;
        g_live_capacity = new_cap;
    }
    g_live_handles[g_live_count++] = h;
    pthread_mutex_unlock(&g_live_mutex);
    return true;
}

static bool live_remove(SqlRDBHandle *h) {
    pthread_mutex_lock(&g_live_mutex);
    for (size_t i = 0; i < g_live_count; i++) {
        if (g_live_handles[i] == h) {
            g_live_handles[i] = g_live_handles[--g_live_count];
            pthread_mutex_unlock(&g_live_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&g_live_mutex);
    return false;
}

static bool live_contains(const SqlRDBHandle *h) {
    if (h == NULL) return false;
    pthread_mutex_lock(&g_live_mutex);
    for (size_t i = 0; i < g_live_count; i++) {
        if (g_live_handles[i] == h) {
            pthread_mutex_unlock(&g_live_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&g_live_mutex);
    return false;
}

/* ------------------------------------------------------------------ */
/* Task 9.1: Lifecycle — SqlRDBInit / SqlRDBClose                     */
/* ------------------------------------------------------------------ */

SqlRDBHandle *SqlRDBInit(const char *connection_string) {
    if (connection_string == NULL) return NULL;

    const SqlRDBDriver *drv = &g_sqlite3_driver;

    void *driver_ctx = NULL;
    if (drv->open(connection_string, &driver_ctx) != SQL_RDB_OK) return NULL;

    SqlRDBHandle *h = calloc(1, sizeof(SqlRDBHandle));
    if (h == NULL) {
        drv->close(driver_ctx);
        return NULL;
    }

    h->config     = DEFAULT_CONFIG;
    h->driver     = drv;
    h->driver_ctx = driver_ctx;

    if (c2sql_internal_mutex_init(&h->mutex, h->config.threadsafe) != SQL_RDB_OK) {
        drv->close(driver_ctx);
        free(h);
        return NULL;
    }

    c2sql_internal_err_clear(&h->error);
    h->logger.fn   = NULL;
    h->logger.user = NULL;

    c2sql_internal_schema_registry_init(&h->registry);

    if (c2sql_internal_cache_init(&h->cache, h->config.stmt_cache_size) != SQL_RDB_OK) {
        c2sql_internal_schema_registry_destroy(&h->registry);
        c2sql_internal_mutex_destroy(&h->mutex);
        drv->close(driver_ctx);
        free(h);
        return NULL;
    }

    memset(&h->txn, 0, sizeof(h->txn));

    h->magic = SQL_RDB_HANDLE_MAGIC;

    if (!live_register(h)) {
        h->magic = SQL_RDB_HANDLE_DEAD;
        c2sql_internal_cache_destroy(&h->cache, drv, driver_ctx);
        c2sql_internal_schema_registry_destroy(&h->registry);
        c2sql_internal_mutex_destroy(&h->mutex);
        drv->close(driver_ctx);
        free(h);
        return NULL;
    }
    return h;
}

SqlRDBResult SqlRDBClose(SqlRDBHandle *h) {
    /* Req 1.5: double-close (or close on a non-handle pointer) must return
     * SQL_RDB_ERR_INVALID_HANDLE without dereferencing freed memory. The live
     * registry lookup is the authoritative check; the magic field is only a
     * fast-path sanity guard while the handle is alive. */
    if (!live_contains(h)) return SQL_RDB_ERR_INVALID_HANDLE;
    if (h->magic != SQL_RDB_HANDLE_MAGIC) return SQL_RDB_ERR_INVALID_HANDLE;

    /* Remove from the registry first so a concurrent caller cannot race and
     * see a half-torn-down handle as still alive. */
    live_remove(h);
    h->magic = SQL_RDB_HANDLE_DEAD;

    /* TX: roll back any active transaction (best effort). */
    if (h->txn.depth > 0 && h->driver != NULL && h->driver_ctx != NULL) {
        h->driver->rollback(h->driver_ctx);
        h->txn.depth = 0;
    }

    /* Cache → registry → driver → mutex → free. */
    c2sql_internal_cache_destroy(&h->cache, h->driver, h->driver_ctx);
    c2sql_internal_schema_registry_destroy(&h->registry);

    if (h->driver != NULL && h->driver_ctx != NULL) {
        h->driver->close(h->driver_ctx);
        h->driver_ctx = NULL;
    }

    c2sql_internal_mutex_destroy(&h->mutex);
    free(h);
    return SQL_RDB_OK;
}

/* ------------------------------------------------------------------ */
/* Task 9.2: SqlRDBSetConfig                                          */
/* ------------------------------------------------------------------ */

SqlRDBResult SqlRDBSetConfig(SqlRDBHandle *h, const SqlRDBConfig *cfg) {
    if (!c2sql_handle_valid(h)) return SQL_RDB_ERR_INVALID_HANDLE;
    if (cfg == NULL) return SQL_RDB_ERR_INVALID_ARG;

    c2sql_internal_mutex_lock(&h->mutex);

    SqlRDBResult r = SQL_RDB_OK;
    if (cfg->stmt_cache_size != h->config.stmt_cache_size) {
        r = c2sql_internal_cache_rebuild(&h->cache, cfg->stmt_cache_size,
                                         h->driver, h->driver_ctx);
    }

    if (r == SQL_RDB_OK) {
        bool old_threadsafe = h->config.threadsafe;
        h->config = *cfg;
        if (cfg->threadsafe != old_threadsafe) {
            h->mutex.threadsafe = cfg->threadsafe;
        }
    }

    c2sql_internal_mutex_unlock(&h->mutex);
    return r;
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

const char *SqlRDBLastError(const SqlRDBHandle *h, SqlRDBResult *out_code) {
    static const char empty[] = "";
    if (!c2sql_handle_valid(h)) {
        if (out_code) *out_code = SQL_RDB_OK;
        return empty;
    }
    if (out_code) *out_code = h->error.code;
    return h->error.message;
}

SqlRDBResult SqlRDBSetLogger(SqlRDBHandle *h, SqlRDBLoggerFn fn, void *user) {
    if (!c2sql_handle_valid(h)) return SQL_RDB_ERR_INVALID_HANDLE;
    c2sql_internal_mutex_lock(&h->mutex);
    h->logger.fn   = fn;
    h->logger.user = user;
    c2sql_internal_mutex_unlock(&h->mutex);
    return SQL_RDB_OK;
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

SqlRDBResult SqlRDBFreeResult(void *ptr) {
    free(ptr);
    return SQL_RDB_OK;
}

/* ------------------------------------------------------------------ */
/* Task 10: Internal helpers                                          */
/* ------------------------------------------------------------------ */

/*
 * Recursively bind condition leaf values to placeholders (1-based idx).
 * *idx must be initialised to 1 before the first call.
 */
static SqlRDBResult bind_cond_values(const SqlRDBDriver *drv, void *stmt,
                                      const SqlRDBCondition *cond, int *idx) {
    if (!cond || cond->kind == COND_ALL) return SQL_RDB_OK;

    switch (cond->kind) {
        case COND_LEAF: {
            int i = (*idx)++;
            switch (cond->u.leaf.value_type) {
                case SQL_TYPE_INT32:
                case SQL_TYPE_INT64:
                    return drv->bind_int64(stmt, i, cond->u.leaf.v.i);
                case SQL_TYPE_REAL:
                    return drv->bind_real(stmt, i, cond->u.leaf.v.r);
                case SQL_TYPE_TEXT:
                    return drv->bind_text(stmt, i, cond->u.leaf.v.t, -1);
                case SQL_TYPE_BLOB:
                    return drv->bind_blob(stmt, i,
                                          cond->u.leaf.v.b.p, cond->u.leaf.v.b.n);
            }
            return SQL_RDB_OK;
        }
        case COND_AND:
        case COND_OR: {
            SqlRDBResult r = bind_cond_values(drv, stmt,
                                               cond->u.composite.left, idx);
            if (r != SQL_RDB_OK) return r;
            return bind_cond_values(drv, stmt, cond->u.composite.right, idx);
        }
        case COND_ALL:
            return SQL_RDB_OK;
    }
    return SQL_RDB_OK;
}

/*
 * Fetch a prepared statement from the cache, or prepare a fresh one.
 *
 * On success:
 *   - *out_stmt is valid and ready for binding.
 *   - *out_owned == true  → caller must call driver->finalize after use.
 *   - *out_owned == false → caller must call driver->reset  after use.
 *
 * On failure: *out_stmt is NULL (already cleaned up).
 */
static SqlRDBResult get_or_prepare(SqlRDBHandle *h, const char *sql,
                                    void **out_stmt, bool *out_owned) {
    *out_stmt  = NULL;
    *out_owned = false;

    /* Try cache first */
    SqlRDBResult r = c2sql_internal_cache_get(&h->cache, sql, out_stmt);
    if (r == SQL_RDB_OK) return SQL_RDB_OK;

    /* Prepare fresh */
    r = h->driver->prepare(h->driver_ctx, sql, out_stmt);
    if (r != SQL_RDB_OK) return r;

    if (h->cache.capacity == 0) {
        /* Cache disabled: caller owns and must finalize */
        *out_owned = true;
        return SQL_RDB_OK;
    }

    /* Try to cache the newly prepared statement.
     * On failure, cache_put finalizes *out_stmt internally. */
    r = c2sql_internal_cache_put(&h->cache, sql, *out_stmt, h->driver, h->driver_ctx);
    if (r != SQL_RDB_OK) {
        *out_stmt = NULL;
        return r;
    }

    /* Re-acquire from cache (same pointer, but goes through the lookup) */
    r = c2sql_internal_cache_get(&h->cache, sql, out_stmt);
    if (r != SQL_RDB_OK) {
        /* Should not happen; guard against internal inconsistency */
        *out_stmt = NULL;
        return SQL_RDB_ERR_INTERNAL;
    }
    return SQL_RDB_OK;
}

/* Release a statement obtained via get_or_prepare. */
static void release_cached_stmt(SqlRDBHandle *h, void *stmt, bool owned) {
    if (owned) {
        h->driver->finalize(stmt);
    } else {
        h->driver->reset(stmt);
    }
}

/* ------------------------------------------------------------------ */
/* Task 10.1: SqlRDBRegisterStruct                                     */
/* ------------------------------------------------------------------ */

SqlRDBResult SqlRDBRegisterStruct(SqlRDBHandle *h, const char *struct_name,
                                   const SqlRDBColumnDef *cols, size_t col_count) {
    if (!c2sql_handle_valid(h)) return SQL_RDB_ERR_INVALID_HANDLE;
    if (!struct_name || !cols || col_count == 0) {
        if (c2sql_handle_valid(h)) {
            c2sql_internal_err_set(&h->error, SQL_RDB_ERR_INVALID_ARG,
                                   "RegisterStruct: struct_name, cols, col_count required");
        }
        return SQL_RDB_ERR_INVALID_ARG;
    }

    c2sql_internal_mutex_lock(&h->mutex);

    /* Validate & deep-copy into the registry */
    SqlRDBResult r = c2sql_internal_schema_register(h, struct_name, cols, col_count);
    if (r != SQL_RDB_OK) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    /* Retrieve the registered schema */
    const SqlRDBSchema *schema = c2sql_internal_schema_lookup(h, struct_name);
    if (!schema) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_INTERNAL;
    }

    /* Issue CREATE TABLE, validate against any existing table, optionally
     * auto-migrate missing trailing columns. On failure, roll back the
     * in-memory registration so the user may retry with corrected schema. */
    r = c2sql_internal_migrate_table(h, schema);
    if (r != SQL_RDB_OK) {
        c2sql_internal_schema_unregister(h, struct_name);
    }

    c2sql_internal_mutex_unlock(&h->mutex);
    return r;
}

/* ------------------------------------------------------------------ */
/* Task 10.2: SqlRDBWrite / SqlRDBWriteMany                            */
/* ------------------------------------------------------------------ */

SqlRDBResult SqlRDBWrite(SqlRDBHandle *h, const char *struct_name,
                          const void *row, const uint8_t *null_map) {
    if (!c2sql_handle_valid(h)) return SQL_RDB_ERR_INVALID_HANDLE;
    if (!struct_name)           return SQL_RDB_ERR_INVALID_ARG;
    if (!row)                   return SQL_RDB_ERR_INVALID_ARG;

    c2sql_internal_mutex_lock(&h->mutex);

    const SqlRDBSchema *schema = c2sql_internal_schema_lookup(h, struct_name);
    if (!schema) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_UNKNOWN_STRUCT,
                               "Write: unknown struct '%s'", struct_name);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_UNKNOWN_STRUCT;
    }

    C2SqlQBOp op = (schema->pk_index >= 0) ? C2SQL_QB_UPSERT : C2SQL_QB_INSERT;

    char *sql = NULL;
    SqlRDBQuerySpec spec = { .op = op, .schema = schema, .cond = NULL, .new_col = NULL };
    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, NULL);
    if (r != SQL_RDB_OK) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    void *stmt  = NULL;
    bool  owned = false;
    r = get_or_prepare(h, sql, &stmt, &owned);
    free(sql);
    if (r != SQL_RDB_OK) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    r = c2sql_internal_tm_bind_row(h->driver, stmt, schema, row, null_map);
    if (r != SQL_RDB_OK) {
        release_cached_stmt(h, stmt, owned);
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    bool has_row;
    r = h->driver->step(stmt, &has_row);
    release_cached_stmt(h, stmt, owned);

    if (r != SQL_RDB_OK) {
        c2sql_internal_err_set(&h->error, r, "Write: step failed");
    }

    c2sql_internal_mutex_unlock(&h->mutex);
    return r;
}

SqlRDBResult SqlRDBWriteMany(SqlRDBHandle *h, const char *struct_name,
                              const void *rows, size_t count, size_t stride,
                              const uint8_t *null_maps) {
    if (!c2sql_handle_valid(h))              return SQL_RDB_ERR_INVALID_HANDLE;
    if (!struct_name || !rows)               return SQL_RDB_ERR_INVALID_ARG;
    if (count == 0 || stride == 0)           return SQL_RDB_ERR_INVALID_ARG;

    c2sql_internal_mutex_lock(&h->mutex);

    const SqlRDBSchema *schema = c2sql_internal_schema_lookup(h, struct_name);
    if (!schema) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_UNKNOWN_STRUCT,
                               "WriteMany: unknown struct '%s'", struct_name);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_UNKNOWN_STRUCT;
    }

    C2SqlQBOp op = (schema->pk_index >= 0) ? C2SQL_QB_UPSERT : C2SQL_QB_INSERT;

    char *sql = NULL;
    SqlRDBQuerySpec spec = { .op = op, .schema = schema, .cond = NULL, .new_col = NULL };
    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, NULL);
    if (r != SQL_RDB_OK) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    void *stmt  = NULL;
    bool  owned = false;
    r = get_or_prepare(h, sql, &stmt, &owned);
    free(sql);
    if (r != SQL_RDB_OK) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    /* Wrap all rows in an implicit transaction unless one is already active */
    bool implicit_tx = (h->txn.depth == 0);
    if (implicit_tx) {
        SqlRDBResult br = h->driver->begin(h->driver_ctx);
        if (br != SQL_RDB_OK) {
            release_cached_stmt(h, stmt, owned);
            c2sql_internal_mutex_unlock(&h->mutex);
            return br;
        }
    }

    size_t bitmap_bytes = SQL_RDB_NULL_BITMAP_BYTES(schema->col_count);

    for (size_t i = 0; i < count; i++) {
        const void    *row = (const char *)rows + i * stride;
        const uint8_t *nm  = null_maps ? (null_maps + i * bitmap_bytes) : NULL;

        r = c2sql_internal_tm_bind_row(h->driver, stmt, schema, row, nm);
        if (r != SQL_RDB_OK) break;

        bool has_row;
        r = h->driver->step(stmt, &has_row);
        h->driver->reset(stmt);  /* reset after each step so next bind is clean */
        if (r != SQL_RDB_OK) break;
    }

    if (implicit_tx) {
        if (r == SQL_RDB_OK) {
            h->driver->commit(h->driver_ctx);
        } else {
            h->driver->rollback(h->driver_ctx);
        }
    }

    release_cached_stmt(h, stmt, owned);
    c2sql_internal_mutex_unlock(&h->mutex);
    return r;
}

/* ------------------------------------------------------------------ */
/* Task 10.3: SqlRDBRead / SqlRDBReadMany / StmtNext / StmtFree        */
/* ------------------------------------------------------------------ */

SqlRDBResult SqlRDBRead(SqlRDBHandle *h, const char *struct_name,
                         const SqlRDBCondition *cond, void *out_row,
                         uint8_t *out_null_map, const SqlRDBReadOpts *opts) {
    if (!c2sql_handle_valid(h)) return SQL_RDB_ERR_INVALID_HANDLE;
    if (!struct_name)           return SQL_RDB_ERR_INVALID_ARG;
    if (!out_row)               return SQL_RDB_ERR_INVALID_ARG;

    c2sql_internal_mutex_lock(&h->mutex);

    const SqlRDBSchema *schema = c2sql_internal_schema_lookup(h, struct_name);
    if (!schema) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_UNKNOWN_STRUCT,
                               "Read: unknown struct '%s'", struct_name);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_UNKNOWN_STRUCT;
    }

    bool allow_multi = (opts && opts->allow_multi) || (h->config.multirow_default == 1);

    char *sql = NULL;
    SqlRDBQuerySpec spec = { .op = C2SQL_QB_SELECT, .schema = schema,
                              .cond = cond, .new_col = NULL };
    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, NULL);
    if (r != SQL_RDB_OK) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    void *stmt  = NULL;
    bool  owned = false;
    r = get_or_prepare(h, sql, &stmt, &owned);
    free(sql);
    if (r != SQL_RDB_OK) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    /* Bind WHERE clause parameters */
    if (cond && cond->kind != COND_ALL) {
        int idx = 1;
        r = bind_cond_values(h->driver, stmt, cond, &idx);
        if (r != SQL_RDB_OK) {
            release_cached_stmt(h, stmt, owned);
            c2sql_internal_mutex_unlock(&h->mutex);
            return r;
        }
    }

    /* Step to first row */
    bool has_row = false;
    r = h->driver->step(stmt, &has_row);
    if (r != SQL_RDB_OK) {
        release_cached_stmt(h, stmt, owned);
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }
    if (!has_row) {
        release_cached_stmt(h, stmt, owned);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_NOT_FOUND;
    }

    if (allow_multi) {
        /* Read first row directly into out_row */
        r = c2sql_internal_tm_read_row(h->driver, stmt, schema, out_row, out_null_map);
        release_cached_stmt(h, stmt, owned);
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    /* Must not modify out_row/out_null_map until we confirm single-row result.
     * Read first row into a temporary buffer. */
    size_t buf_size = 0;
    for (size_t i = 0; i < schema->col_count; i++) {
        size_t end = schema->cols[i].offset + schema->cols[i].size;
        if (end > buf_size) buf_size = end;
    }

    /* malloc(0) is implementation-defined; guarantee a non-NULL allocation. */
    void *tmp_row = malloc(buf_size ? buf_size : 1);
    if (!tmp_row) {
        release_cached_stmt(h, stmt, owned);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_NO_MEMORY;
    }
    memset(tmp_row, 0, buf_size);

    /* schema->col_count > 0 is guaranteed by registration, but guard against
     * calloc(1, 0) being implementation-defined for the analyzer's benefit. */
    size_t   nm_bytes = SQL_RDB_NULL_BITMAP_BYTES(schema->col_count);
    uint8_t *tmp_null = out_null_map ? calloc(1, nm_bytes ? nm_bytes : 1) : NULL;

    SqlRDBResult read_rc = c2sql_internal_tm_read_row(h->driver, stmt, schema,
                                                       tmp_row, tmp_null);
    if (read_rc != SQL_RDB_OK && read_rc != SQL_RDB_WARN_TRUNCATED) {
        free(tmp_row);
        free(tmp_null);
        release_cached_stmt(h, stmt, owned);
        c2sql_internal_mutex_unlock(&h->mutex);
        return read_rc;
    }

    /* Check for a second row */
    bool has_row2 = false;
    r = h->driver->step(stmt, &has_row2);
    release_cached_stmt(h, stmt, owned);

    if (r != SQL_RDB_OK || has_row2) {
        free(tmp_row);
        free(tmp_null);
        c2sql_internal_mutex_unlock(&h->mutex);
        return has_row2 ? SQL_RDB_ERR_MULTIPLE_ROWS : r;
    }

    /* Single result: copy to caller's buffers */
    memcpy(out_row, tmp_row, buf_size);
    if (out_null_map && tmp_null) {
        for (size_t i = 0; i < nm_bytes; i++) {
            out_null_map[i] |= tmp_null[i];
        }
    }
    free(tmp_row);
    free(tmp_null);

    c2sql_internal_mutex_unlock(&h->mutex);
    return read_rc;  /* may be WARN_TRUNCATED */
}

SqlRDBResult SqlRDBReadMany(SqlRDBHandle *h, const char *struct_name,
                             const SqlRDBCondition *cond, SqlRDBStmt **out_stmt) {
    if (!c2sql_handle_valid(h)) return SQL_RDB_ERR_INVALID_HANDLE;
    if (!struct_name)           return SQL_RDB_ERR_INVALID_ARG;
    if (!out_stmt)              return SQL_RDB_ERR_INVALID_ARG;

    c2sql_internal_mutex_lock(&h->mutex);

    const SqlRDBSchema *schema = c2sql_internal_schema_lookup(h, struct_name);
    if (!schema) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_UNKNOWN_STRUCT,
                               "ReadMany: unknown struct '%s'", struct_name);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_UNKNOWN_STRUCT;
    }

    char *sql = NULL;
    SqlRDBQuerySpec spec = { .op = C2SQL_QB_SELECT, .schema = schema,
                              .cond = cond, .new_col = NULL };
    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, NULL);
    if (r != SQL_RDB_OK) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    /* ReadMany needs an uncached statement (cursor stays open across StmtNext calls) */
    void *raw = NULL;
    r = h->driver->prepare(h->driver_ctx, sql, &raw);
    free(sql);
    if (r != SQL_RDB_OK) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    /* Bind condition values */
    if (cond && cond->kind != COND_ALL) {
        int idx = 1;
        r = bind_cond_values(h->driver, raw, cond, &idx);
        if (r != SQL_RDB_OK) {
            h->driver->finalize(raw);
            c2sql_internal_mutex_unlock(&h->mutex);
            return r;
        }
    }

    SqlRDBStmt *iter = malloc(sizeof(SqlRDBStmt));
    if (!iter) {
        h->driver->finalize(raw);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_NO_MEMORY;
    }

    iter->handle   = h;
    iter->raw_stmt = raw;
    iter->schema   = schema;
    *out_stmt = iter;

    c2sql_internal_mutex_unlock(&h->mutex);
    return SQL_RDB_OK;
}

SqlRDBResult SqlRDBStmtNext(SqlRDBStmt *stmt, void *out_row, uint8_t *out_null_map) {
    if (!stmt)    return SQL_RDB_ERR_INVALID_ARG;
    if (!out_row) return SQL_RDB_ERR_INVALID_ARG;

    SqlRDBHandle *h = stmt->handle;
    c2sql_internal_mutex_lock(&h->mutex);

    bool has_row = false;
    SqlRDBResult r = h->driver->step(stmt->raw_stmt, &has_row);
    if (r != SQL_RDB_OK) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }
    if (!has_row) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_NOT_FOUND;
    }

    r = c2sql_internal_tm_read_row(h->driver, stmt->raw_stmt, stmt->schema,
                                   out_row, out_null_map);
    c2sql_internal_mutex_unlock(&h->mutex);
    return r;
}

SqlRDBResult SqlRDBStmtFree(SqlRDBStmt *stmt) {
    if (!stmt) return SQL_RDB_ERR_INVALID_ARG;

    SqlRDBHandle *h = stmt->handle;
    c2sql_internal_mutex_lock(&h->mutex);
    h->driver->finalize(stmt->raw_stmt);
    c2sql_internal_mutex_unlock(&h->mutex);

    free(stmt);
    return SQL_RDB_OK;
}

/* ------------------------------------------------------------------ */
/* Task 10.4: SqlRDBDelete                                            */
/* ------------------------------------------------------------------ */

SqlRDBResult SqlRDBDelete(SqlRDBHandle *h, const char *struct_name,
                           const SqlRDBCondition *cond, size_t *out_deleted) {
    if (!c2sql_handle_valid(h)) return SQL_RDB_ERR_INVALID_HANDLE;
    if (!struct_name)           return SQL_RDB_ERR_INVALID_ARG;
    if (!cond)                  return SQL_RDB_ERR_INVALID_ARG; /* guard full-table delete */

    c2sql_internal_mutex_lock(&h->mutex);

    const SqlRDBSchema *schema = c2sql_internal_schema_lookup(h, struct_name);
    if (!schema) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_UNKNOWN_STRUCT,
                               "Delete: unknown struct '%s'", struct_name);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_UNKNOWN_STRUCT;
    }

    char *sql = NULL;
    SqlRDBQuerySpec spec = { .op = C2SQL_QB_DELETE, .schema = schema,
                              .cond = cond, .new_col = NULL };
    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, NULL);
    if (r != SQL_RDB_OK) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    void *stmt  = NULL;
    bool  owned = false;
    r = get_or_prepare(h, sql, &stmt, &owned);
    free(sql);
    if (r != SQL_RDB_OK) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    /* Bind condition values */
    if (cond->kind != COND_ALL) {
        int idx = 1;
        r = bind_cond_values(h->driver, stmt, cond, &idx);
        if (r != SQL_RDB_OK) {
            release_cached_stmt(h, stmt, owned);
            c2sql_internal_mutex_unlock(&h->mutex);
            return r;
        }
    }

    bool has_row;
    r = h->driver->step(stmt, &has_row);
    release_cached_stmt(h, stmt, owned);

    if (r == SQL_RDB_OK && out_deleted) {
        int n = h->driver->changes(h->driver_ctx);
        *out_deleted = (n >= 0) ? (size_t)n : 0;
    }

    c2sql_internal_mutex_unlock(&h->mutex);
    return r;
}

/* ------------------------------------------------------------------ */
/* SqlRDBCount — read-only COUNT(*) with optional condition            */
/* ------------------------------------------------------------------ */

SqlRDBResult SqlRDBCount(SqlRDBHandle *h, const char *struct_name,
                          const SqlRDBCondition *cond, size_t *out_count) {
    if (!c2sql_handle_valid(h)) return SQL_RDB_ERR_INVALID_HANDLE;
    if (!struct_name || !out_count) return SQL_RDB_ERR_INVALID_ARG;
    /* cond may be NULL: counting is non-destructive, so NULL == count all. */

    c2sql_internal_mutex_lock(&h->mutex);

    const SqlRDBSchema *schema = c2sql_internal_schema_lookup(h, struct_name);
    if (!schema) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_UNKNOWN_STRUCT,
                               "Count: unknown struct '%s'", struct_name);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_UNKNOWN_STRUCT;
    }

    char *sql = NULL;
    SqlRDBQuerySpec spec = { .op = C2SQL_QB_COUNT, .schema = schema,
                              .cond = cond, .new_col = NULL };
    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, NULL);
    if (r != SQL_RDB_OK) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    void *stmt  = NULL;
    bool  owned = false;
    r = get_or_prepare(h, sql, &stmt, &owned);
    free(sql);
    if (r != SQL_RDB_OK) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    if (cond && cond->kind != COND_ALL) {
        int idx = 1;
        r = bind_cond_values(h->driver, stmt, cond, &idx);
        if (r != SQL_RDB_OK) {
            release_cached_stmt(h, stmt, owned);
            c2sql_internal_mutex_unlock(&h->mutex);
            return r;
        }
    }

    bool has_row = false;
    r = h->driver->step(stmt, &has_row);
    if (r == SQL_RDB_OK && has_row) {
        int64_t n = 0;
        r = h->driver->column_int64(stmt, 0, &n);
        if (r == SQL_RDB_OK) *out_count = (n >= 0) ? (size_t)n : 0;
    }
    release_cached_stmt(h, stmt, owned);

    c2sql_internal_mutex_unlock(&h->mutex);
    return r;
}

/* ------------------------------------------------------------------ */
/* Task 11.1: Transaction control                                      */
/*                                                                     */
/* State model (Req 10.1〜10.6):                                       */
/*   depth == 0   → no active TX                                       */
/*   depth == 1   → BEGIN / COMMIT / ROLLBACK                          */
/*   depth >= 2   → SAVEPOINT / RELEASE / ROLLBACK TO  sp_<depth>      */
/* Stack slot N (1-based depth) stores the SAVEPOINT name used at      */
/* that depth; index 0 is unused for symmetry with the depth counter.  */
/* ------------------------------------------------------------------ */

SqlRDBResult SqlRDBBeginTx(SqlRDBHandle *h) {
    if (!c2sql_handle_valid(h)) return SQL_RDB_ERR_INVALID_HANDLE;

    c2sql_internal_mutex_lock(&h->mutex);

    if (h->txn.depth >= C2SQL_MAX_TX_DEPTH) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_NESTED_TX,
                               "BeginTx: nesting depth limit %d reached",
                               C2SQL_MAX_TX_DEPTH);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_NESTED_TX;
    }

    SqlRDBResult r;
    if (h->txn.depth == 0) {
        r = h->driver->begin(h->driver_ctx);
    } else {
        int new_depth = h->txn.depth + 1;
        snprintf(h->txn.sp_names[new_depth - 1], C2SQL_SP_NAME_LEN,
                 "sp_%d", new_depth);
        r = h->driver->savepoint(h->driver_ctx, h->txn.sp_names[new_depth - 1]);
    }

    if (r != SQL_RDB_OK) {
        c2sql_internal_err_set(&h->error, r, "BeginTx: driver call failed");
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    h->txn.depth++;
    c2sql_internal_mutex_unlock(&h->mutex);
    return SQL_RDB_OK;
}

SqlRDBResult SqlRDBCommitTx(SqlRDBHandle *h) {
    if (!c2sql_handle_valid(h)) return SQL_RDB_ERR_INVALID_HANDLE;

    c2sql_internal_mutex_lock(&h->mutex);

    if (h->txn.depth == 0) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_NO_ACTIVE_TX;
    }

    SqlRDBResult r;
    if (h->txn.depth == 1) {
        r = h->driver->commit(h->driver_ctx);
    } else {
        r = h->driver->release_sp(h->driver_ctx,
                                   h->txn.sp_names[h->txn.depth - 1]);
    }

    if (r != SQL_RDB_OK) {
        c2sql_internal_err_set(&h->error, r, "CommitTx: driver call failed");
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    h->txn.depth--;
    c2sql_internal_mutex_unlock(&h->mutex);
    return SQL_RDB_OK;
}

SqlRDBResult SqlRDBRollbackTx(SqlRDBHandle *h) {
    if (!c2sql_handle_valid(h)) return SQL_RDB_ERR_INVALID_HANDLE;

    c2sql_internal_mutex_lock(&h->mutex);

    if (h->txn.depth == 0) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_NO_ACTIVE_TX;
    }

    SqlRDBResult r;
    if (h->txn.depth == 1) {
        r = h->driver->rollback(h->driver_ctx);
    } else {
        /* ROLLBACK TO SAVEPOINT rewinds the savepoint without releasing it;
         * pair with RELEASE so the frame is fully unwound. */
        const char *name = h->txn.sp_names[h->txn.depth - 1];
        r = h->driver->rollback_sp(h->driver_ctx, name);
        if (r == SQL_RDB_OK) {
            r = h->driver->release_sp(h->driver_ctx, name);
        }
    }

    if (r != SQL_RDB_OK) {
        c2sql_internal_err_set(&h->error, r, "RollbackTx: driver call failed");
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    h->txn.depth--;
    c2sql_internal_mutex_unlock(&h->mutex);
    return SQL_RDB_OK;
}

/* ------------------------------------------------------------------ */
/* Task 12.1: Variable-length BLOB field auxiliary APIs                */
/*                                                                     */
/* Row-identity contract (design.md, Req 8.5 / 5.2):                   */
/*   - 0 matching rows → SQL_RDB_ERR_NOT_FOUND, no write performed     */
/*   - 2+ matching rows → SQL_RDB_ERR_MULTIPLE_ROWS, no write performed*/
/*   - Read failure paths must NOT modify *out_bytes / *out_len.       */
/* ------------------------------------------------------------------ */

/* Return column index for col_name in schema, or -1 if absent. */
static int find_col_index(const SqlRDBSchema *schema, const char *col_name) {
    for (size_t i = 0; i < schema->col_count; i++) {
        if (strcmp(schema->cols[i].name, col_name) == 0) return (int)i;
    }
    return -1;
}

/*
 * Count how many rows match `cond` in `schema`'s table.
 * Returns SQL_RDB_OK and writes 0/1/2 into *out_n (2 means "two or more").
 * Uses an uncached prepared statement (one-shot lookup).
 */
static SqlRDBResult count_matching_rows(SqlRDBHandle *h,
                                         const SqlRDBSchema *schema,
                                         const char *target_col,
                                         const SqlRDBCondition *cond,
                                         int *out_n) {
    *out_n = 0;
    char *sql = NULL;
    SqlRDBQuerySpec spec = {
        .op         = C2SQL_QB_SELECT_FIELD,
        .schema     = schema,
        .cond       = cond,
        .new_col    = NULL,
        .target_col = target_col,
    };
    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, NULL);
    if (r != SQL_RDB_OK) return r;

    void *stmt = NULL;
    r = h->driver->prepare(h->driver_ctx, sql, &stmt);
    free(sql);
    if (r != SQL_RDB_OK) return r;

    if (cond && cond->kind != COND_ALL) {
        int idx = 1;
        r = bind_cond_values(h->driver, stmt, cond, &idx);
        if (r != SQL_RDB_OK) { h->driver->finalize(stmt); return r; }
    }

    bool has_row = false;
    r = h->driver->step(stmt, &has_row);
    if (r != SQL_RDB_OK) { h->driver->finalize(stmt); return r; }
    if (has_row) {
        *out_n = 1;
        bool has_row2 = false;
        r = h->driver->step(stmt, &has_row2);
        if (r != SQL_RDB_OK) { h->driver->finalize(stmt); return r; }
        if (has_row2) *out_n = 2;
    }
    h->driver->finalize(stmt);
    return SQL_RDB_OK;
}

SqlRDBResult SqlRDBWriteBlobField(SqlRDBHandle *h, const char *struct_name,
                                   const SqlRDBCondition *key, const char *col_name,
                                   const void *bytes, size_t len) {
    if (!c2sql_handle_valid(h))                  return SQL_RDB_ERR_INVALID_HANDLE;
    if (!struct_name || !key || !col_name || !bytes)
                                                 return SQL_RDB_ERR_INVALID_ARG;

    c2sql_internal_mutex_lock(&h->mutex);

    const SqlRDBSchema *schema = c2sql_internal_schema_lookup(h, struct_name);
    if (!schema) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_UNKNOWN_STRUCT,
                               "WriteBlobField: unknown struct '%s'", struct_name);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_UNKNOWN_STRUCT;
    }

    int col_idx = find_col_index(schema, col_name);
    if (col_idx < 0) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_UNKNOWN_COLUMN;
    }
    if (schema->cols[col_idx].type != SQL_TYPE_BLOB) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_INVALID_ARG,
                               "WriteBlobField: column '%s' is not BLOB", col_name);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_INVALID_ARG;
    }

    /* Pre-flight: ensure exactly one row matches before issuing UPDATE. */
    int matches = 0;
    SqlRDBResult r = count_matching_rows(h, schema, col_name, key, &matches);
    if (r != SQL_RDB_OK) { c2sql_internal_mutex_unlock(&h->mutex); return r; }
    if (matches == 0) { c2sql_internal_mutex_unlock(&h->mutex); return SQL_RDB_ERR_NOT_FOUND; }
    if (matches >= 2) { c2sql_internal_mutex_unlock(&h->mutex); return SQL_RDB_ERR_MULTIPLE_ROWS; }

    /* Issue UPDATE table SET col=? WHERE <cond> */
    char *sql = NULL;
    SqlRDBQuerySpec spec = {
        .op         = C2SQL_QB_UPDATE_FIELD,
        .schema     = schema,
        .cond       = key,
        .new_col    = NULL,
        .target_col = col_name,
    };
    r = c2sql_internal_qb_build(&spec, &sql, NULL);
    if (r != SQL_RDB_OK) { c2sql_internal_mutex_unlock(&h->mutex); return r; }

    void *stmt  = NULL;
    bool  owned = false;
    r = get_or_prepare(h, sql, &stmt, &owned);
    free(sql);
    if (r != SQL_RDB_OK) { c2sql_internal_mutex_unlock(&h->mutex); return r; }

    /* Bind 1: the BLOB value */
    r = h->driver->bind_blob(stmt, 1, bytes, len);
    if (r != SQL_RDB_OK) {
        release_cached_stmt(h, stmt, owned);
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    /* Bind 2..N: condition values */
    if (key->kind != COND_ALL) {
        int idx = 2;
        r = bind_cond_values(h->driver, stmt, key, &idx);
        if (r != SQL_RDB_OK) {
            release_cached_stmt(h, stmt, owned);
            c2sql_internal_mutex_unlock(&h->mutex);
            return r;
        }
    }

    bool has_row;
    r = h->driver->step(stmt, &has_row);
    release_cached_stmt(h, stmt, owned);

    if (r != SQL_RDB_OK) {
        c2sql_internal_err_set(&h->error, r, "WriteBlobField: step failed");
    }
    c2sql_internal_mutex_unlock(&h->mutex);
    return r;
}

SqlRDBResult SqlRDBReadBlobField(SqlRDBHandle *h, const char *struct_name,
                                  const SqlRDBCondition *key, const char *col_name,
                                  void **out_bytes, size_t *out_len) {
    if (!c2sql_handle_valid(h))                  return SQL_RDB_ERR_INVALID_HANDLE;
    if (!struct_name || !key || !col_name || !out_bytes || !out_len)
                                                 return SQL_RDB_ERR_INVALID_ARG;

    c2sql_internal_mutex_lock(&h->mutex);

    const SqlRDBSchema *schema = c2sql_internal_schema_lookup(h, struct_name);
    if (!schema) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_UNKNOWN_STRUCT,
                               "ReadBlobField: unknown struct '%s'", struct_name);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_UNKNOWN_STRUCT;
    }

    int col_idx = find_col_index(schema, col_name);
    if (col_idx < 0) {
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_UNKNOWN_COLUMN;
    }
    if (schema->cols[col_idx].type != SQL_TYPE_BLOB) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_INVALID_ARG,
                               "ReadBlobField: column '%s' is not BLOB", col_name);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_INVALID_ARG;
    }

    char *sql = NULL;
    SqlRDBQuerySpec spec = {
        .op         = C2SQL_QB_SELECT_FIELD,
        .schema     = schema,
        .cond       = key,
        .new_col    = NULL,
        .target_col = col_name,
    };
    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, NULL);
    if (r != SQL_RDB_OK) { c2sql_internal_mutex_unlock(&h->mutex); return r; }

    /* Use an uncached statement: column_blob's pointer is invalidated on reset. */
    void *stmt = NULL;
    r = h->driver->prepare(h->driver_ctx, sql, &stmt);
    free(sql);
    if (r != SQL_RDB_OK) { c2sql_internal_mutex_unlock(&h->mutex); return r; }

    if (key->kind != COND_ALL) {
        int idx = 1;
        r = bind_cond_values(h->driver, stmt, key, &idx);
        if (r != SQL_RDB_OK) {
            h->driver->finalize(stmt);
            c2sql_internal_mutex_unlock(&h->mutex);
            return r;
        }
    }

    bool has_row = false;
    r = h->driver->step(stmt, &has_row);
    if (r != SQL_RDB_OK) {
        h->driver->finalize(stmt);
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }
    if (!has_row) {
        h->driver->finalize(stmt);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_NOT_FOUND;   /* out_bytes/out_len untouched */
    }

    /* Copy column 0's BLOB into a malloc'd buffer (caller owns; freed via SqlRDBFreeResult). */
    const void *raw = NULL;
    size_t      raw_len = 0;
    r = h->driver->column_blob(stmt, 0, &raw, &raw_len);
    if (r != SQL_RDB_OK) {
        h->driver->finalize(stmt);
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }

    /* malloc(0) is implementation-defined; use a 1-byte allocation as a portable
     * non-NULL sentinel so callers can SqlRDBFreeResult() unconditionally. */
    void *copy = malloc(raw_len > 0 ? raw_len : 1);
    if (!copy) {
        h->driver->finalize(stmt);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_NO_MEMORY;
    }
    if (raw_len > 0 && raw != NULL) memcpy(copy, raw, raw_len);

    /* Confirm single-row result before publishing the buffer to the caller. */
    bool has_row2 = false;
    r = h->driver->step(stmt, &has_row2);
    h->driver->finalize(stmt);

    if (r != SQL_RDB_OK) {
        free(copy);
        c2sql_internal_mutex_unlock(&h->mutex);
        return r;
    }
    if (has_row2) {
        free(copy);
        c2sql_internal_mutex_unlock(&h->mutex);
        return SQL_RDB_ERR_MULTIPLE_ROWS;  /* out_bytes/out_len untouched */
    }

    *out_bytes = copy;
    *out_len   = raw_len;
    c2sql_internal_mutex_unlock(&h->mutex);
    return SQL_RDB_OK;
}
