/*
 * type_mapping.h — Internal type mapping and NULL bitmap helpers for libc2sql.
 *
 * Bridges between C struct members and the driver vtable's typed bind/column
 * operations. Tasks 8.1 (bind/read per SqlRDBType) and 8.2 (NULL bitmap
 * handling + NOT NULL enforcement).
 *
 * Layouts assumed for each SqlRDBType when packed inside the caller's struct:
 *   - SQL_TYPE_INT32 : int32_t at col->offset, col->size == 4
 *   - SQL_TYPE_INT64 : int64_t at col->offset, col->size == 8
 *   - SQL_TYPE_REAL  : double  at col->offset, col->size == sizeof(double)
 *   - SQL_TYPE_TEXT  : char[col->size] at col->offset, NUL-terminated
 *   - SQL_TYPE_BLOB  : uint8_t[col->size] at col->offset, fixed length
 *
 * Truncation rules (Req 5.6, 8.2):
 *   - INT32 read: if stored INTEGER falls outside [INT32_MIN, INT32_MAX] the
 *     value is cast and SQL_RDB_WARN_TRUNCATED is reported.
 *   - TEXT  read: if stored TEXT length >= col->size, the value is copied
 *     up to size-1 chars, NUL-terminated, and SQL_RDB_WARN_TRUNCATED reported.
 *   - BLOB  read: if stored BLOB length > col->size, the leading col->size
 *     bytes are copied and SQL_RDB_WARN_TRUNCATED reported.
 *
 * NULL handling:
 *   - bind: null_map bit == 1 binds SQL NULL. If the column does not carry
 *     SQL_COL_FLAG_NULLABLE the call returns SQL_RDB_ERR_NOT_NULL_VIOLATION
 *     immediately without producing partial side effects on the SQL stmt
 *     visible to the caller.
 *   - read: when the driver reports the column as SQL NULL the corresponding
 *     bit of out_null_map (if provided) is set and the struct member is left
 *     unmodified.
 *
 * Concurrency: none. Caller serializes via the owning handle's mutex.
 */
#ifndef C2SQL_TYPE_MAPPING_H
#define C2SQL_TYPE_MAPPING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "c2sql.h"
#include "db_driver.h"
#include "schema_registry.h"

/* ------------------------------------------------------------------ */
/* NULL bitmap helpers (inline; safe with map == NULL).                */
/* ------------------------------------------------------------------ */

static inline bool c2sql_internal_null_bit_get(const uint8_t *map, size_t col_idx) {
    if (!map) return false;
    return ((map[col_idx >> 3] >> (col_idx & 7u)) & 1u) != 0u;
}

static inline void c2sql_internal_null_bit_set(uint8_t *map, size_t col_idx) {
    if (!map) return;
    map[col_idx >> 3] |= (uint8_t)(1u << (col_idx & 7u));
}

/* ------------------------------------------------------------------ */
/* Row-level bind / read                                               */
/* ------------------------------------------------------------------ */

/*
 * Bind every column of `schema` from `row` to `stmt` in declaration order.
 * Parameters are bound starting at index 1 (driver vtable convention).
 *
 * null_map (may be NULL) is consulted per column:
 *   - bit == 1 + SQL_COL_FLAG_NULLABLE set  -> driver->bind_null
 *   - bit == 1 + SQL_COL_FLAG_NULLABLE clear -> SQL_RDB_ERR_NOT_NULL_VIOLATION
 *   - bit == 0                              -> typed bind from struct member
 *
 * Returns SQL_RDB_OK on success, or the first error encountered.
 */
SqlRDBResult c2sql_internal_tm_bind_row(
    const SqlRDBDriver *drv,
    void               *stmt,
    const SqlRDBSchema *schema,
    const void         *row,
    const uint8_t      *null_map);

/*
 * Read every column of `schema` from the current row of `stmt` into `out_row`.
 * Columns are read starting at index 0.
 *
 * If out_null_map is non-NULL, bits are set (never cleared) for columns whose
 * stored value is SQL NULL; those struct members are left untouched.
 *
 * Returns SQL_RDB_OK if every column read cleanly, SQL_RDB_WARN_TRUNCATED if
 * any column was truncated (other columns still wrote successfully), or a
 * driver error code if a column read failed.
 */
SqlRDBResult c2sql_internal_tm_read_row(
    const SqlRDBDriver *drv,
    void               *stmt,
    const SqlRDBSchema *schema,
    void               *out_row,
    uint8_t            *out_null_map);

#endif /* C2SQL_TYPE_MAPPING_H */
