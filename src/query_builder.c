/*
 * query_builder.c — SQL string generation for libc2sql.
 *
 * All user-supplied values are emitted as ? placeholders; no data is
 * interpolated into SQL text (injection-safe by construction).
 */
#define _POSIX_C_SOURCE 200809L
#include "query_builder.h"
#include "condition_ast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Dynamic string buffer                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

static bool buf_init(Buf *b) {
    b->data = malloc(256);
    if (!b->data) return false;
    b->data[0] = '\0';
    b->len = 0;
    b->cap = 256;
    return true;
}

static bool buf_grow(Buf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return true;
    size_t new_cap = b->cap;
    while (new_cap < b->len + extra + 1) new_cap *= 2;
    char *p = realloc(b->data, new_cap);
    if (!p) return false;
    b->data = p;
    b->cap  = new_cap;
    return true;
}

static bool buf_append(Buf *b, const char *s) {
    size_t n = strlen(s);
    if (!buf_grow(b, n)) return false;
    memcpy(b->data + b->len, s, n + 1);
    b->len += n;
    return true;
}

static void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

/* ------------------------------------------------------------------ */
/* Type and operator helpers                                           */
/* ------------------------------------------------------------------ */

static const char *type_to_sql_str(SqlRDBType t, C2SqlDialect d) {
    if (d == C2SQL_DIALECT_POSTGRES) {
        switch (t) {
            case SQL_TYPE_INT32: return "INTEGER";
            case SQL_TYPE_INT64: return "BIGINT";
            case SQL_TYPE_REAL:  return "DOUBLE PRECISION";
            case SQL_TYPE_TEXT:  return "TEXT";
            case SQL_TYPE_BLOB:  return "BYTEA";
        }
        return "TEXT";
    }
    switch (t) {
        case SQL_TYPE_INT32:
        case SQL_TYPE_INT64: return "INTEGER";  /* SQLite uses INTEGER affinity */
        case SQL_TYPE_REAL:  return "REAL";
        case SQL_TYPE_TEXT:  return "TEXT";
        case SQL_TYPE_BLOB:  return "BLOB";
    }
    return "ANY";
}

/*
 * Append the next bind placeholder: '?' for SQLite, '$N' for PostgreSQL
 * (N = 1-based parameter index). Increments *bind_count first so the
 * emitted index matches the eventual bind order.
 */
static bool append_placeholder(Buf *b, C2SqlDialect d, size_t *bind_count) {
    (*bind_count)++;
    if (d == C2SQL_DIALECT_POSTGRES) {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "$%zu", *bind_count);
        return buf_append(b, tmp);
    }
    return buf_append(b, "?");
}

static const char *op_to_sql_str(SqlRDBOp op) {
    switch (op) {
        case SQL_OP_EQ: return "=";
        case SQL_OP_NE: return "!=";
        case SQL_OP_LT: return "<";
        case SQL_OP_LE: return "<=";
        case SQL_OP_GT: return ">";
        case SQL_OP_GE: return ">=";
    }
    return "=";
}

/* ------------------------------------------------------------------ */
/* Column constraint fragment                                          */
/* ------------------------------------------------------------------ */

/*
 * Append column constraints: [NOT NULL] [PRIMARY KEY] [UNIQUE].
 * UNIQUE is suppressed for PK columns (PK implies uniqueness).
 */
