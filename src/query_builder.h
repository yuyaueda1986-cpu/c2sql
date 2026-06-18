/*
 * query_builder.h — Internal SQL generation API for libc2sql.
 *
 * Generates SQL strings from schema metadata and an optional condition AST.
 * No DB access is performed here; generated SQL is returned as a heap-allocated
 * string that the caller must free().
 *
 * All values are emitted as ? placeholders; no user data is interpolated
 * directly into SQL (injection-safe by construction).
 */
#ifndef C2SQL_QUERY_BUILDER_H
#define C2SQL_QUERY_BUILDER_H

#include "c2sql.h"
#include "db_driver.h"
#include "schema_registry.h"
#include "condition_ast.h"
#include <stddef.h>

/* Operation selector for SqlRDBQuerySpec.op */
typedef enum {
    C2SQL_QB_CREATE,        /* CREATE TABLE IF NOT EXISTS ... STRICT          */
    C2SQL_QB_INSERT,        /* INSERT INTO ... VALUES (...)                   */
    C2SQL_QB_UPSERT,        /* INSERT INTO ... ON CONFLICT(...) DO UPDATE     */
    C2SQL_QB_SELECT,        /* SELECT ... FROM ... [WHERE ...]                */
    C2SQL_QB_COUNT,         /* SELECT COUNT(*) FROM ... [WHERE ...]           */
    C2SQL_QB_DELETE,        /* DELETE FROM ... [WHERE ...]                    */
    C2SQL_QB_ALTER_ADD,     /* ALTER TABLE ... ADD COLUMN ...                 */
    C2SQL_QB_UPDATE_FIELD,  /* UPDATE table SET <target_col>=? WHERE ...      */
    C2SQL_QB_SELECT_FIELD   /* SELECT <target_col> FROM table WHERE ...       */
} C2SqlQBOp;

/*
 * Input descriptor for c2sql_internal_qb_build().
 *
 * Fields:
 *   op       - Which SQL statement to generate.
 *   schema   - Target table schema (must be non-NULL).
 *   cond     - WHERE condition (nullable; NULL or COND_ALL → no WHERE).
 *   new_col  - New column definition for ALTER_ADD; NULL for other ops.
 */
typedef struct {
    C2SqlQBOp              op;
    const SqlRDBSchema    *schema;
    const SqlRDBCondition *cond;
    const SqlRDBColumnDef *new_col;
    const char            *target_col;  /* UPDATE_FIELD / SELECT_FIELD */
    C2SqlDialect          dialect;      /* target SQL dialect (default SQLITE=0) */
} SqlRDBQuerySpec;

/*
 * Build a SQL string from the given query spec.
 *
 * On success:
 *   - *out_sql is a malloc'd NUL-terminated string; caller must free() it.
 *   - *out_bind_count receives the number of ? placeholders in the SQL.
 *   - Returns SQL_RDB_OK.
 *
 * On failure:
 *   - *out_sql is set to NULL.
 *   - Returns SQL_RDB_ERR_INVALID_ARG or SQL_RDB_ERR_NO_MEMORY.
 *
 * Preconditions: spec and spec->schema must be non-NULL.
 */
SqlRDBResult c2sql_internal_qb_build(
    const SqlRDBQuerySpec *spec,
    char                 **out_sql,
    size_t                *out_bind_count);

#endif /* C2SQL_QUERY_BUILDER_H */
