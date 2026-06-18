/*
 * test_query_builder.c — Task 5: クエリビルダーの実装
 *
 * Task 5.1: DDL生成（CREATE TABLE / ALTER TABLE）
 * Task 5.2: DML生成（INSERT / UPSERT / SELECT / DELETE）
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

/* Register a schema and return it; aborts test if registration fails. */
static const SqlRDBSchema *register_and_lookup(
    SqlRDBHandle         *h,
    const char           *name,
    const SqlRDBColumnDef *cols,
    size_t                n)
{
    SqlRDBResult r = c2sql_internal_schema_register(h, name, cols, n);
    if (r != SQL_RDB_OK) return NULL;
    return c2sql_internal_schema_lookup(h, name);
}

/* ------------------------------------------------------------------ */
/* Sample struct layouts and column definitions                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t id;
    char    name[64];
    double  score;
} RowWithPK;

typedef struct {
    int64_t val;
    double  rating;
} RowNoPK;

typedef struct {
    int32_t id;
    char    email[128];
    int64_t ts;
} RowUnique;

/* Schema: id(PK INT32), name(TEXT NOT NULL), score(REAL NULLABLE) */
static const SqlRDBColumnDef COLS_PK[] = {
    { "id",    SQL_TYPE_INT32, offsetof(RowWithPK, id),    4,  SQL_COL_FLAG_PRIMARY_KEY },
    { "name",  SQL_TYPE_TEXT,  offsetof(RowWithPK, name),  64, SQL_COL_FLAG_NONE        },
    { "score", SQL_TYPE_REAL,  offsetof(RowWithPK, score), 8,  SQL_COL_FLAG_NULLABLE    },
};
#define COLS_PK_COUNT 3

/* Schema: val(INT64 NOT NULL), rating(REAL NOT NULL) — no PK */
static const SqlRDBColumnDef COLS_NO_PK[] = {
    { "val",    SQL_TYPE_INT64, offsetof(RowNoPK, val),    8, SQL_COL_FLAG_NONE },
    { "rating", SQL_TYPE_REAL,  offsetof(RowNoPK, rating), 8, SQL_COL_FLAG_NONE },
};
#define COLS_NO_PK_COUNT 2

/* Schema: id(PK INT32), email(TEXT UNIQUE NULLABLE), ts(INT64 NOT NULL) */
static const SqlRDBColumnDef COLS_UNIQUE[] = {
    { "id",    SQL_TYPE_INT32, offsetof(RowUnique, id),    4,   SQL_COL_FLAG_PRIMARY_KEY               },
    { "email", SQL_TYPE_TEXT,  offsetof(RowUnique, email), 128, SQL_COL_FLAG_UNIQUE | SQL_COL_FLAG_NULLABLE },
    { "ts",    SQL_TYPE_INT64, offsetof(RowUnique, ts),    8,   SQL_COL_FLAG_NONE                      },
};
#define COLS_UNIQUE_COUNT 3

/* ------------------------------------------------------------------ */
/* Task 5.1: DDL生成                                                   */
/* ------------------------------------------------------------------ */