static bool append_col_constraints(Buf *b, const SqlRDBColumnDef *col) {
    bool nullable = (col->flags & SQL_COL_FLAG_NULLABLE) != 0;
    bool pk       = (col->flags & SQL_COL_FLAG_PRIMARY_KEY) != 0;
    bool unique   = (col->flags & SQL_COL_FLAG_UNIQUE) != 0;

    if (!nullable) {
        if (!buf_append(b, " NOT NULL")) return false;
    }
    if (pk) {
        if (!buf_append(b, " PRIMARY KEY")) return false;
    }
    if (unique && !pk) {
        if (!buf_append(b, " UNIQUE")) return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* WHERE clause generation (recursive condition AST traversal)        */
/* ------------------------------------------------------------------ */

/* Returns true if schema contains a column with the given name. */
static bool schema_has_column(const SqlRDBSchema *schema, const char *col) {
    for (size_t i = 0; i < schema->col_count; i++) {
        if (strcmp(schema->cols[i].name, col) == 0) return true;
    }
    return false;
}

/*
 * Validate that every leaf column name in cond exists in schema.
 * Returns SQL_RDB_ERR_UNKNOWN_COLUMN on first mismatch, SQL_RDB_OK otherwise.
 */
static SqlRDBResult validate_cond_columns(const SqlRDBCondition *cond,
                                          const SqlRDBSchema    *schema) {
    if (!cond || cond->kind == COND_ALL) return SQL_RDB_OK;
    if (cond->kind == COND_LEAF) {
        if (!schema_has_column(schema, cond->u.leaf.col))
            return SQL_RDB_ERR_UNKNOWN_COLUMN;
        return SQL_RDB_OK;
    }
    SqlRDBResult r = validate_cond_columns(cond->u.composite.left,  schema);
    if (r != SQL_RDB_OK) return r;
    return           validate_cond_columns(cond->u.composite.right, schema);
}

/*
 * Append a WHERE condition to the buffer, incrementing *bind_count for
 * each ? placeholder emitted.
 * Returns true on success, false on OOM.
 */
static bool append_cond(Buf *b, const SqlRDBCondition *cond,
                        C2SqlDialect d, size_t *bind_count) {
    switch (cond->kind) {
        case COND_ALL:
            /* COND_ALL means no WHERE clause; callers must check for this */
            return true;

        case COND_LEAF:
            if (!buf_append(b, "\""))                                 return false;
            if (!buf_append(b, cond->u.leaf.col))                    return false;
            if (!buf_append(b, "\" "))                               return false;
            if (!buf_append(b, op_to_sql_str(cond->u.leaf.op)))      return false;
            if (!buf_append(b, " "))                                  return false;
            if (!append_placeholder(b, d, bind_count))               return false;
            return true;

        case COND_AND:
        case COND_OR: {
            const char *op_str = (cond->kind == COND_AND) ? " AND " : " OR ";
            if (!buf_append(b, "("))                                          return false;
            if (!append_cond(b, cond->u.composite.left,  d, bind_count))     return false;
            if (!buf_append(b, op_str))                                       return false;
            if (!append_cond(b, cond->u.composite.right, d, bind_count))     return false;
            if (!buf_append(b, ")"))                                          return false;
            return true;
        }
    }
    return false;
}

/*
 * Returns true if cond requires a WHERE clause.
 * NULL and COND_ALL both mean "no WHERE".
 */
static bool cond_needs_where(const SqlRDBCondition *cond) {
    return cond != NULL && cond->kind != COND_ALL;
}

/* ------------------------------------------------------------------ */
/* DDL builders (Task 5.1)                                            */
/* ------------------------------------------------------------------ */

static SqlRDBResult build_create(const SqlRDBQuerySpec *spec, Buf *b) {
    const SqlRDBSchema *s = spec->schema;

    if (!buf_append(b, "CREATE TABLE IF NOT EXISTS \"")) return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, s->name))                         return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, "\" ("))                          return SQL_RDB_ERR_NO_MEMORY;

    for (size_t i = 0; i < s->col_count; i++) {
        if (i > 0 && !buf_append(b, ","))               return SQL_RDB_ERR_NO_MEMORY;
        if (!buf_append(b, "\""))                        return SQL_RDB_ERR_NO_MEMORY;
        if (!buf_append(b, s->cols[i].name))             return SQL_RDB_ERR_NO_MEMORY;
        if (!buf_append(b, "\" "))                       return SQL_RDB_ERR_NO_MEMORY;
        if (!buf_append(b, type_to_sql_str(s->cols[i].type, spec->dialect))) return SQL_RDB_ERR_NO_MEMORY;
        if (!append_col_constraints(b, &s->cols[i]))     return SQL_RDB_ERR_NO_MEMORY;
    }

    /* STRICT is SQLite-only (PostgreSQL is statically typed already). */
    if (spec->dialect == C2SQL_DIALECT_POSTGRES) {
        if (!buf_append(b, ")"))                         return SQL_RDB_ERR_NO_MEMORY;
    } else {
        if (!buf_append(b, ") STRICT"))                  return SQL_RDB_ERR_NO_MEMORY;
    }
    return SQL_RDB_OK;
}

