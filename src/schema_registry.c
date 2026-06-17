/*
 * schema_registry.c — Schema registry implementation.
 */
#define _POSIX_C_SOURCE 200809L  /* strdup */
#include "schema_registry.h"
#include "handle_internal.h"
#include "error_ctx.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* SQL reserved word list (SQLite keywords, uppercase, NULL-terminated) */
/* ------------------------------------------------------------------ */

static const char *const RESERVED_WORDS[] = {
    "ABORT", "ACTION", "ADD", "AFTER", "ALL", "ALTER", "ALWAYS",
    "ANALYZE", "AND", "AS", "ASC", "ATTACH", "AUTOINCREMENT",
    "BEFORE", "BEGIN", "BETWEEN", "BY",
    "CASCADE", "CASE", "CAST", "CHECK", "COLLATE", "COLUMN",
    "COMMIT", "CONFLICT", "CONSTRAINT", "CREATE", "CROSS",
    "CURRENT", "CURRENT_DATE", "CURRENT_TIME", "CURRENT_TIMESTAMP",
    "DATABASE", "DEFAULT", "DEFERRABLE", "DEFERRED", "DELETE",
    "DETACH", "DISTINCT", "DO", "DROP",
    "EACH", "ELSE", "END", "ESCAPE", "EXCEPT", "EXCLUDE",
    "EXCLUSIVE", "EXISTS", "EXPLAIN",
    "FAIL", "FILTER", "FIRST", "FOLLOWING", "FOR", "FOREIGN",
    "FROM", "FULL",
    "GENERATED", "GLOB", "GROUP", "GROUPS",
    "HAVING",
    "IF", "IGNORE", "IMMEDIATE", "IN", "INDEX", "INDEXED",
    "INITIALLY", "INNER", "INSERT", "INSTEAD", "INTERSECT", "INTO",
    "IS", "ISNULL",
    "JOIN", "KEY",
    "LAST", "LEFT", "LIKE", "LIMIT",
    "MATCH", "MATERIALIZED",
    "NATURAL", "NO", "NOT", "NOTHING", "NOTNULL", "NULL", "NULLS",
    "OF", "OFFSET", "ON", "OR", "ORDER", "OTHERS", "OUTER", "OVER",
    "PARTITION", "PLAN", "PRAGMA", "PRECEDING", "PRIMARY",
    "QUERY",
    "RAISE", "RANGE", "RECURSIVE", "REFERENCES", "REGEXP",
    "REINDEX", "RELEASE", "RENAME", "REPLACE", "RESTRICT",
    "RETURNING", "RIGHT", "ROLLBACK", "ROW", "ROWS",
    "SAVEPOINT", "SELECT", "SET",
    "TABLE", "TEMP", "TEMPORARY", "THEN", "TIES", "TO",
    "TRANSACTION", "TRIGGER",
    "UNBOUNDED", "UNION", "UNIQUE", "UPDATE", "USING",
    "VACUUM", "VALUES", "VIEW", "VIRTUAL",
    "WHEN", "WHERE", "WINDOW", "WITH", "WITHOUT",
    NULL
};

/* Maximum length of an identifier (longer → INVALID_NAME). */
#define MAX_IDENT_LEN 128

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * Returns true when `name` is a valid SQL identifier:
 *   - non-empty, <= MAX_IDENT_LEN chars
 *   - first char: ASCII letter or underscore
 *   - subsequent chars: ASCII alphanumeric or underscore
 *   - not a SQL reserved word (case-insensitive)
 */
static bool identifier_valid(const char *name) {
    if (name == NULL || name[0] == '\0') return false;

    /* First character: letter or underscore */
    unsigned char first = (unsigned char)name[0];
    if (!isalpha(first) && first != '_') return false;

    /* Remaining characters and length check */
    size_t len = 1;
    for (; name[len] != '\0'; len++) {
        if (len >= MAX_IDENT_LEN) return false;
        unsigned char c = (unsigned char)name[len];
        if (!isalnum(c) && c != '_') return false;
    }

    /* Reserved word check (convert to uppercase, compare) */
    char upper[MAX_IDENT_LEN + 1];
    for (size_t i = 0; i < len; i++) {
        upper[i] = (char)toupper((unsigned char)name[i]);
    }
    upper[len] = '\0';

    for (size_t i = 0; RESERVED_WORDS[i] != NULL; i++) {
        if (strcmp(upper, RESERVED_WORDS[i]) == 0) return false;
    }

    return true;
}