/* CREATE TABLE: PK, NOT NULL, NULLABLE の各制約が正しく反映される */
static void test_create_table_pk_and_nullable(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBQuerySpec spec = { C2SQL_QB_CREATE, s, NULL, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 99;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(sql != NULL);
    TEST_ASSERT(bind_count == 0);

    const char *expected =
        "CREATE TABLE IF NOT EXISTS \"events\" ("
        "\"id\" INTEGER NOT NULL PRIMARY KEY,"
        "\"name\" TEXT NOT NULL,"
        "\"score\" REAL"
        ") STRICT";
    if (sql && strcmp(sql, expected) != 0) {
        fprintf(stderr, "  GOT: %s\n  EXP: %s\n", sql, expected);
    }
    TEST_ASSERT(sql && strcmp(sql, expected) == 0);

    free(sql);
    teardown_handle(&h);
}

/* CREATE TABLE: UNIQUE制約が付く */
static void test_create_table_unique(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "users", COLS_UNIQUE, COLS_UNIQUE_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBQuerySpec spec = { C2SQL_QB_CREATE, s, NULL, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 99;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(bind_count == 0);

    const char *expected =
        "CREATE TABLE IF NOT EXISTS \"users\" ("
        "\"id\" INTEGER NOT NULL PRIMARY KEY,"
        "\"email\" TEXT UNIQUE,"
        "\"ts\" INTEGER NOT NULL"
        ") STRICT";
    if (sql && strcmp(sql, expected) != 0) {
        fprintf(stderr, "  GOT: %s\n  EXP: %s\n", sql, expected);
    }
    TEST_ASSERT(sql && strcmp(sql, expected) == 0);

    free(sql);
    teardown_handle(&h);
}

/* CREATE TABLE: PKなし */
static void test_create_table_no_pk(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "items", COLS_NO_PK, COLS_NO_PK_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBQuerySpec spec = { C2SQL_QB_CREATE, s, NULL, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 0;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);

    const char *expected =
        "CREATE TABLE IF NOT EXISTS \"items\" ("
        "\"val\" INTEGER NOT NULL,"
        "\"rating\" REAL NOT NULL"
        ") STRICT";
    if (sql && strcmp(sql, expected) != 0) {
        fprintf(stderr, "  GOT: %s\n  EXP: %s\n", sql, expected);
    }
    TEST_ASSERT(sql && strcmp(sql, expected) == 0);

    free(sql);
    teardown_handle(&h);
}

/* ALTER TABLE ADD COLUMN: NOT NULL付き */
static void test_alter_add_column_not_null(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBColumnDef new_col = { "priority", SQL_TYPE_INT32, 0, 4, SQL_COL_FLAG_NONE };
    SqlRDBQuerySpec spec = { C2SQL_QB_ALTER_ADD, s, NULL, &new_col, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 99;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(bind_count == 0);

    const char *expected =
        "ALTER TABLE \"events\" ADD COLUMN \"priority\" INTEGER NOT NULL";
    if (sql && strcmp(sql, expected) != 0) {
        fprintf(stderr, "  GOT: %s\n  EXP: %s\n", sql, expected);
    }
    TEST_ASSERT(sql && strcmp(sql, expected) == 0);

    free(sql);
    teardown_handle(&h);
}

/* ALTER TABLE ADD COLUMN: NULLABLE */
static void test_alter_add_column_nullable(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBColumnDef new_col = { "note", SQL_TYPE_TEXT, 0, 256, SQL_COL_FLAG_NULLABLE };
    SqlRDBQuerySpec spec = { C2SQL_QB_ALTER_ADD, s, NULL, &new_col, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 0;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);

    const char *expected =
        "ALTER TABLE \"events\" ADD COLUMN \"note\" TEXT";
    if (sql && strcmp(sql, expected) != 0) {
        fprintf(stderr, "  GOT: %s\n  EXP: %s\n", sql, expected);
    }
    TEST_ASSERT(sql && strcmp(sql, expected) == 0);

    free(sql);
    teardown_handle(&h);
}

/* ------------------------------------------------------------------ */
/* Task 5.2: DML生成                                                   */
/* ------------------------------------------------------------------ */

/* INSERT: PKなしのスキーマ → プレーンINSERT */
static void test_insert_no_pk(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "items", COLS_NO_PK, COLS_NO_PK_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBQuerySpec spec = { C2SQL_QB_INSERT, s, NULL, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 0;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(bind_count == COLS_NO_PK_COUNT);

    const char *expected =
        "INSERT INTO \"items\" (\"val\",\"rating\") VALUES (?,?)";
    if (sql && strcmp(sql, expected) != 0) {
        fprintf(stderr, "  GOT: %s\n  EXP: %s\n", sql, expected);
    }
    TEST_ASSERT(sql && strcmp(sql, expected) == 0);

    free(sql);
    teardown_handle(&h);
}

/* UPSERT: PKありのスキーマ → ON CONFLICT DO UPDATE */
static void test_upsert_with_pk(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBQuerySpec spec = { C2SQL_QB_UPSERT, s, NULL, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 0;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(bind_count == COLS_PK_COUNT);

    const char *expected =
        "INSERT INTO \"events\" (\"id\",\"name\",\"score\") VALUES (?,?,?)"
        " ON CONFLICT(\"id\") DO UPDATE SET "
        "\"name\"=excluded.\"name\","
        "\"score\"=excluded.\"score\"";
    if (sql && strcmp(sql, expected) != 0) {
        fprintf(stderr, "  GOT: %s\n  EXP: %s\n", sql, expected);
    }
    TEST_ASSERT(sql && strcmp(sql, expected) == 0);

    free(sql);
    teardown_handle(&h);
}

/* SELECT: condがNULL → WHEREなし */
static void test_select_no_cond(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBQuerySpec spec = { C2SQL_QB_SELECT, s, NULL, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 99;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(bind_count == 0);

    const char *expected =
        "SELECT \"id\",\"name\",\"score\" FROM \"events\"";
    if (sql && strcmp(sql, expected) != 0) {
        fprintf(stderr, "  GOT: %s\n  EXP: %s\n", sql, expected);
    }
    TEST_ASSERT(sql && strcmp(sql, expected) == 0);

    free(sql);
    teardown_handle(&h);
}

/* SELECT: COND_ALL → WHEREなし */
static void test_select_cond_all(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBCondition all_cond = { .kind = COND_ALL };
    SqlRDBQuerySpec spec = { C2SQL_QB_SELECT, s, &all_cond, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 99;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(bind_count == 0);

    const char *expected =
        "SELECT \"id\",\"name\",\"score\" FROM \"events\"";
    TEST_ASSERT(sql && strcmp(sql, expected) == 0);

    free(sql);
    teardown_handle(&h);
}

/* SELECT: リーフ条件EQ → WHERE句に1プレースホルダ */
static void test_select_leaf_eq(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBCondition leaf = {
        .kind = COND_LEAF,
        .u.leaf = { .col = "id", .op = SQL_OP_EQ, .value_type = SQL_TYPE_INT32, .v = { .i = 1 } }
    };
    SqlRDBQuerySpec spec = { C2SQL_QB_SELECT, s, &leaf, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 0;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(bind_count == 1);

    const char *expected =
        "SELECT \"id\",\"name\",\"score\" FROM \"events\" WHERE \"id\" = ?";
    if (sql && strcmp(sql, expected) != 0) {
        fprintf(stderr, "  GOT: %s\n  EXP: %s\n", sql, expected);
    }
    TEST_ASSERT(sql && strcmp(sql, expected) == 0);

    free(sql);
    teardown_handle(&h);
}

/* SELECT: AND条件 → WHERE句に2プレースホルダ */
static void test_select_and_cond(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBCondition left = {
        .kind = COND_LEAF,
        .u.leaf = { .col = "id", .op = SQL_OP_GE, .value_type = SQL_TYPE_INT32, .v = { .i = 10 } }
    };
    SqlRDBCondition right = {
        .kind = COND_LEAF,
        .u.leaf = { .col = "id", .op = SQL_OP_LT, .value_type = SQL_TYPE_INT32, .v = { .i = 20 } }
    };
    SqlRDBCondition and_cond = {
        .kind = COND_AND,
        .u.composite = { .left = &left, .right = &right }
    };
    SqlRDBQuerySpec spec = { C2SQL_QB_SELECT, s, &and_cond, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 0;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(bind_count == 2);

    const char *expected =
        "SELECT \"id\",\"name\",\"score\" FROM \"events\""
        " WHERE (\"id\" >= ? AND \"id\" < ?)";
    if (sql && strcmp(sql, expected) != 0) {
        fprintf(stderr, "  GOT: %s\n  EXP: %s\n", sql, expected);
    }
    TEST_ASSERT(sql && strcmp(sql, expected) == 0);

    free(sql);
    teardown_handle(&h);
}

/* SELECT: OR条件 */
static void test_select_or_cond(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBCondition left = {
        .kind = COND_LEAF,
        .u.leaf = { .col = "id", .op = SQL_OP_EQ, .value_type = SQL_TYPE_INT32, .v = { .i = 1 } }
    };
    SqlRDBCondition right = {
        .kind = COND_LEAF,
        .u.leaf = { .col = "id", .op = SQL_OP_EQ, .value_type = SQL_TYPE_INT32, .v = { .i = 2 } }
    };
    SqlRDBCondition or_cond = {
        .kind = COND_OR,
        .u.composite = { .left = &left, .right = &right }
    };
    SqlRDBQuerySpec spec = { C2SQL_QB_SELECT, s, &or_cond, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 0;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(bind_count == 2);

    const char *expected =
        "SELECT \"id\",\"name\",\"score\" FROM \"events\""
        " WHERE (\"id\" = ? OR \"id\" = ?)";
    if (sql && strcmp(sql, expected) != 0) {
        fprintf(stderr, "  GOT: %s\n  EXP: %s\n", sql, expected);
    }
    TEST_ASSERT(sql && strcmp(sql, expected) == 0);

    free(sql);
    teardown_handle(&h);
}

/* SELECT: 各比較演算子が正しくSQLに変換される */
static void test_select_all_operators(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);

    struct { SqlRDBOp op; const char *op_str; } cases[] = {
        { SQL_OP_EQ, "=" },
        { SQL_OP_NE, "!=" },
        { SQL_OP_LT, "<" },
        { SQL_OP_LE, "<=" },
        { SQL_OP_GT, ">" },
        { SQL_OP_GE, ">=" },
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        SqlRDBCondition leaf = {
            .kind = COND_LEAF,
            .u.leaf = { .col = "id", .op = cases[i].op,
                        .value_type = SQL_TYPE_INT32, .v = { .i = 0 } }
        };
        SqlRDBQuerySpec spec = { C2SQL_QB_SELECT, s, &leaf, NULL, NULL, C2SQL_DIALECT_SQLITE };
        char   *sql        = NULL;
        size_t  bind_count = 0;

        SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
        TEST_ASSERT(r == SQL_RDB_OK);
        TEST_ASSERT(bind_count == 1);
        /* Verify the operator appears in the WHERE clause */
        if (sql) {
            char needle[64];
            snprintf(needle, sizeof(needle), "\"id\" %s ?", cases[i].op_str);
            TEST_ASSERT(strstr(sql, needle) != NULL);
            free(sql);
        }
    }

    teardown_handle(&h);
}

/* DELETE: リーフ条件 */
static void test_delete_leaf(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBCondition leaf = {
        .kind = COND_LEAF,
        .u.leaf = { .col = "id", .op = SQL_OP_EQ, .value_type = SQL_TYPE_INT32, .v = { .i = 1 } }
    };
    SqlRDBQuerySpec spec = { C2SQL_QB_DELETE, s, &leaf, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 0;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(bind_count == 1);

    const char *expected =
        "DELETE FROM \"events\" WHERE \"id\" = ?";
    if (sql && strcmp(sql, expected) != 0) {
        fprintf(stderr, "  GOT: %s\n  EXP: %s\n", sql, expected);
    }
    TEST_ASSERT(sql && strcmp(sql, expected) == 0);

    free(sql);
    teardown_handle(&h);
}

/* DELETE: COND_ALL → WHEREなし（全件削除） */
static void test_delete_all(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBCondition all_cond = { .kind = COND_ALL };
    SqlRDBQuerySpec spec = { C2SQL_QB_DELETE, s, &all_cond, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 99;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(bind_count == 0);

    const char *expected = "DELETE FROM \"events\"";
    if (sql && strcmp(sql, expected) != 0) {
        fprintf(stderr, "  GOT: %s\n  EXP: %s\n", sql, expected);
    }
    TEST_ASSERT(sql && strcmp(sql, expected) == 0);

    free(sql);
    teardown_handle(&h);
}

/* DELETE: condがNULL → WHEREなし */
static void test_delete_null_cond(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBQuerySpec spec = { C2SQL_QB_DELETE, s, NULL, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 99;

    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(bind_count == 0);

    const char *expected = "DELETE FROM \"events\"";
    TEST_ASSERT(sql && strcmp(sql, expected) == 0);

    free(sql);
    teardown_handle(&h);
}

/* ------------------------------------------------------------------ */
/* エラーケース                                                        */
/* ------------------------------------------------------------------ */

/* NULLスペック → INVALID_ARG */
static void test_null_spec(void) {
    char   *sql        = NULL;
    size_t  bind_count = 0;
    SqlRDBResult r = c2sql_internal_qb_build(NULL, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    TEST_ASSERT(sql == NULL);
}

/* NULLスキーマ → INVALID_ARG */
static void test_null_schema(void) {
    SqlRDBQuerySpec spec = { C2SQL_QB_CREATE, NULL, NULL, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 0;
    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    TEST_ASSERT(sql == NULL);
}

/* NULLのout_sql → INVALID_ARG */
static void test_null_out_sql(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBQuerySpec spec = { C2SQL_QB_CREATE, s, NULL, NULL, NULL, C2SQL_DIALECT_SQLITE };
    size_t bind_count = 0;
    SqlRDBResult r = c2sql_internal_qb_build(&spec, NULL, &bind_count);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);

    teardown_handle(&h);
}

/* ALTER_ADD: new_colがNULL → INVALID_ARG */
static void test_alter_add_null_new_col(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);

    SqlRDBQuerySpec spec = { C2SQL_QB_ALTER_ADD, s, NULL, NULL, NULL, C2SQL_DIALECT_SQLITE };
    char   *sql        = NULL;
    size_t  bind_count = 0;
    SqlRDBResult r = c2sql_internal_qb_build(&spec, &sql, &bind_count);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    TEST_ASSERT(sql == NULL);

    teardown_handle(&h);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* PostgreSQL dialect SQL generation                                   */
/* ------------------------------------------------------------------ */

static void expect_sql(const SqlRDBQuerySpec *spec, const char *expected) {
    char *sql = NULL;
    SqlRDBResult r = c2sql_internal_qb_build(spec, &sql, NULL);
    TEST_ASSERT(r == SQL_RDB_OK);
    if (sql && strcmp(sql, expected) != 0) {
        fprintf(stderr, "  GOT: %s\n  EXP: %s\n", sql, expected);
    }
    TEST_ASSERT(sql && strcmp(sql, expected) == 0);
    free(sql);
}

/* CREATE: no STRICT suffix; INT32->INTEGER, INT64->BIGINT, REAL->DOUBLE PRECISION */
static void test_pg_create_types(void) {
    SqlRDBHandle h; setup_handle(&h);
    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);
    SqlRDBQuerySpec spec = { C2SQL_QB_CREATE, s, NULL, NULL, NULL, C2SQL_DIALECT_POSTGRES };
    expect_sql(&spec,
        "CREATE TABLE IF NOT EXISTS \"events\" ("
        "\"id\" INTEGER NOT NULL PRIMARY KEY,"
        "\"name\" TEXT NOT NULL,"
        "\"score\" DOUBLE PRECISION)");
    teardown_handle(&h);
}

static void test_pg_create_int64_blob(void) {
    SqlRDBHandle h; setup_handle(&h);
    const SqlRDBSchema *s = register_and_lookup(&h, "nums", COLS_NO_PK, COLS_NO_PK_COUNT);
    TEST_ASSERT(s != NULL);
    SqlRDBQuerySpec spec = { C2SQL_QB_CREATE, s, NULL, NULL, NULL, C2SQL_DIALECT_POSTGRES };
    expect_sql(&spec,
        "CREATE TABLE IF NOT EXISTS \"nums\" ("
        "\"val\" BIGINT NOT NULL,"
        "\"rating\" DOUBLE PRECISION NOT NULL)");
    teardown_handle(&h);
}

/* INSERT uses $1.. placeholders */
static void test_pg_insert_placeholders(void) {
    SqlRDBHandle h; setup_handle(&h);
    const SqlRDBSchema *s = register_and_lookup(&h, "nums", COLS_NO_PK, COLS_NO_PK_COUNT);
    TEST_ASSERT(s != NULL);
    SqlRDBQuerySpec spec = { C2SQL_QB_INSERT, s, NULL, NULL, NULL, C2SQL_DIALECT_POSTGRES };
    expect_sql(&spec, "INSERT INTO \"nums\" (\"val\",\"rating\") VALUES ($1,$2)");
    teardown_handle(&h);
}

/* UPSERT: $1.. placeholders + ON CONFLICT ... excluded (portable syntax) */
static void test_pg_upsert(void) {
    SqlRDBHandle h; setup_handle(&h);
    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);
    SqlRDBQuerySpec spec = { C2SQL_QB_UPSERT, s, NULL, NULL, NULL, C2SQL_DIALECT_POSTGRES };
    expect_sql(&spec,
        "INSERT INTO \"events\" (\"id\",\"name\",\"score\") VALUES ($1,$2,$3) "
        "ON CONFLICT(\"id\") DO UPDATE SET "
        "\"name\"=excluded.\"name\",\"score\"=excluded.\"score\"");
    teardown_handle(&h);
}

/* WHERE placeholders are numbered sequentially across AND */
static void test_pg_select_and(void) {
    SqlRDBHandle h; setup_handle(&h);
    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);
    SqlRDBCondition *a = SqlRDBCondInt("id", SQL_OP_GE, 1);
    SqlRDBCondition *b = SqlRDBCondText("name", SQL_OP_NE, "x");
    SqlRDBCondition *and_c = SqlRDBCondAnd(a, b);
    SqlRDBQuerySpec spec = { C2SQL_QB_SELECT, s, and_c, NULL, NULL, C2SQL_DIALECT_POSTGRES };
    expect_sql(&spec,
        "SELECT \"id\",\"name\",\"score\" FROM \"events\" "
        "WHERE (\"id\" >= $1 AND \"name\" != $2)");
    SqlRDBCondFree(and_c);
    teardown_handle(&h);
}

/* UPDATE_FIELD: SET $1 then WHERE $2 */
static void test_pg_update_field(void) {
    SqlRDBHandle h; setup_handle(&h);
    const SqlRDBSchema *s = register_and_lookup(&h, "events", COLS_PK, COLS_PK_COUNT);
    TEST_ASSERT(s != NULL);
    SqlRDBCondition *k = SqlRDBCondInt("id", SQL_OP_EQ, 7);
    SqlRDBQuerySpec spec = { C2SQL_QB_UPDATE_FIELD, s, k, NULL, "name", C2SQL_DIALECT_POSTGRES };
    expect_sql(&spec, "UPDATE \"events\" SET \"name\"=$1 WHERE \"id\" = $2");
    SqlRDBCondFree(k);
    teardown_handle(&h);
}

int main(void) {
    /* Task 5.1: DDL生成 */
    test_create_table_pk_and_nullable();
    test_create_table_unique();
    test_create_table_no_pk();
    test_alter_add_column_not_null();
    test_alter_add_column_nullable();

    /* Task 5.2: DML生成 */
    test_insert_no_pk();
    test_upsert_with_pk();
    test_select_no_cond();
    test_select_cond_all();
    test_select_leaf_eq();
    test_select_and_cond();
    test_select_or_cond();
    test_select_all_operators();
    test_delete_leaf();
    test_delete_all();
    test_delete_null_cond();

    /* エラーケース */
    test_null_spec();
    test_null_schema();
    test_null_out_sql();
    test_alter_add_null_new_col();

    /* PostgreSQL dialect SQL generation */
    test_pg_create_types();
    test_pg_create_int64_blob();
    test_pg_insert_placeholders();
    test_pg_upsert();
    test_pg_select_and();
    test_pg_update_field();

    TEST_SUMMARY();
}
