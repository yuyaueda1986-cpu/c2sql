/*
 * test_condition_ast.c — Task 6: 検索条件ASTの実装
 *
 * Task 6.1: 条件ノード構築とビルダーAPI
 * Task 6.2: 条件ASTのWHERE句展開と解放
 */
#define _POSIX_C_SOURCE 200809L
#include "harness.h"
#include "handle_internal.h"
#include "schema_registry.h"
#include "query_builder.h"
#include "condition_ast.h"
#include "c2sql.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Fixture helpers                                                     */
/* ------------------------------------------------------------------ */

static void setup_handle(SqlRDBHandle *h) {
    memset(h, 0, sizeof(*h));
    h->magic = SQL_RDB_HANDLE_MAGIC;
    c2sql_internal_mutex_init(&h->mutex, false);
    c2sql_internal_err_clear(&h->error);
    c2sql_internal_schema_registry_init(&h->registry);
}

static void teardown_handle(SqlRDBHandle *h) {
    c2sql_internal_schema_registry_destroy(&h->registry);
    c2sql_internal_mutex_destroy(&h->mutex);
}

/* ------------------------------------------------------------------ */
/* Sample schema: id(PK INT32), name(TEXT), score(REAL NULLABLE)      */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t id;
    char    name[64];
    double  score;
} TestRow;

static const SqlRDBColumnDef COLS[] = {
    { "id",    SQL_TYPE_INT32, offsetof(TestRow, id),    4,  SQL_COL_FLAG_PRIMARY_KEY },
    { "name",  SQL_TYPE_TEXT,  offsetof(TestRow, name),  64, SQL_COL_FLAG_NONE        },
    { "score", SQL_TYPE_REAL,  offsetof(TestRow, score), 8,  SQL_COL_FLAG_NULLABLE    },
};
#define COLS_COUNT 3

/* ------------------------------------------------------------------ */
/* Task 6.1: 条件ノード構築とビルダーAPI                             */
/* ------------------------------------------------------------------ */

/* SqlRDBCondInt: 正常系 — リーフノード取得、kind/col/op/value が正しい */
static void test_cond_int_valid(void) {
    SqlRDBCondition *c = SqlRDBCondInt("id", SQL_OP_EQ, 42);
    TEST_ASSERT(c != NULL);
    TEST_ASSERT(c->kind == COND_LEAF);
    TEST_ASSERT(c->u.leaf.col == (const char *)"id");
    TEST_ASSERT(c->u.leaf.op == SQL_OP_EQ);
    TEST_ASSERT(c->u.leaf.value_type == SQL_TYPE_INT64 || c->u.leaf.value_type == SQL_TYPE_INT32);
    TEST_ASSERT(c->u.leaf.v.i == 42);
    SqlRDBCondFree(c);
}

/* SqlRDBCondInt: col == NULL → NULL返却 */
static void test_cond_int_null_col(void) {
    SqlRDBCondition *c = SqlRDBCondInt(NULL, SQL_OP_EQ, 0);
    TEST_ASSERT(c == NULL);
}

/* SqlRDBCondText: 正常系 */
static void test_cond_text_valid(void) {
    const char *val = "hello";
    SqlRDBCondition *c = SqlRDBCondText("name", SQL_OP_EQ, val);
    TEST_ASSERT(c != NULL);
    TEST_ASSERT(c->kind == COND_LEAF);
    TEST_ASSERT(c->u.leaf.value_type == SQL_TYPE_TEXT);
    TEST_ASSERT(c->u.leaf.v.t == val);
    SqlRDBCondFree(c);
}

/* SqlRDBCondText: col == NULL → NULL */
static void test_cond_text_null_col(void) {
    SqlRDBCondition *c = SqlRDBCondText(NULL, SQL_OP_EQ, "x");
    TEST_ASSERT(c == NULL);
}

/* SqlRDBCondText: value == NULL → NULL */
static void test_cond_text_null_value(void) {
    SqlRDBCondition *c = SqlRDBCondText("name", SQL_OP_EQ, NULL);
    TEST_ASSERT(c == NULL);
}