/*
 * Deep-copy `n` column definitions.
 * Each cols[i].name is strdup'd. Returns SQL_RDB_ERR_NO_MEMORY on failure,
 * leaving no partial allocation for the caller to free.
 */
static SqlRDBResult deep_copy_cols(
    const SqlRDBColumnDef *src,
    size_t                 n,
    SqlRDBColumnDef      **out)
{
    SqlRDBColumnDef *dst = malloc(n * sizeof(SqlRDBColumnDef));
    if (dst == NULL) return SQL_RDB_ERR_NO_MEMORY;

    for (size_t i = 0; i < n; i++) {
        dst[i]      = src[i];
        dst[i].name = strdup(src[i].name);
        if (dst[i].name == NULL) {
            for (size_t j = 0; j < i; j++) free((char *)dst[j].name);
            free(dst);
            return SQL_RDB_ERR_NO_MEMORY;
        }
    }
    *out = dst;
    return SQL_RDB_OK;
}

/* Grow the registry's entry array when count == capacity. */
static SqlRDBResult registry_grow(SqlRDBSchemaRegistry *reg, SqlRDBErrorCtx *err) {
    size_t       new_cap  = (reg->capacity == 0) ? 8 : reg->capacity * 2;
    SqlRDBSchema *entries = realloc(reg->entries, new_cap * sizeof(SqlRDBSchema));
    if (entries == NULL) {
        c2sql_internal_err_set(err, SQL_RDB_ERR_NO_MEMORY,
                               "schema registry: out of memory growing to %zu entries",
                               new_cap);
        return SQL_RDB_ERR_NO_MEMORY;
    }
    reg->entries  = entries;
    reg->capacity = new_cap;
    return SQL_RDB_OK;
}