static SqlRDBResult build_alter_add(const SqlRDBQuerySpec *spec, Buf *b) {
    const SqlRDBSchema    *s   = spec->schema;
    const SqlRDBColumnDef *col = spec->new_col;

    if (!buf_append(b, "ALTER TABLE \""))                return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, s->name))                         return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, "\" ADD COLUMN \""))              return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, col->name))                       return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, "\" "))                           return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, type_to_sql_str(col->type, spec->dialect))) return SQL_RDB_ERR_NO_MEMORY;
    if (!append_col_constraints(b, col))                 return SQL_RDB_ERR_NO_MEMORY;
    return SQL_RDB_OK;
}

/* ------------------------------------------------------------------ */
/* DML builders (Task 5.2)                                            */
/* ------------------------------------------------------------------ */

static SqlRDBResult build_insert(const SqlRDBQuerySpec *spec, Buf *b,
                                 size_t *bind_count) {
    const SqlRDBSchema *s = spec->schema;

    if (!buf_append(b, "INSERT INTO \""))  return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, s->name))           return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, "\" ("))            return SQL_RDB_ERR_NO_MEMORY;

    for (size_t i = 0; i < s->col_count; i++) {
        if (i > 0 && !buf_append(b, ",")) return SQL_RDB_ERR_NO_MEMORY;
        if (!buf_append(b, "\""))          return SQL_RDB_ERR_NO_MEMORY;
        if (!buf_append(b, s->cols[i].name)) return SQL_RDB_ERR_NO_MEMORY;
        if (!buf_append(b, "\""))          return SQL_RDB_ERR_NO_MEMORY;
    }

    if (!buf_append(b, ") VALUES ("))      return SQL_RDB_ERR_NO_MEMORY;
    for (size_t i = 0; i < s->col_count; i++) {
        if (i > 0 && !buf_append(b, ","))             return SQL_RDB_ERR_NO_MEMORY;
        if (!append_placeholder(b, spec->dialect, bind_count)) return SQL_RDB_ERR_NO_MEMORY;
    }
    if (!buf_append(b, ")"))              return SQL_RDB_ERR_NO_MEMORY;

    return SQL_RDB_OK;
}

static SqlRDBResult build_upsert(const SqlRDBQuerySpec *spec, Buf *b,
                                 size_t *bind_count) {
    const SqlRDBSchema *s = spec->schema;

    /* Base INSERT part */
    SqlRDBResult r = build_insert(spec, b, bind_count);
    if (r != SQL_RDB_OK) return r;

    /* ON CONFLICT(pk_col) DO UPDATE SET non-pk-cols=excluded.col */
    const char *pk_name = s->cols[s->pk_index].name;

    if (!buf_append(b, " ON CONFLICT(\""))  return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, pk_name))            return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, "\") DO UPDATE SET ")) return SQL_RDB_ERR_NO_MEMORY;

    bool first = true;
    for (size_t i = 0; i < s->col_count; i++) {
        if ((int)i == s->pk_index) continue;  /* skip PK column */
        if (!first && !buf_append(b, ","))    return SQL_RDB_ERR_NO_MEMORY;
        first = false;
        if (!buf_append(b, "\""))             return SQL_RDB_ERR_NO_MEMORY;
        if (!buf_append(b, s->cols[i].name))  return SQL_RDB_ERR_NO_MEMORY;
        if (!buf_append(b, "\"=excluded.\"")) return SQL_RDB_ERR_NO_MEMORY;
        if (!buf_append(b, s->cols[i].name))  return SQL_RDB_ERR_NO_MEMORY;
        if (!buf_append(b, "\""))             return SQL_RDB_ERR_NO_MEMORY;
    }

    return SQL_RDB_OK;
}

