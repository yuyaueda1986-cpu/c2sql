/*
 * test_schema_register.c — Task 4: スキーマレジストリの実装
 *
 * Task 4.1: 構造体スキーマ登録と検証
 * Task 4.2: スキーマ検索とプライマリキー識別
 */
#include "harness.h"
#include "handle_internal.h"
#include "schema_registry.h"
#include "c2sql.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Test fixture helpers                                                */
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

/* Sample struct layout for tests */
typedef struct TestRow {
    int32_t id;
    int64_t value;
    double  score;
    char    label[64];
} TestRow;

static const SqlRDBColumnDef COLS_WITH_PK[] = {
    { "id",    SQL_TYPE_INT32, offsetof(TestRow, id),    sizeof(int32_t), SQL_COL_FLAG_PRIMARY_KEY },
    { "value", SQL_TYPE_INT64, offsetof(TestRow, value), sizeof(int64_t), SQL_COL_FLAG_NONE        },
    { "score", SQL_TYPE_REAL,  offsetof(TestRow, score), sizeof(double),  SQL_COL_FLAG_NONE        },
    { "label", SQL_TYPE_TEXT,  offsetof(TestRow, label), 64,              SQL_COL_FLAG_NULLABLE    },
};
static const size_t COLS_WITH_PK_COUNT = 4;

static const SqlRDBColumnDef COLS_NO_PK[] = {
    { "value", SQL_TYPE_INT64, offsetof(TestRow, value), sizeof(int64_t), SQL_COL_FLAG_NONE },
    { "score", SQL_TYPE_REAL,  offsetof(TestRow, score), sizeof(double),  SQL_COL_FLAG_NONE },
};
static const size_t COLS_NO_PK_COUNT = 2;

/* ------------------------------------------------------------------ */
/* Task 4.1: 登録と検証                                               */
/* ------------------------------------------------------------------ */

/* 正常系：基本的な登録が成功する */
static void test_register_basic_ok(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBResult r = c2sql_internal_schema_register(&h, "test_table", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    TEST_ASSERT(r == SQL_RDB_OK);
    teardown_handle(&h);
}

/* NULLハンドル → INVALID_HANDLE */
static void test_register_null_handle(void) {
    SqlRDBResult r = c2sql_internal_schema_register(NULL, "t", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_HANDLE);
}

/* NULLの構造体名 → INVALID_ARG */
static void test_register_null_name(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBResult r = c2sql_internal_schema_register(&h, NULL, COLS_WITH_PK, COLS_WITH_PK_COUNT);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    teardown_handle(&h);
}

/* NULLのカラム配列 → INVALID_ARG */
static void test_register_null_cols(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBResult r = c2sql_internal_schema_register(&h, "t", NULL, 1);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    teardown_handle(&h);
}

/* カラム数0 → INVALID_ARG */
static void test_register_zero_col_count(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBResult r = c2sql_internal_schema_register(&h, "t", COLS_WITH_PK, 0);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    teardown_handle(&h);
}

/* カラム数上限超過 → TOO_MANY_COLUMNS */
static void test_register_too_many_columns(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBResult r = c2sql_internal_schema_register(&h, "t", COLS_WITH_PK, C2SQL_MAX_COLUMNS + 1);
    TEST_ASSERT(r == SQL_RDB_ERR_TOO_MANY_COLUMNS);
    teardown_handle(&h);
}

/* 不正な構造体名（スペース含む） → INVALID_NAME */
static void test_register_invalid_struct_name_space(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBResult r = c2sql_internal_schema_register(&h, "foo bar", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_NAME);
    teardown_handle(&h);
}

/* 不正な構造体名（ハイフン含む） → INVALID_NAME */
static void test_register_invalid_struct_name_hyphen(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBResult r = c2sql_internal_schema_register(&h, "foo-bar", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_NAME);
    teardown_handle(&h);
}

/* 構造体名が数字始まり → INVALID_NAME */
static void test_register_name_starts_with_digit(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBResult r = c2sql_internal_schema_register(&h, "1table", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_NAME);
    teardown_handle(&h);
}

/* SQL予約語の構造体名 → INVALID_NAME */
static void test_register_reserved_word_struct_select(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBResult r = c2sql_internal_schema_register(&h, "SELECT", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_NAME);
    teardown_handle(&h);
}

static void test_register_reserved_word_struct_table(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBResult r = c2sql_internal_schema_register(&h, "table", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_NAME);
    teardown_handle(&h);
}

/* SQL予約語のカラム名 → INVALID_NAME */
static void test_register_reserved_word_column(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBColumnDef bad_cols[] = {
        { "SELECT", SQL_TYPE_INT32, 0, 4, SQL_COL_FLAG_NONE },
    };
    SqlRDBResult r = c2sql_internal_schema_register(&h, "t", bad_cols, 1);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_NAME);
    teardown_handle(&h);
}

