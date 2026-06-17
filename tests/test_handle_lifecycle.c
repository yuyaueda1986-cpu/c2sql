/*
 * test_handle_lifecycle.c — Task 9: ハンドルマネージャと公開ライフサイクルAPI
 *
 * Task 9.1: ハンドル確保・解放と二重解放検知
 * Task 9.2: 設定APIとスレッドロックの組込み
 */
#include "harness.h"
#include "c2sql.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Task 9.1: SqlRDBInit / SqlRDBClose                                 */
/* ------------------------------------------------------------------ */

/* 正常系: in-memory SQLiteで初期化成功 */
static void test_init_memory_ok(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    SqlRDBResult r = SqlRDBClose(h);
    TEST_ASSERT(r == SQL_RDB_OK);
}

/* 異常系: NULL接続文字列はNULLを返す */
static void test_init_null_dsn_returns_null(void) {
    SqlRDBHandle *h = SqlRDBInit(NULL);
    TEST_ASSERT(h == NULL);
}

/* 異常系: 無効なパスはNULLを返す */
static void test_init_invalid_path_returns_null(void) {
    SqlRDBHandle *h = SqlRDBInit("/no_such_dir_c2sql_test/db.sqlite3");
    TEST_ASSERT(h == NULL);
}

/* 正常系: 複数ハンドルを同時保持できる */
static void test_multiple_handles(void) {
    SqlRDBHandle *h1 = SqlRDBInit(":memory:");
    SqlRDBHandle *h2 = SqlRDBInit(":memory:");
    TEST_ASSERT(h1 != NULL);
    TEST_ASSERT(h2 != NULL);
    TEST_ASSERT(h1 != h2);
    TEST_ASSERT(SqlRDBClose(h1) == SQL_RDB_OK);
    TEST_ASSERT(SqlRDBClose(h2) == SQL_RDB_OK);
}

/* 異常系: NULL ハンドルのClose */
static void test_close_null_handle(void) {
    SqlRDBResult r = SqlRDBClose(NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_HANDLE);
}

/* 異常系: 二重解放はエラーを返す */
static void test_double_close_returns_invalid_handle(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(SqlRDBClose(h) == SQL_RDB_OK);
    SqlRDBResult r = SqlRDBClose(h);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_HANDLE);
}

/* 正常系: Close後にSqlRDBLastErrorを呼んでも安全（NULLチェック） */
static void test_last_error_on_invalid_handle(void) {
    SqlRDBResult code = SQL_RDB_OK;
    const char *msg = SqlRDBLastError(NULL, &code);
    TEST_ASSERT(msg != NULL); /* 空文字列を返す */
    TEST_ASSERT(code == SQL_RDB_OK);
}

/* ------------------------------------------------------------------ */
/* Task 9.2: SqlRDBSetConfig / スレッドロック                         */
/* ------------------------------------------------------------------ */

/* 正常系: デフォルト設定のハンドルで SetConfig 成功 */
static void test_setconfig_ok(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);

    SqlRDBConfig cfg = {
        .threadsafe       = true,
        .stmt_cache_size  = 32,
        .auto_migrate     = false,
        .multirow_default = 0,
        .require_strict   = false,
    };
    SqlRDBResult r = SqlRDBSetConfig(h, &cfg);
    TEST_ASSERT(r == SQL_RDB_OK);
    SqlRDBClose(h);
}

/* 異常系: NULL ハンドルへの SetConfig */
static void test_setconfig_null_handle(void) {
    SqlRDBConfig cfg = {0};
    SqlRDBResult r = SqlRDBSetConfig(NULL, &cfg);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_HANDLE);
}

/* 異常系: NULL cfg */
static void test_setconfig_null_cfg(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    SqlRDBResult r = SqlRDBSetConfig(h, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    SqlRDBClose(h);
}

/* 正常系: stmt_cache_size=0 (無効化モード) も受け付ける */
static void test_setconfig_cache_size_zero(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    SqlRDBConfig cfg = {
        .threadsafe       = true,
        .stmt_cache_size  = 0,
        .auto_migrate     = false,
        .multirow_default = 0,
        .require_strict   = false,
    };
    TEST_ASSERT(SqlRDBSetConfig(h, &cfg) == SQL_RDB_OK);
    SqlRDBClose(h);
}

/* 正常系: スレッドセーフ無効でも SqlRDBSetLogger が機能する */
static void test_setlogger_after_init(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    SqlRDBResult r = SqlRDBSetLogger(h, NULL, NULL);
    TEST_ASSERT(r == SQL_RDB_OK);
    SqlRDBClose(h);
}

/* 正常系: SqlRDBInit後にSqlRDBLastErrorが正常動作する */
static void test_last_error_on_fresh_handle(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    SqlRDBResult code = SQL_RDB_ERR_INTERNAL;
    const char *msg = SqlRDBLastError(h, &code);
    TEST_ASSERT(msg != NULL);
    TEST_ASSERT(code == SQL_RDB_OK); /* 初期状態はエラーなし */
    SqlRDBClose(h);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    /* Task 9.1: Init/Close */
    test_init_memory_ok();
    test_init_null_dsn_returns_null();
    test_init_invalid_path_returns_null();
    test_multiple_handles();
    test_close_null_handle();
    test_double_close_returns_invalid_handle();
    test_last_error_on_invalid_handle();

    /* Task 9.2: SetConfig / スレッドロック */
    test_setconfig_ok();
    test_setconfig_null_handle();
    test_setconfig_null_cfg();
    test_setconfig_cache_size_zero();
    test_setlogger_after_init();
    test_last_error_on_fresh_handle();

    TEST_SUMMARY();
}