static SqlRDBResult build_select(const SqlRDBQuerySpec *spec, Buf *b,
                                 size_t *bind_count) {
    const SqlRDBSchema    *s    = spec->schema;
    const SqlRDBCondition *cond = spec->cond;

    if (!buf_append(b, "SELECT "))         return SQL_RDB_ERR_NO_MEMORY;
    for (size_t i = 0; i < s->col_count; i++) {
        if (i > 0 && !buf_append(b, ",")) return SQL_RDB_ERR_NO_MEMORY;
        if (!buf_append(b, "\""))          return SQL_RDB_ERR_NO_MEMORY;
        if (!buf_append(b, s->cols[i].name)) return SQL_RDB_ERR_NO_MEMORY;
        if (!buf_append(b, "\""))          return SQL_RDB_ERR_NO_MEMORY;
    }
    if (!buf_append(b, " FROM \""))        return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, s->name))           return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, "\""))              return SQL_RDB_ERR_NO_MEMORY;

    if (cond_needs_where(cond)) {
        SqlRDBResult vr = validate_cond_columns(cond, s);
        if (vr != SQL_RDB_OK) return vr;
        if (!buf_append(b, " WHERE "))     return SQL_RDB_ERR_NO_MEMORY;
        if (!append_cond(b, cond, spec->dialect, bind_count)) return SQL_RDB_ERR_NO_MEMORY;
    }

    return SQL_RDB_OK;
}

static SqlRDBResult build_count(const SqlRDBQuerySpec *spec, Buf *b,
                                size_t *bind_count) {
    const SqlRDBSchema    *s    = spec->schema;
    const SqlRDBCondition *cond = spec->cond;

    if (!buf_append(b, "SELECT COUNT(*) FROM \"")) return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, s->name))                   return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, "\""))                      return SQL_RDB_ERR_NO_MEMORY;

    if (cond_needs_where(cond)) {
        SqlRDBResult vr = validate_cond_columns(cond, s);
        if (vr != SQL_RDB_OK) return vr;
        if (!buf_append(b, " WHERE "))             return SQL_RDB_ERR_NO_MEMORY;
        if (!append_cond(b, cond, spec->dialect, bind_count)) return SQL_RDB_ERR_NO_MEMORY;
    }
    return SQL_RDB_OK;
}

static SqlRDBResult build_update_field(const SqlRDBQuerySpec *spec, Buf *b,
                                       size_t *bind_count) {
    const SqlRDBSchema    *s    = spec->schema;
    const SqlRDBCondition *cond = spec->cond;
    const char            *col  = spec->target_col;

    if (!schema_has_column(s, col)) return SQL_RDB_ERR_UNKNOWN_COLUMN;

    if (!buf_append(b, "UPDATE \""))   return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, s->name))       return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, "\" SET \""))   return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, col))           return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, "\"="))         return SQL_RDB_ERR_NO_MEMORY;
    if (!append_placeholder(b, spec->dialect, bind_count)) return SQL_RDB_ERR_NO_MEMORY;

    if (cond_needs_where(cond)) {
        SqlRDBResult vr = validate_cond_columns(cond, s);
        if (vr != SQL_RDB_OK) return vr;
        if (!buf_append(b, " WHERE "))         return SQL_RDB_ERR_NO_MEMORY;
        if (!append_cond(b, cond, spec->dialect, bind_count)) return SQL_RDB_ERR_NO_MEMORY;
    }
    return SQL_RDB_OK;
}

