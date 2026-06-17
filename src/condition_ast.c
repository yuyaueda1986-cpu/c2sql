/*
 * condition_ast.c — Public condition AST builder API for libc2sql.
 *
 * String/pointer values stored in leaf nodes are NOT owned by the node;
 * the caller must keep them alive until SqlRDBCondFree() is called on
 * any ancestor that includes this node.
 */
#include "condition_ast.h"
#include "c2sql.h"

#include <stdlib.h>

static SqlRDBCondition *alloc_node(void) {
    return calloc(1, sizeof(SqlRDBCondition));
}

/* ------------------------------------------------------------------ */
/* Leaf constructors                                                   */
/* ------------------------------------------------------------------ */

SqlRDBCondition *SqlRDBCondInt(const char *col, int op, int64_t value) {
    if (!col) return NULL;
    SqlRDBCondition *n = alloc_node();
    if (!n) return NULL;
    n->kind              = COND_LEAF;
    n->u.leaf.col        = col;
    n->u.leaf.op         = (SqlRDBOp)op;
    n->u.leaf.value_type = SQL_TYPE_INT64;
    n->u.leaf.v.i        = value;
    return n;
}

SqlRDBCondition *SqlRDBCondText(const char *col, int op, const char *value) {
    if (!col || !value) return NULL;
    SqlRDBCondition *n = alloc_node();
    if (!n) return NULL;
    n->kind              = COND_LEAF;
    n->u.leaf.col        = col;
    n->u.leaf.op         = (SqlRDBOp)op;
    n->u.leaf.value_type = SQL_TYPE_TEXT;
    n->u.leaf.v.t        = value;
    return n;
}

SqlRDBCondition *SqlRDBCondReal(const char *col, int op, double value) {
    if (!col) return NULL;
    SqlRDBCondition *n = alloc_node();
    if (!n) return NULL;
    n->kind              = COND_LEAF;
    n->u.leaf.col        = col;
    n->u.leaf.op         = (SqlRDBOp)op;
    n->u.leaf.value_type = SQL_TYPE_REAL;
    n->u.leaf.v.r        = value;
    return n;
}

SqlRDBCondition *SqlRDBCondBlob(const char *col, int op,
                                const void *bytes, size_t len) {
    if (!col || !bytes) return NULL;
    SqlRDBCondition *n = alloc_node();
    if (!n) return NULL;
    n->kind              = COND_LEAF;
    n->u.leaf.col        = col;
    n->u.leaf.op         = (SqlRDBOp)op;
    n->u.leaf.value_type = SQL_TYPE_BLOB;
    n->u.leaf.v.b.p      = bytes;
    n->u.leaf.v.b.n      = len;
    return n;
}

/* ------------------------------------------------------------------ */
/* Composite constructors                                              */
/* ------------------------------------------------------------------ */

SqlRDBCondition *SqlRDBCondAnd(SqlRDBCondition *a, SqlRDBCondition *b) {
    if (!a || !b) return NULL;
    SqlRDBCondition *n = alloc_node();
    if (!n) return NULL;
    n->kind                  = COND_AND;
    n->u.composite.left      = a;
    n->u.composite.right     = b;
    return n;
}

SqlRDBCondition *SqlRDBCondOr(SqlRDBCondition *a, SqlRDBCondition *b) {
    if (!a || !b) return NULL;
    SqlRDBCondition *n = alloc_node();
    if (!n) return NULL;
    n->kind                  = COND_OR;
    n->u.composite.left      = a;
    n->u.composite.right     = b;
    return n;
}

/* ------------------------------------------------------------------ */
/* Match-all sentinel                                                  */
/* ------------------------------------------------------------------ */

SqlRDBCondition *SqlRDBCondAll(void) {
    SqlRDBCondition *n = alloc_node();
    if (!n) return NULL;
    n->kind = COND_ALL;
    return n;
}

/* ------------------------------------------------------------------ */
/* Recursive free                                                      */
/* ------------------------------------------------------------------ */

void SqlRDBCondFree(SqlRDBCondition *cond) {
    if (!cond) return;
    if (cond->kind == COND_AND || cond->kind == COND_OR) {
        SqlRDBCondFree(cond->u.composite.left);
        SqlRDBCondFree(cond->u.composite.right);
    }
    free(cond);
}