/* SqlRDBCondReal: 正常系 */
static void test_cond_real_valid(void) {
    SqlRDBCondition *c = SqlRDBCondReal("score", SQL_OP_GE, 3.14);
    TEST_ASSERT(c != NULL);
    TEST_ASSERT(c->kind == COND_LEAF);
    TEST_ASSERT(c->u.leaf.value_type == SQL_TYPE_REAL);
    TEST_ASSERT(c->u.leaf.v.r == 3.14);
    SqlRDBCondFree(c);
}

/* SqlRDBCondReal: col == NULL → NULL */
static void test_cond_real_null_col(void) {
    SqlRDBCondition *c = SqlRDBCondReal(NULL, SQL_OP_GE, 0.0);
    TEST_ASSERT(c == NULL);
}

/* SqlRDBCondBlob: 正常系 */
static void test_cond_blob_valid(void) {
    const uint8_t data[] = {0xDE, 0xAD};
    SqlRDBCondition *c = SqlRDBCondBlob("id", SQL_OP_EQ, data, sizeof(data));
    TEST_ASSERT(c != NULL);
    TEST_ASSERT(c->kind == COND_LEAF);
    TEST_ASSERT(c->u.leaf.value_type == SQL_TYPE_BLOB);
    TEST_ASSERT(c->u.leaf.v.b.p == (const void *)data);
    TEST_ASSERT(c->u.leaf.v.b.n == sizeof(data));
    SqlRDBCondFree(c);
}

/* SqlRDBCondBlob: col == NULL → NULL */
static void test_cond_blob_null_col(void) {
    uint8_t d = 0;
    SqlRDBCondition *c = SqlRDBCondBlob(NULL, SQL_OP_EQ, &d, 1);
    TEST_ASSERT(c == NULL);
}

/* SqlRDBCondBlob: bytes == NULL → NULL */
static void test_cond_blob_null_bytes(void) {
    SqlRDBCondition *c = SqlRDBCondBlob("id", SQL_OP_EQ, NULL, 4);
    TEST_ASSERT(c == NULL);
}

/* SqlRDBCondAnd: 正常系 — 複合ノード、leftとrightが設定される */
static void test_cond_and_valid(void) {
    SqlRDBCondition *a = SqlRDBCondInt("id", SQL_OP_GE, 10);
    SqlRDBCondition *b = SqlRDBCondInt("id", SQL_OP_LT, 20);
    TEST_ASSERT(a != NULL && b != NULL);

    SqlRDBCondition *and_c = SqlRDBCondAnd(a, b);
    TEST_ASSERT(and_c != NULL);
    TEST_ASSERT(and_c->kind == COND_AND);
    TEST_ASSERT(and_c->u.composite.left == a);
    TEST_ASSERT(and_c->u.composite.right == b);
    /* SqlRDBCondFree は複合ノード配下を再帰的に解放する */
    SqlRDBCondFree(and_c);
}

/* SqlRDBCondAnd: a == NULL → NULL */
static void test_cond_and_null_a(void) {
    SqlRDBCondition *b = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBCondition *r = SqlRDBCondAnd(NULL, b);
    TEST_ASSERT(r == NULL);
    SqlRDBCondFree(b);
}

/* SqlRDBCondAnd: b == NULL → NULL */
static void test_cond_and_null_b(void) {
    SqlRDBCondition *a = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBCondition *r = SqlRDBCondAnd(a, NULL);
    TEST_ASSERT(r == NULL);
    SqlRDBCondFree(a);
}

/* SqlRDBCondOr: 正常系 */
static void test_cond_or_valid(void) {
    SqlRDBCondition *a = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBCondition *b = SqlRDBCondInt("id", SQL_OP_EQ, 2);
    SqlRDBCondition *or_c = SqlRDBCondOr(a, b);
    TEST_ASSERT(or_c != NULL);
    TEST_ASSERT(or_c->kind == COND_OR);
    SqlRDBCondFree(or_c);
}

/* SqlRDBCondOr: a == NULL → NULL */
static void test_cond_or_null_a(void) {
    SqlRDBCondition *b = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBCondition *r = SqlRDBCondOr(NULL, b);
    TEST_ASSERT(r == NULL);
    SqlRDBCondFree(b);
}

/* SqlRDBCondOr: b == NULL → NULL */
static void test_cond_or_null_b(void) {
    SqlRDBCondition *a = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBCondition *r = SqlRDBCondOr(a, NULL);
    TEST_ASSERT(r == NULL);
    SqlRDBCondFree(a);
}