/* Free a single SqlRDBSchema's owned memory. */
static void schema_free(SqlRDBSchema *s) {
    for (size_t i = 0; i < s->col_count; i++) {
        free((char *)s->cols[i].name);
    }
    free(s->cols);
    free(s->name);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

SqlRDBResult c2sql_internal_schema_registry_init(SqlRDBSchemaRegistry *reg) {
    reg->entries  = NULL;
    reg->count    = 0;
    reg->capacity = 0;
    return SQL_RDB_OK;
}

void c2sql_internal_schema_registry_destroy(SqlRDBSchemaRegistry *reg) {
    for (size_t i = 0; i < reg->count; i++) {
        schema_free(&reg->entries[i]);
    }
    free(reg->entries);
    reg->entries  = NULL;
    reg->count    = 0;
    reg->capacity = 0;
}

SqlRDBResult c2sql_internal_schema_register(
    struct SqlRDBHandle   *h,
    const char            *name,
    const SqlRDBColumnDef *cols,
    size_t                 col_count)
{
    if (!c2sql_handle_valid(h)) return SQL_RDB_ERR_INVALID_HANDLE;

    /* Argument presence */
    if (name == NULL || cols == NULL) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_INVALID_ARG,
                               "schema_register: name and cols must be non-NULL");
        return SQL_RDB_ERR_INVALID_ARG;
    }
    if (col_count == 0) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_INVALID_ARG,
                               "schema_register: col_count must be > 0");
        return SQL_RDB_ERR_INVALID_ARG;
    }
    if (col_count > C2SQL_MAX_COLUMNS) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_TOO_MANY_COLUMNS,
                               "schema_register: col_count %zu exceeds maximum %d",
                               col_count, C2SQL_MAX_COLUMNS);
        return SQL_RDB_ERR_TOO_MANY_COLUMNS;
    }

    /* Validate struct name */
    if (!identifier_valid(name)) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_INVALID_NAME,
                               "schema_register: invalid struct name '%s'", name);
        return SQL_RDB_ERR_INVALID_NAME;
    }

    /* Duplicate struct check */
    SqlRDBSchemaRegistry *reg = &h->registry;
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            c2sql_internal_err_set(&h->error, SQL_RDB_ERR_DUPLICATE_SCHEMA,
                                   "schema_register: struct '%s' already registered", name);
            return SQL_RDB_ERR_DUPLICATE_SCHEMA;
        }
    }

    /* Validate each column */
    for (size_t i = 0; i < col_count; i++) {
        const SqlRDBColumnDef *col = &cols[i];

        if (col->name == NULL) {
            c2sql_internal_err_set(&h->error, SQL_RDB_ERR_INVALID_ARG,
                                   "schema_register: cols[%zu].name is NULL", i);
            return SQL_RDB_ERR_INVALID_ARG;
        }
        if (!identifier_valid(col->name)) {
            c2sql_internal_err_set(&h->error, SQL_RDB_ERR_INVALID_NAME,
                                   "schema_register: invalid column name '%s'", col->name);
            return SQL_RDB_ERR_INVALID_NAME;
        }

        /* Duplicate column name check (O(n²) for small n) */
        for (size_t j = 0; j < i; j++) {
            if (strcmp(cols[j].name, col->name) == 0) {
                c2sql_internal_err_set(&h->error, SQL_RDB_ERR_DUPLICATE_COLUMN,
                                       "schema_register: duplicate column name '%s'", col->name);
                return SQL_RDB_ERR_DUPLICATE_COLUMN;
            }
        }

        /* Integer type size validation */
        if (col->type == SQL_TYPE_INT32 && col->size != 4) {
            c2sql_internal_err_set(&h->error, SQL_RDB_ERR_INVALID_ARG,
                                   "schema_register: INT32 column '%s' must have size 4, got %zu",
                                   col->name, col->size);
            return SQL_RDB_ERR_INVALID_ARG;
        }
        if (col->type == SQL_TYPE_INT64 && col->size != 8) {
            c2sql_internal_err_set(&h->error, SQL_RDB_ERR_INVALID_ARG,
                                   "schema_register: INT64 column '%s' must have size 8, got %zu",
                                   col->name, col->size);
            return SQL_RDB_ERR_INVALID_ARG;
        }
    }

    /* Grow registry if full */
    if (reg->count == reg->capacity) {
        SqlRDBResult r = registry_grow(reg, &h->error);
        if (r != SQL_RDB_OK) return r;
    }

    /* Build the new schema entry */
    SqlRDBSchema *entry = &reg->entries[reg->count];
    entry->name = strdup(name);
    if (entry->name == NULL) {
        c2sql_internal_err_set(&h->error, SQL_RDB_ERR_NO_MEMORY,
                               "schema_register: out of memory duplicating name");
        return SQL_RDB_ERR_NO_MEMORY;
    }

    SqlRDBResult r = deep_copy_cols(cols, col_count, &entry->cols);
    if (r != SQL_RDB_OK) {
        free(entry->name);
        entry->name = NULL;
        c2sql_internal_err_set(&h->error, r, "schema_register: out of memory copying columns");
        return r;
    }

    entry->col_count = col_count;

    /* Find primary key index */
    entry->pk_index = -1;
    for (size_t i = 0; i < col_count; i++) {
        if (cols[i].flags & SQL_COL_FLAG_PRIMARY_KEY) {
            entry->pk_index = (int)i;
            break;
        }
    }

    reg->count++;
    return SQL_RDB_OK;
}

const SqlRDBSchema *c2sql_internal_schema_lookup(
    const struct SqlRDBHandle *h,
    const char                *name)
{
    if (!c2sql_handle_valid(h) || name == NULL) return NULL;

    const SqlRDBSchemaRegistry *reg = &h->registry;
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            return &reg->entries[i];
        }
    }
    return NULL;
}

SqlRDBResult c2sql_internal_schema_unregister(
    struct SqlRDBHandle *h,
    const char          *name)
{
    if (!c2sql_handle_valid(h) || name == NULL) return SQL_RDB_ERR_INVALID_ARG;

    SqlRDBSchemaRegistry *reg = &h->registry;
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            schema_free(&reg->entries[i]);
            /* Shift trailing entries down to keep the array contiguous. */
            for (size_t j = i + 1; j < reg->count; j++) {
                reg->entries[j - 1] = reg->entries[j];
            }
            reg->count--;
            return SQL_RDB_OK;
        }
    }
    return SQL_RDB_ERR_UNKNOWN_STRUCT;
}