/* 不正なカラム名（スペース） → INVALID_NAME */
static void test_register_invalid_column_name_chars(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBColumnDef bad_cols[] = {
        { "bad col", SQL_TYPE_INT32, 0, 4, SQL_COL_FLAG_NONE },
    };
    SqlRDBResult r = c2sql_internal_schema_register(&h, "t", bad_cols, 1);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_NAME);
    teardown_handle(&h);
}

/* NULLのカラム名 → INVALID_ARG */
static void test_register_null_column_name(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBColumnDef bad_cols[] = {
        { NULL, SQL_TYPE_INT32, 0, 4, SQL_COL_FLAG_NONE },
    };
    SqlRDBResult r = c2sql_internal_schema_register(&h, "t", bad_cols, 1);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    teardown_handle(&h);
}

/* 重複した構造体名 → DUPLICATE_SCHEMA */
static void test_register_duplicate_struct(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBResult r1 = c2sql_internal_schema_register(&h, "my_table", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    SqlRDBResult r2 = c2sql_internal_schema_register(&h, "my_table", COLS_NO_PK, COLS_NO_PK_COUNT);
    TEST_ASSERT(r1 == SQL_RDB_OK);
    TEST_ASSERT(r2 == SQL_RDB_ERR_DUPLICATE_SCHEMA);
    teardown_handle(&h);
}

/* 重複した構造体名（大文字小文字区別あり：異なる名前として扱う） */
static void test_register_names_case_sensitive(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBResult r1 = c2sql_internal_schema_register(&h, "mytable", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    SqlRDBResult r2 = c2sql_internal_schema_register(&h, "MyTable", COLS_NO_PK, COLS_NO_PK_COUNT);
    TEST_ASSERT(r1 == SQL_RDB_OK);
    TEST_ASSERT(r2 == SQL_RDB_OK);
    teardown_handle(&h);
}

/* 重複したカラム名 → DUPLICATE_COLUMN */
static void test_register_duplicate_column(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBColumnDef dup_cols[] = {
        { "id",  SQL_TYPE_INT32, 0, 4, SQL_COL_FLAG_PRIMARY_KEY },
        { "id",  SQL_TYPE_INT64, 4, 8, SQL_COL_FLAG_NONE        },
    };
    SqlRDBResult r = c2sql_internal_schema_register(&h, "t", dup_cols, 2);
    TEST_ASSERT(r == SQL_RDB_ERR_DUPLICATE_COLUMN);
    teardown_handle(&h);
}

/* INT32でサイズが4バイト以外 → INVALID_ARG */
static void test_register_int32_wrong_size(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBColumnDef bad_cols[] = {
        { "id", SQL_TYPE_INT32, 0, 8, SQL_COL_FLAG_NONE },
    };
    SqlRDBResult r = c2sql_internal_schema_register(&h, "t", bad_cols, 1);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    teardown_handle(&h);
}

/* INT64でサイズが8バイト以外 → INVALID_ARG */
static void test_register_int64_wrong_size(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBColumnDef bad_cols[] = {
        { "val", SQL_TYPE_INT64, 0, 4, SQL_COL_FLAG_NONE },
    };
    SqlRDBResult r = c2sql_internal_schema_register(&h, "t", bad_cols, 1);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    teardown_handle(&h);
}

/* 重複登録失敗時は既存スキーマを変更しない */
static void test_register_duplicate_leaves_existing_intact(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    c2sql_internal_schema_register(&h, "t", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    /* 2回目の登録（失敗） */
    c2sql_internal_schema_register(&h, "t", COLS_NO_PK, COLS_NO_PK_COUNT);

    /* 既存スキーマは変更されていない */
    const SqlRDBSchema *s = c2sql_internal_schema_lookup(&h, "t");
    TEST_ASSERT(s != NULL);
    TEST_ASSERT(s->col_count == COLS_WITH_PK_COUNT);

    teardown_handle(&h);
}

/* ディープコピー：元のカラム定義を変更してもスキーマに影響しない */
static void test_register_deep_copy_immutable(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    /* mutableなローカルコピーでテスト */
    SqlRDBColumnDef mutable_cols[2] = {
        { "aa", SQL_TYPE_INT32, 0, 4, SQL_COL_FLAG_NONE },
        { "bb", SQL_TYPE_INT64, 4, 8, SQL_COL_FLAG_NONE },
    };
    /* ここで一時的な文字列を使用しても、登録後は変更できないはず */
    char col_name_buf[8] = "aa";
    mutable_cols[0].name = col_name_buf;

    SqlRDBResult r = c2sql_internal_schema_register(&h, "t", mutable_cols, 2);
    TEST_ASSERT(r == SQL_RDB_OK);

    /* 元の名前バッファを変更 */
    col_name_buf[0] = 'z';

    const SqlRDBSchema *s = c2sql_internal_schema_lookup(&h, "t");
    TEST_ASSERT(s != NULL);
    /* ディープコピーされているので元の変更に影響されない */
    TEST_ASSERT(strcmp(s->cols[0].name, "aa") == 0);

    teardown_handle(&h);
}

/* 複数のスキーマを登録できる */
static void test_register_multiple_schemas(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    SqlRDBResult r1 = c2sql_internal_schema_register(&h, "alpha", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    SqlRDBResult r2 = c2sql_internal_schema_register(&h, "beta",  COLS_NO_PK,   COLS_NO_PK_COUNT);
    SqlRDBResult r3 = c2sql_internal_schema_register(&h, "gamma", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    TEST_ASSERT(r1 == SQL_RDB_OK);
    TEST_ASSERT(r2 == SQL_RDB_OK);
    TEST_ASSERT(r3 == SQL_RDB_OK);

    teardown_handle(&h);
}

/* アンダースコア始まりの名前は有効 */
static void test_register_name_starting_with_underscore(void) {
    SqlRDBHandle h;
    setup_handle(&h);
    SqlRDBResult r = c2sql_internal_schema_register(&h, "_my_table", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    TEST_ASSERT(r == SQL_RDB_OK);
    teardown_handle(&h);
}

/* ------------------------------------------------------------------ */
/* Task 4.2: スキーマ検索とプライマリキー識別                         */
/* ------------------------------------------------------------------ */

/* 登録済みスキーマを名前で検索できる */
static void test_lookup_registered_schema(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    c2sql_internal_schema_register(&h, "my_table", COLS_WITH_PK, COLS_WITH_PK_COUNT);

    const SqlRDBSchema *s = c2sql_internal_schema_lookup(&h, "my_table");
    TEST_ASSERT(s != NULL);
    TEST_ASSERT(strcmp(s->name, "my_table") == 0);
    TEST_ASSERT(s->col_count == COLS_WITH_PK_COUNT);

    teardown_handle(&h);
}

/* 存在しない名前の検索 → NULL */
static void test_lookup_nonexistent_schema(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = c2sql_internal_schema_lookup(&h, "nonexistent");
    TEST_ASSERT(s == NULL);

    teardown_handle(&h);
}

/* NULLハンドル → NULL */
static void test_lookup_null_handle(void) {
    const SqlRDBSchema *s = c2sql_internal_schema_lookup(NULL, "t");
    TEST_ASSERT(s == NULL);
}

/* NULLの名前 → NULL */
static void test_lookup_null_name(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    const SqlRDBSchema *s = c2sql_internal_schema_lookup(&h, NULL);
    TEST_ASSERT(s == NULL);

    teardown_handle(&h);
}

/* PKフラグ付きカラムでpk_indexが正しく設定される */
static void test_lookup_pk_index_with_pk(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    /* COLS_WITH_PK: index 0 が PRIMARY_KEY */
    c2sql_internal_schema_register(&h, "t", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    const SqlRDBSchema *s = c2sql_internal_schema_lookup(&h, "t");
    TEST_ASSERT(s != NULL);
    TEST_ASSERT(s->pk_index == 0);

    teardown_handle(&h);
}

/* PKなしのスキーマでpk_index == -1 */
static void test_lookup_pk_index_no_pk(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    c2sql_internal_schema_register(&h, "t", COLS_NO_PK, COLS_NO_PK_COUNT);
    const SqlRDBSchema *s = c2sql_internal_schema_lookup(&h, "t");
    TEST_ASSERT(s != NULL);
    TEST_ASSERT(s->pk_index == -1);

    teardown_handle(&h);
}

/* PKが2番目のカラムのときpk_index == 1 */
static void test_lookup_pk_index_second_column(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    SqlRDBColumnDef cols[] = {
        { "name", SQL_TYPE_TEXT,  0,  64, SQL_COL_FLAG_NONE        },
        { "id",   SQL_TYPE_INT32, 64, 4,  SQL_COL_FLAG_PRIMARY_KEY },
    };
    c2sql_internal_schema_register(&h, "t", cols, 2);
    const SqlRDBSchema *s = c2sql_internal_schema_lookup(&h, "t");
    TEST_ASSERT(s != NULL);
    TEST_ASSERT(s->pk_index == 1);

    teardown_handle(&h);
}

/* NULLABLE フラグが保持される */
static void test_flags_nullable_preserved(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    c2sql_internal_schema_register(&h, "t", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    const SqlRDBSchema *s = c2sql_internal_schema_lookup(&h, "t");
    TEST_ASSERT(s != NULL);
    /* label (index 3) は NULLABLE */
    TEST_ASSERT((s->cols[3].flags & SQL_COL_FLAG_NULLABLE) != 0);
    /* id (index 0) は NULLABLE ではない */
    TEST_ASSERT((s->cols[0].flags & SQL_COL_FLAG_NULLABLE) == 0);

    teardown_handle(&h);
}

/* UNIQUE フラグが保持される */
static void test_flags_unique_preserved(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    SqlRDBColumnDef cols[] = {
        { "id",    SQL_TYPE_INT32, 0, 4, SQL_COL_FLAG_PRIMARY_KEY },
        { "email", SQL_TYPE_TEXT,  4, 128, SQL_COL_FLAG_UNIQUE | SQL_COL_FLAG_NULLABLE },
    };
    c2sql_internal_schema_register(&h, "t", cols, 2);
    const SqlRDBSchema *s = c2sql_internal_schema_lookup(&h, "t");
    TEST_ASSERT(s != NULL);
    TEST_ASSERT((s->cols[1].flags & SQL_COL_FLAG_UNIQUE) != 0);

    teardown_handle(&h);
}

/* スキーマの名前は大文字小文字を区別して検索する */
static void test_lookup_case_sensitive(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    c2sql_internal_schema_register(&h, "MyTable", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    TEST_ASSERT(c2sql_internal_schema_lookup(&h, "MyTable")  != NULL);
    TEST_ASSERT(c2sql_internal_schema_lookup(&h, "mytable")  == NULL);
    TEST_ASSERT(c2sql_internal_schema_lookup(&h, "MYTABLE")  == NULL);

    teardown_handle(&h);
}

/* 複数のスキーマを個別に検索できる */
static void test_lookup_multiple_schemas(void) {
    SqlRDBHandle h;
    setup_handle(&h);

    c2sql_internal_schema_register(&h, "alpha", COLS_WITH_PK, COLS_WITH_PK_COUNT);
    c2sql_internal_schema_register(&h, "beta",  COLS_NO_PK,   COLS_NO_PK_COUNT);

    const SqlRDBSchema *a = c2sql_internal_schema_lookup(&h, "alpha");
    const SqlRDBSchema *b = c2sql_internal_schema_lookup(&h, "beta");
    TEST_ASSERT(a != NULL && strcmp(a->name, "alpha") == 0);
    TEST_ASSERT(b != NULL && strcmp(b->name, "beta") == 0);
    TEST_ASSERT(a != b);

    teardown_handle(&h);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    /* Task 4.1: 登録と検証 */
    test_register_basic_ok();
    test_register_null_handle();
    test_register_null_name();
    test_register_null_cols();
    test_register_zero_col_count();
    test_register_too_many_columns();
    test_register_invalid_struct_name_space();
    test_register_invalid_struct_name_hyphen();
    test_register_name_starts_with_digit();
    test_register_reserved_word_struct_select();
    test_register_reserved_word_struct_table();
    test_register_reserved_word_column();
    test_register_invalid_column_name_chars();
    test_register_null_column_name();
    test_register_duplicate_struct();
    test_register_names_case_sensitive();
    test_register_duplicate_column();
    test_register_int32_wrong_size();
    test_register_int64_wrong_size();
    test_register_duplicate_leaves_existing_intact();
    test_register_deep_copy_immutable();
    test_register_multiple_schemas();
    test_register_name_starting_with_underscore();

    /* Task 4.2: スキーマ検索とプライマリキー識別 */
    test_lookup_registered_schema();
    test_lookup_nonexistent_schema();
    test_lookup_null_handle();
    test_lookup_null_name();
    test_lookup_pk_index_with_pk();
    test_lookup_pk_index_no_pk();
    test_lookup_pk_index_second_column();
    test_flags_nullable_preserved();
    test_flags_unique_preserved();
    test_lookup_case_sensitive();
    test_lookup_multiple_schemas();

    TEST_SUMMARY();
}