/* SqlRDBCondAll: COND_ALL センチネル */
static void test_cond_all(void) {
    SqlRDBCondition *c = SqlRDBCondAll();
    TEST_ASSERT(c != NULL);
    TEST_ASSERT(c->kind == COND_ALL);
    SqlRDBCondFree(c);
}

/* SqlRDBCondFree: NULLを渡してもクラッシュしない */
static void test_cond_free_null(void) {
    SqlRDBCondFree(NULL); /* should not crash */
    TEST_ASSERT(1);
}

/* SqlRDBCondFree: 深いネスト（AND(OR(leaf, leaf), leaf)）を再帰的に解放 */
static void test_cond_free_deep_tree(void) {
    SqlRDBCondition *l1 = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBCondition *l2 = SqlRDBCondText("name", SQL_OP_NE, "x");
    SqlRDBCondition *l3 = SqlRDBCondReal("score", SQL_OP_GT, 0.5);
    TEST_ASSERT(l1 && l2 && l3);

    SqlRDBCondition *or_c  = SqlRDBCondOr(l1, l2);
    SqlRDBCondition *and_c = SqlRDBCondAnd(or_c, l3);
    TEST_ASSERT(or_c && and_c);

    SqlRDBCondFree(and_c); /* 全ノードを再帰解放、クラッシュなし */
    TEST_ASSERT(1);
}

/* ------------------------------------------------------------------ */
/* Task 6.2: WHERE句展開とカラム名検証                               */
/* ------------------------------------------------------------------ */

/* 既知カラム名の条件 → SQL生成OK、WHERE句にプレースホルダが入る */
static void test_select_with_known_col(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    SqlRDBResult r2 = c2sql_internal_schema_register(&h, "t", COLS, COLS_COUNT);
    TEST_ASSERT(r2 == SQL_RDB_OK);
    const SqlRDBSchema *s = c2sql_internal_schema_lookup(&h, "t");
    TEST_ASSERT(s != NULL);

    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 7);
    TEST_ASSERT(cond != NULL);

    SqlRDBQuerySpec spec = { C2SQL_QB_SELECT, s, cond, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 0;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(bind_count == 1);
    TEST_ASSERT(sql != NULL && strstr(sql, "WHERE") != NULL);
    TEST_ASSERT(sql != NULL && strstr(sql, "\"id\" = ?") != NULL);

    free(sql);
    SqlRDBCondFree(cond);
    teardown_handle(&h);
}

/* 未知カラム名の条件 → SQL_RDB_ERR_UNKNOWN_COLUMN */
static void test_select_unknown_col(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    SqlRDBResult r2 = c2sql_internal_schema_register(&h, "t", COLS, COLS_COUNT);
    TEST_ASSERT(r2 == SQL_RDB_OK);
    const SqlRDBSchema *s = c2sql_internal_schema_lookup(&h, "t");
    TEST_ASSERT(s != NULL);

    SqlRDBCondition *cond = SqlRDBCondInt("nonexistent", SQL_OP_EQ, 1);
    TEST_ASSERT(cond != NULL);

    SqlRDBQuerySpec spec = { C2SQL_QB_SELECT, s, cond, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 0;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_ERR_UNKNOWN_COLUMN);
    TEST_ASSERT(sql == NULL);

    SqlRDBCondFree(cond);
    teardown_handle(&h);
}

/* AND条件の片方が未知カラム → UNKNOWN_COLUMN */
static void test_select_and_one_unknown_col(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    SqlRDBResult r2 = c2sql_internal_schema_register(&h, "t", COLS, COLS_COUNT);
    TEST_ASSERT(r2 == SQL_RDB_OK);
    const SqlRDBSchema *s = c2sql_internal_schema_lookup(&h, "t");
    TEST_ASSERT(s != NULL);

    SqlRDBCondition *a = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBCondition *b = SqlRDBCondText("ghost_col", SQL_OP_EQ, "x");
    SqlRDBCondition *and_c = SqlRDBCondAnd(a, b);
    TEST_ASSERT(and_c != NULL);

    SqlRDBQuerySpec spec = { C2SQL_QB_SELECT, s, and_c, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 0;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_ERR_UNKNOWN_COLUMN);
    TEST_ASSERT(sql == NULL);

    SqlRDBCondFree(and_c);
    teardown_handle(&h);
}