static SqlRDBResult build_select_field(const SqlRDBQuerySpec *spec, Buf *b,
                                       size_t *bind_count) {
    const SqlRDBSchema    *s    = spec->schema;
    const SqlRDBCondition *cond = spec->cond;
    const char            *col  = spec->target_col;

    if (!schema_has_column(s, col)) return SQL_RDB_ERR_UNKNOWN_COLUMN;

    if (!buf_append(b, "SELECT \""))   return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, col))           return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, "\" FROM \""))  return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, s->name))       return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, "\""))          return SQL_RDB_ERR_NO_MEMORY;

    if (cond_needs_where(cond)) {
        SqlRDBResult vr = validate_cond_columns(cond, s);
        if (vr != SQL_RDB_OK) return vr;
        if (!buf_append(b, " WHERE "))         return SQL_RDB_ERR_NO_MEMORY;
        if (!append_cond(b, cond, spec->dialect, bind_count)) return SQL_RDB_ERR_NO_MEMORY;
    }
    return SQL_RDB_OK;
}

static SqlRDBResult build_delete(const SqlRDBQuerySpec *spec, Buf *b,
                                 size_t *bind_count) {
    const SqlRDBSchema    *s    = spec->schema;
    const SqlRDBCondition *cond = spec->cond;

    if (!buf_append(b, "DELETE FROM \"")) return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, s->name))          return SQL_RDB_ERR_NO_MEMORY;
    if (!buf_append(b, "\""))             return SQL_RDB_ERR_NO_MEMORY;

    if (cond_needs_where(cond)) {
        SqlRDBResult vr = validate_cond_columns(cond, s);
        if (vr != SQL_RDB_OK) return vr;
        if (!buf_append(b, " WHERE "))    return SQL_RDB_ERR_NO_MEMORY;
        if (!append_cond(b, cond, spec->dialect, bind_count)) return SQL_RDB_ERR_NO_MEMORY;
    }

    return SQL_RDB_OK;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

SqlRDBResult c2sql_internal_qb_build(
    const SqlRDBQuerySpec *spec,
    char                 **out_sql,
    size_t                *out_bind_count)
{
    if (out_sql) *out_sql = NULL;

    if (spec == NULL || out_sql == NULL)         return SQL_RDB_ERR_INVALID_ARG;
    if (spec->schema == NULL)                    return SQL_RDB_ERR_INVALID_ARG;
    if (spec->op == C2SQL_QB_ALTER_ADD && spec->new_col == NULL)
                                                 return SQL_RDB_ERR_INVALID_ARG;
    if ((spec->op == C2SQL_QB_UPDATE_FIELD || spec->op == C2SQL_QB_SELECT_FIELD)
        && spec->target_col == NULL)             return SQL_RDB_ERR_INVALID_ARG;

    size_t bind_count = 0;
    if (out_bind_count) *out_bind_count = 0;

    Buf b;
    if (!buf_init(&b)) return SQL_RDB_ERR_NO_MEMORY;

    SqlRDBResult r;
    switch (spec->op) {
        case C2SQL_QB_CREATE:
            r = build_create(spec, &b);
            break;
        case C2SQL_QB_ALTER_ADD:
            r = build_alter_add(spec, &b);
            break;
        case C2SQL_QB_INSERT:
            r = build_insert(spec, &b, &bind_count);
            break;
        case C2SQL_QB_UPSERT:
            r = build_upsert(spec, &b, &bind_count);
            break;
        case C2SQL_QB_SELECT:
            r = build_select(spec, &b, &bind_count);
            break;
        case C2SQL_QB_COUNT:
            r = build_count(spec, &b, &bind_count);
            break;
        case C2SQL_QB_DELETE:
            r = build_delete(spec, &b, &bind_count);
            break;
        case C2SQL_QB_UPDATE_FIELD:
            r = build_update_field(spec, &b, &bind_count);
            break;
        case C2SQL_QB_SELECT_FIELD:
            r = build_select_field(spec, &b, &bind_count);
            break;
        default:
            r = SQL_RDB_ERR_INVALID_ARG;
            break;
    }

    if (r != SQL_RDB_OK) {
        buf_free(&b);
        return r;
    }

    *out_sql = b.data;  /* transfer ownership */
    if (out_bind_count) *out_bind_count = bind_count;
    return SQL_RDB_OK;
}
