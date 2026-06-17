/*
 * condition_ast.h — Internal struct definition for SqlRDBCondition.
 *
 * The public header (c2sql.h) forward-declares SqlRDBCondition as opaque.
 * This header provides the complete definition for internal use.
 *
 * String/pointer values in leaf nodes are NOT owned by the node;
 * the caller must keep them alive until the condition is freed.
 */
#ifndef C2SQL_CONDITION_AST_H
#define C2SQL_CONDITION_AST_H

#include "c2sql.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
    COND_LEAF,    /* column op value */
    COND_AND,     /* left AND right  */
    COND_OR,      /* left OR right   */
    COND_ALL      /* match-all sentinel (no WHERE clause) */
} C2SqlCondKind;

struct SqlRDBCondition {
    C2SqlCondKind kind;
    union {
        struct {
            const char  *col;
            SqlRDBOp     op;
            SqlRDBType   value_type;
            union {
                int64_t     i;
                double      r;
                const char *t;
                struct { const void *p; size_t n; } b;
            } v;
        } leaf;
        struct {
            struct SqlRDBCondition *left;
            struct SqlRDBCondition *right;
        } composite;
    } u;
};

#endif /* C2SQL_CONDITION_AST_H */