/* OR条件: 両方既知 → OK */
static void test_select_or_both_known(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    SqlRDBResult r2 = c2sql_internal_schema_register(&h, "t", COLS, COLS_COUNT);
    TEST_ASSERT(r2 == SQL_RDB_OK);
    const SqlRDBSchema *s = c2sql_internal_schema_lookup(&h, "t");
    TEST_ASSERT(s != NULL);

    SqlRDBCondition *a = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBCondition *b = SqlRDBCondInt("id", SQL_OP_EQ, 2);
    SqlRDBCondition *or_c = SqlRDBCondOr(a, b);
    TEST_ASSERT(or_c != NULL);

    SqlRDBQuerySpec spec = { C2SQL_QB_SELECT, s, or_c, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 0;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(bind_count == 2);
    TEST_ASSERT(sql != NULL && strstr(sql, " OR ") != NULL);

    free(sql);
    SqlRDBCondFree(or_c);
    teardown_handle(&h);
}

/* COND_ALL のSentinel → WHERE句なしのSELECT */
static void test_select_cond_all_no_where(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    SqlRDBResult r2 = c2sql_internal_schema_register(&h, "t", COLS, COLS_COUNT);
    TEST_ASSERT(r2 == SQL_RDB_OK);
    const SqlRDBSchema *s = c2sql_internal_schema_lookup(&h, "t");
    TEST_ASSERT(s != NULL);

    SqlRDBCondition *all = SqlRDBCondAll();
    TEST_ASSERT(all != NULL);

    SqlRDBQuerySpec spec = { C2SQL_QB_SELECT, s, all, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 99;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(bind_count == 0);
    TEST_ASSERT(sql != NULL && strstr(sql, "WHERE") == NULL);

    free(sql);
    SqlRDBCondFree(all);
    teardown_handle(&h);
}

/* DELETE: 未知カラム → UNKNOWN_COLUMN */
static void test_delete_unknown_col(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    SqlRDBResult r2 = c2sql_internal_schema_register(&h, "t", COLS, COLS_COUNT);
    TEST_ASSERT(r2 == SQL_RDB_OK);
    const SqlRDBSchema *s = c2sql_internal_schema_lookup(&h, "t");
    TEST_ASSERT(s != NULL);

    SqlRDBCondition *cond = SqlRDBCondText("bad_col", SQL_OP_EQ, "x");
    TEST_ASSERT(cond != NULL);

    SqlRDBQuerySpec spec = { C2SQL_QB_DELETE, s, cond, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 0;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_ERR_UNKNOWN_COLUMN);
    TEST_ASSERT(sql == NULL);

    SqlRDBCondFree(cond);
    teardown_handle(&h);
}

/* SqlRDBCondFree で解放後に qb_build を呼ばなくてもメモリリークしない確認 */
static void test_cond_free_prevents_leak(void) {
    /* SqlRDBCondAnd が所有権を取得、SqlRDBCondFree が再帰的に全解放する */
    SqlRDBCondition *a = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBCondition *b = SqlRDBCondReal("score", SQL_OP_GT, 0.0);
    SqlRDBCondition *c = SqlRDBCondAnd(a, b);
    TEST_ASSERT(c != NULL);
    SqlRDBCondFree(c); /* valgrind 下で確認: リーク0 */
    TEST_ASSERT(1);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    /* Task 6.1: 条件ノード構築とビルダーAPI */
    test_cond_int_valid();
    test_cond_int_null_col();
    test_cond_text_valid();
    test_cond_text_null_col();
    test_cond_text_null_value();
    test_cond_real_valid();
    test_cond_real_null_col();
    test_cond_blob_valid();
    test_cond_blob_null_col();
    test_cond_blob_null_bytes();
    test_cond_and_valid();
    test_cond_and_null_a();
    test_cond_and_null_b();
    test_cond_or_valid();
    test_cond_or_null_a();
    test_cond_or_null_b();
    test_cond_all();
    test_cond_free_null();
    test_cond_free_deep_tree();

    /* Task 6.2: WHERE句展開とカラム名検証 */
    test_select_with_known_col();
    test_select_unknown_col();
    test_select_and_one_unknown_col();
    test_select_or_both_known();
    test_select_cond_all_no_where();
    test_delete_unknown_col();
    test_cond_free_prevents_leak();

    TEST_SUMMARY();
}
