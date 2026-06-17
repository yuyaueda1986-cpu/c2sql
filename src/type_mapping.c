/*
 * type_mapping.c — Bind/read helpers between C structs and driver statements.
 */
#include "type_mapping.h"

#include <string.h>
#include <limits.h>

/* POSIX strnlen is not in C11; provide a portable bounded variant. */
static size_t bounded_strlen(const char *s, size_t maxlen) {
    size_t n = 0;
    while (n < maxlen && s[n] != '\0') n++;
    return n;
}

static SqlRDBResult bind_one(
    const SqlRDBDriver *drv, void *stmt, int bind_idx,
    const SqlRDBColumnDef *col, const void *row)
{
    const unsigned char *base = (const unsigned char *)row + col->offset;
    switch (col->type) {
    case SQL_TYPE_INT32: {
        int32_t v;
        memcpy(&v, base, sizeof(v));
        return drv->bind_int32(stmt, bind_idx, v);
    }
    case SQL_TYPE_INT64: {
        int64_t v;
        memcpy(&v, base, sizeof(v));
        return drv->bind_int64(stmt, bind_idx, v);
    }
    case SQL_TYPE_REAL: {
        double v;
        memcpy(&v, base, sizeof(v));
        return drv->bind_real(stmt, bind_idx, v);
    }
    case SQL_TYPE_TEXT: {
        const char *s = (const char *)base;
        /* Be defensive: cap at col->size in case the caller forgot to NUL-
         * terminate. Driver bind_text with len >= 0 does not assume NUL. */
        size_t n = bounded_strlen(s, col->size);
        if (n > (size_t)INT_MAX) n = (size_t)INT_MAX;
        return drv->bind_text(stmt, bind_idx, s, (int)n);
    }
    case SQL_TYPE_BLOB:
        return drv->bind_blob(stmt, bind_idx, base, col->size);
    }
    return SQL_RDB_ERR_INTERNAL;
}

SqlRDBResult c2sql_internal_tm_bind_row(
    const SqlRDBDriver *drv,
    void               *stmt,
    const SqlRDBSchema *schema,
    const void         *row,
    const uint8_t      *null_map)
{
    if (!drv || !stmt || !schema || !row) return SQL_RDB_ERR_INVALID_ARG;
    for (size_t i = 0; i < schema->col_count; i++) {
        const SqlRDBColumnDef *col = &schema->cols[i];
        int bind_idx = (int)i + 1;
        if (c2sql_internal_null_bit_get(null_map, i)) {
            if (!(col->flags & SQL_COL_FLAG_NULLABLE)) {
                return SQL_RDB_ERR_NOT_NULL_VIOLATION;
            }
            SqlRDBResult r = drv->bind_null(stmt, bind_idx);
            if (r != SQL_RDB_OK) return r;
            continue;
        }
        SqlRDBResult r = bind_one(drv, stmt, bind_idx, col, row);
        if (r != SQL_RDB_OK) return r;
    }
    return SQL_RDB_OK;
}

/*
 * Read one column. Returns SQL_RDB_OK on a clean read, SQL_RDB_WARN_TRUNCATED
 * if data was truncated (still written), or a driver error otherwise.
 */
static SqlRDBResult read_one(
    const SqlRDBDriver *drv, void *stmt, int col_idx,
    const SqlRDBColumnDef *col, void *row)
{
    unsigned char *base = (unsigned char *)row + col->offset;
    switch (col->type) {
    case SQL_TYPE_INT32: {
        int64_t v;
        SqlRDBResult r = drv->column_int64(stmt, col_idx, &v);
        if (r != SQL_RDB_OK) return r;
        bool truncated = (v < INT32_MIN || v > INT32_MAX);
        int32_t out = (int32_t)v;   /* implementation-defined cast on overflow */
        memcpy(base, &out, sizeof(out));
        return truncated ? SQL_RDB_WARN_TRUNCATED : SQL_RDB_OK;
    }
    case SQL_TYPE_INT64: {
        int64_t v;
        SqlRDBResult r = drv->column_int64(stmt, col_idx, &v);
        if (r != SQL_RDB_OK) return r;
        memcpy(base, &v, sizeof(v));
        return SQL_RDB_OK;
    }
    case SQL_TYPE_REAL: {
        double v;
        SqlRDBResult r = drv->column_real(stmt, col_idx, &v);
        if (r != SQL_RDB_OK) return r;
        memcpy(base, &v, sizeof(v));
        return SQL_RDB_OK;
    }
    case SQL_TYPE_TEXT: {
        const char *p = NULL;
        size_t plen = 0;
        SqlRDBResult r = drv->column_text(stmt, col_idx, &p, &plen);
        if (r != SQL_RDB_OK) return r;
        if (col->size == 0) return SQL_RDB_WARN_TRUNCATED;
        size_t maxlen = col->size - 1u;
        size_t to_copy = plen < maxlen ? plen : maxlen;
        if (to_copy > 0 && p) memcpy(base, p, to_copy);
        base[to_copy] = '\0';
        return (plen > maxlen) ? SQL_RDB_WARN_TRUNCATED : SQL_RDB_OK;
    }
    case SQL_TYPE_BLOB: {
        const void *p = NULL;
        size_t plen = 0;
        SqlRDBResult r = drv->column_blob(stmt, col_idx, &p, &plen);
        if (r != SQL_RDB_OK) return r;
        size_t to_copy = plen < col->size ? plen : col->size;
        if (to_copy > 0 && p) memcpy(base, p, to_copy);
        return (plen > col->size) ? SQL_RDB_WARN_TRUNCATED : SQL_RDB_OK;
    }
    }
    return SQL_RDB_ERR_INTERNAL;
}

SqlRDBResult c2sql_internal_tm_read_row(
    const SqlRDBDriver *drv,
    void               *stmt,
    const SqlRDBSchema *schema,
    void               *out_row,
    uint8_t            *out_null_map)
{
    if (!drv || !stmt || !schema || !out_row) return SQL_RDB_ERR_INVALID_ARG;
    bool any_truncated = false;
    for (size_t i = 0; i < schema->col_count; i++) {
        const SqlRDBColumnDef *col = &schema->cols[i];
        int col_idx = (int)i;
        bool is_null = false;
        SqlRDBResult r = drv->column_isnull(stmt, col_idx, &is_null);
        if (r != SQL_RDB_OK) return r;
        if (is_null) {
            c2sql_internal_null_bit_set(out_null_map, i);
            continue;
        }
        r = read_one(drv, stmt, col_idx, col, out_row);
        if (r == SQL_RDB_WARN_TRUNCATED) {
            any_truncated = true;
        } else if (r != SQL_RDB_OK) {
            return r;
        }
    }
    return any_truncated ? SQL_RDB_WARN_TRUNCATED : SQL_RDB_OK;
}
