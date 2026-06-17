/*
 * schema_registry.h — Internal schema registry for libc2sql.
 *
 * Maps struct names to SqlRDBSchema instances (deep copies of SqlRDBColumnDef
 * arrays). The registry is owned by SqlRDBHandle and protected by its mutex.
 *
 * Preconditions for all functions: caller holds the handle mutex when
 * threadsafe mode is enabled.
 */
#ifndef C2SQL_SCHEMA_REGISTRY_H
#define C2SQL_SCHEMA_REGISTRY_H

#include "c2sql.h"
#include <stddef.h>

/* Maximum number of columns per registered schema. */
#define C2SQL_MAX_COLUMNS 64

/*
 * Internal representation of a registered struct schema.
 * All pointer fields are owned by this struct and freed by the registry.
 */
typedef struct SqlRDBSchema {
    char            *name;      /* strdup'd struct/table name */
    SqlRDBColumnDef *cols;      /* deep copy (each cols[i].name is strdup'd) */
    size_t           col_count;
    int              pk_index;  /* index of PRIMARY KEY column, or -1 if none */
} SqlRDBSchema;

/* Dynamic array of registered schemas (linear search; hash-ready API). */
typedef struct SqlRDBSchemaRegistry {
    SqlRDBSchema *entries;
    size_t        count;
    size_t        capacity;
} SqlRDBSchemaRegistry;

/* Forward declaration — full struct in handle_internal.h */
struct SqlRDBHandle;

/* Initialize an empty registry. Must be called before any other operation. */
SqlRDBResult c2sql_internal_schema_registry_init(SqlRDBSchemaRegistry *reg);

/* Free all schemas and reset the registry to the empty state. */
void c2sql_internal_schema_registry_destroy(SqlRDBSchemaRegistry *reg);

/*
 * Validate and register a struct schema.
 *
 * Validation performed:
 *   - name and cols must be non-NULL; col_count must be > 0
 *   - col_count <= C2SQL_MAX_COLUMNS
 *   - name and each cols[i].name must be valid identifiers
 *     (ASCII alpha/underscore start, alphanumeric/underscore body, not a
 *      SQL reserved word)
 *   - No duplicate struct name in this handle
 *   - No duplicate column names within cols[]
 *   - INT32 columns must have size == 4; INT64 columns must have size == 8
 *
 * On success, cols[] is deep-copied into the registry. The caller may free
 * or reuse cols[] immediately after this call returns SQL_RDB_OK.
 *
 * On failure, the registry is left unchanged.
 */
SqlRDBResult c2sql_internal_schema_register(
    struct SqlRDBHandle  *h,
    const char           *name,
    const SqlRDBColumnDef *cols,
    size_t                col_count);

/*
 * Look up a registered schema by exact (case-sensitive) name.
 * Returns a pointer into the registry (valid until the registry is
 * destroyed), or NULL if not found.
 */
const SqlRDBSchema *c2sql_internal_schema_lookup(
    const struct SqlRDBHandle *h,
    const char                *name);

/*
 * Remove the schema entry with the given name from the registry, freeing its
 * owned memory. Returns SQL_RDB_OK if removed, SQL_RDB_ERR_UNKNOWN_STRUCT if
 * no such name. Intended for rolling back a register call that failed at a
 * later step (e.g. CREATE TABLE / migration).
 */
SqlRDBResult c2sql_internal_schema_unregister(
    struct SqlRDBHandle *h,
    const char          *name);

#endif /* C2SQL_SCHEMA_REGISTRY_H */
