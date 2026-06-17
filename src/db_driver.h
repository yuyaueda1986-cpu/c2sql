/*
 * db_driver.h — Internal driver vtable for libc2sql.
 *
 * Error translation convention:
 *   All driver functions translate driver-specific error codes to SqlRDBResult.
 *   Errors without a specific mapping are returned as SQL_RDB_ERR_DRIVER.
 *
 * Index conventions (matches SQLite3 native):
 *   bind_*:   1-based parameter index
 *   column_*: 0-based column index
 */
#ifndef C2SQL_DB_DRIVER_H
#define C2SQL_DB_DRIVER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "c2sql.h"

typedef struct SqlRDBDriver {
    const char *name;

    SqlRDBResult (*open)        (const char *dsn, void **out_ctx);
    SqlRDBResult (*close)       (void *ctx);
    SqlRDBResult (*exec)        (void *ctx, const char *sql);
    SqlRDBResult (*prepare)     (void *ctx, const char *sql, void **out_stmt);
    SqlRDBResult (*bind_int64)  (void *stmt, int index, int64_t value);
    SqlRDBResult (*bind_int32)  (void *stmt, int index, int32_t value);
    SqlRDBResult (*bind_real)   (void *stmt, int index, double value);
    SqlRDBResult (*bind_text)   (void *stmt, int index, const char *value, int len);
    SqlRDBResult (*bind_blob)   (void *stmt, int index, const void *value, size_t len);
    SqlRDBResult (*bind_null)   (void *stmt, int index);
    SqlRDBResult (*step)        (void *stmt, bool *out_has_row);
    SqlRDBResult (*column_int64)(void *stmt, int index, int64_t *out);
    SqlRDBResult (*column_int32)(void *stmt, int index, int32_t *out);
    SqlRDBResult (*column_real) (void *stmt, int index, double *out);
    SqlRDBResult (*column_text) (void *stmt, int index, const char **out_ptr, size_t *out_len);
    SqlRDBResult (*column_blob) (void *stmt, int index, const void **out_ptr, size_t *out_len);
    SqlRDBResult (*column_isnull)(void *stmt, int index, bool *out);
    SqlRDBResult (*reset)       (void *stmt);
    SqlRDBResult (*finalize)    (void *stmt);
    SqlRDBResult (*begin)       (void *ctx);
    SqlRDBResult (*commit)      (void *ctx);
    SqlRDBResult (*rollback)    (void *ctx);
    SqlRDBResult (*savepoint)   (void *ctx, const char *name);
    SqlRDBResult (*release_sp)  (void *ctx, const char *name);
    SqlRDBResult (*rollback_sp) (void *ctx, const char *name);
    int          (*changes)     (void *ctx);
} SqlRDBDriver;

_Static_assert(sizeof(SqlRDBDriver) > 0, "SqlRDBDriver must be non-empty");

#endif /* C2SQL_DB_DRIVER_H */
