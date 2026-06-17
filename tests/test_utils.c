/*
 * test_utils.c — Task 3: 横断ユーティリティ（Mutex / Error / Logger）
 *
 * Task 3.1: SqlRDBMutex (POSIX/Windows abstraction, no-op mode)
 * Task 3.2: SqlRDBErrorCtx + SqlRDBLastError public API
 * Task 3.3: SqlRDBLoggerCtx + SqlRDBSetLogger public API
 */
#include "harness.h"
#include "mutex.h"
#include "error_ctx.h"
#include "logger.h"
#include "handle_internal.h"
#include "c2sql.h"
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Task 3.1: Mutex                                                     */
/* ------------------------------------------------------------------ */

static void test_mutex_threadsafe_init_destroy(void) {
    SqlRDBMutex m;
    SqlRDBResult r = c2sql_internal_mutex_init(&m, true);
    TEST_ASSERT(r == SQL_RDB_OK);
    c2sql_internal_mutex_destroy(&m);
}

static void test_mutex_noop_init_destroy(void) {
    SqlRDBMutex m;
    SqlRDBResult r = c2sql_internal_mutex_init(&m, false);
    TEST_ASSERT(r == SQL_RDB_OK);
    c2sql_internal_mutex_destroy(&m);
}

static void test_mutex_lock_unlock_threadsafe(void) {
    SqlRDBMutex m;
    c2sql_internal_mutex_init(&m, true);
    c2sql_internal_mutex_lock(&m);
    c2sql_internal_mutex_unlock(&m);
    c2sql_internal_mutex_destroy(&m);
    TEST_ASSERT(1);
}

static void test_mutex_lock_unlock_noop(void) {
    SqlRDBMutex m;
    c2sql_internal_mutex_init(&m, false);
    c2sql_internal_mutex_lock(&m);
    c2sql_internal_mutex_unlock(&m);
    c2sql_internal_mutex_destroy(&m);
    /* Must not crash in no-op mode */
    TEST_ASSERT(1);
}

static void test_mutex_threadsafe_flag_stored(void) {
    SqlRDBMutex m_ts, m_noop;
    c2sql_internal_mutex_init(&m_ts,   true);
    c2sql_internal_mutex_init(&m_noop, false);
    TEST_ASSERT(m_ts.threadsafe   == true);
    TEST_ASSERT(m_noop.threadsafe == false);
    c2sql_internal_mutex_destroy(&m_ts);
    c2sql_internal_mutex_destroy(&m_noop);
}

/* ------------------------------------------------------------------ */
/* Task 3.2: Error Context                                             */
/* ------------------------------------------------------------------ */

static void test_err_ctx_initial_state(void) {
    SqlRDBErrorCtx e;
    c2sql_internal_err_clear(&e);
    TEST_ASSERT(e.code == SQL_RDB_OK);
    TEST_ASSERT(e.message[0] == '\0');
}

static void test_err_ctx_set_code_and_message(void) {
    SqlRDBErrorCtx e;
    c2sql_internal_err_clear(&e);
    c2sql_internal_err_set(&e, SQL_RDB_ERR_INVALID_ARG, "bad argument: %s", "foo");
    TEST_ASSERT(e.code == SQL_RDB_ERR_INVALID_ARG);
    TEST_ASSERT(strstr(e.message, "bad argument") != NULL);
    TEST_ASSERT(strstr(e.message, "foo") != NULL);
}

static void test_err_ctx_message_capped_at_255(void) {
    SqlRDBErrorCtx e;
    c2sql_internal_err_clear(&e);
    char big[512];
    memset(big, 'A', 511);
    big[511] = '\0';
    c2sql_internal_err_set(&e, SQL_RDB_ERR_INTERNAL, "%s", big);
    /* Buffer is 256 bytes; message must be <= 255 chars + NUL */
    TEST_ASSERT(strlen(e.message) <= 255);
    TEST_ASSERT(e.message[255] == '\0');
}

static void test_err_ctx_no_memory_does_not_crash(void) {
    SqlRDBErrorCtx e;
    c2sql_internal_err_clear(&e);
    c2sql_internal_err_set(&e, SQL_RDB_ERR_NO_MEMORY, "OOM: %s", "alloc failed");
    TEST_ASSERT(e.code == SQL_RDB_ERR_NO_MEMORY);
    TEST_ASSERT(1);
}

static void test_err_ctx_overwrite(void) {
    SqlRDBErrorCtx e;
    c2sql_internal_err_clear(&e);
    c2sql_internal_err_set(&e, SQL_RDB_ERR_DRIVER, "first");
    c2sql_internal_err_set(&e, SQL_RDB_ERR_NOT_FOUND, "second");
    TEST_ASSERT(e.code == SQL_RDB_ERR_NOT_FOUND);
    TEST_ASSERT(strstr(e.message, "second") != NULL);
}

/* SqlRDBLastError via minimal handle */
static void test_last_error_empty_handle(void) {
    SqlRDBHandle h;
    memset(&h, 0, sizeof(h));
    h.magic = SQL_RDB_HANDLE_MAGIC;
    c2sql_internal_mutex_init(&h.mutex, false);
    c2sql_internal_err_clear(&h.error);

    SqlRDBResult code = SQL_RDB_ERR_INTERNAL;
    const char *msg = SqlRDBLastError(&h, &code);
    TEST_ASSERT(msg != NULL);
    TEST_ASSERT(msg[0] == '\0');
    TEST_ASSERT(code == SQL_RDB_OK);
    c2sql_internal_mutex_destroy(&h.mutex);
}

static void test_last_error_with_error_set(void) {
    SqlRDBHandle h;
    memset(&h, 0, sizeof(h));
    h.magic = SQL_RDB_HANDLE_MAGIC;
    c2sql_internal_mutex_init(&h.mutex, false);
    c2sql_internal_err_set(&h.error, SQL_RDB_ERR_DRIVER, "driver error %d", 42);

    SqlRDBResult code = SQL_RDB_OK;
    const char *msg = SqlRDBLastError(&h, &code);
    TEST_ASSERT(code == SQL_RDB_ERR_DRIVER);
    TEST_ASSERT(strstr(msg, "driver error") != NULL);
    c2sql_internal_mutex_destroy(&h.mutex);
}

static void test_last_error_null_handle(void) {
    /* Must not crash; return empty string */
    const char *msg = SqlRDBLastError(NULL, NULL);
    TEST_ASSERT(msg != NULL);
    TEST_ASSERT(msg[0] == '\0');
}

static void test_last_error_out_code_null(void) {
    SqlRDBHandle h;
    memset(&h, 0, sizeof(h));
    h.magic = SQL_RDB_HANDLE_MAGIC;
    c2sql_internal_mutex_init(&h.mutex, false);
    c2sql_internal_err_set(&h.error, SQL_RDB_ERR_INVALID_ARG, "oops");
    /* Must not crash when out_code is NULL */
    const char *msg = SqlRDBLastError(&h, NULL);
    TEST_ASSERT(msg != NULL);
    TEST_ASSERT(strstr(msg, "oops") != NULL);
    c2sql_internal_mutex_destroy(&h.mutex);
}

/* ------------------------------------------------------------------ */
/* Task 3.3: Logger                                                    */
/* ------------------------------------------------------------------ */

static int         g_log_calls     = 0;
static SqlRDBResult g_log_last_code = SQL_RDB_OK;
static const char  *g_log_last_sql  = NULL;

static void counting_logger(void *user, SqlRDBResult code,
                             const char *sql, const char *msg) {
    (void)user; (void)msg;
    g_log_calls++;
    g_log_last_code = code;
    g_log_last_sql  = sql;
}

static void test_logger_unset_no_invoke(void) {
    SqlRDBLoggerCtx l;
    l.fn   = NULL;
    l.user = NULL;
    g_log_calls = 0;
    c2sql_internal_log(&l, SQL_RDB_OK, "SELECT 1", "ok");
    TEST_ASSERT(g_log_calls == 0);
}

static void test_logger_set_and_invoked(void) {
    SqlRDBLoggerCtx l;
    l.fn   = counting_logger;
    l.user = NULL;
    g_log_calls     = 0;
    g_log_last_code = SQL_RDB_OK;
    c2sql_internal_log(&l, SQL_RDB_ERR_DRIVER, "INSERT INTO t VALUES (?)", "driver error");
    TEST_ASSERT(g_log_calls     == 1);
    TEST_ASSERT(g_log_last_code == SQL_RDB_ERR_DRIVER);
}

static void test_logger_null_ctx_no_crash(void) {
    /* Passing NULL context should not crash */
    c2sql_internal_log(NULL, SQL_RDB_OK, NULL, NULL);
    TEST_ASSERT(1);
}

static void test_logger_null_sql_ok(void) {
    SqlRDBLoggerCtx l;
    l.fn   = counting_logger;
    l.user = NULL;
    g_log_calls    = 0;
    g_log_last_sql = (const char *)1; /* sentinel */
    c2sql_internal_log(&l, SQL_RDB_OK, NULL, NULL);
    TEST_ASSERT(g_log_calls    == 1);
    TEST_ASSERT(g_log_last_sql == NULL);
}

static void test_set_logger_public_api(void) {
    SqlRDBHandle h;
    memset(&h, 0, sizeof(h));
    h.magic = SQL_RDB_HANDLE_MAGIC;
    c2sql_internal_mutex_init(&h.mutex, false);

    g_log_calls = 0;
    SqlRDBResult r = SqlRDBSetLogger(&h, counting_logger, NULL);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(h.logger.fn == counting_logger);

    c2sql_internal_log(&h.logger, SQL_RDB_OK, NULL, NULL);
    TEST_ASSERT(g_log_calls == 1);

    c2sql_internal_mutex_destroy(&h.mutex);
}

static void test_set_logger_null_disables(void) {
    SqlRDBHandle h;
    memset(&h, 0, sizeof(h));
    h.magic = SQL_RDB_HANDLE_MAGIC;
    c2sql_internal_mutex_init(&h.mutex, false);

    SqlRDBSetLogger(&h, counting_logger, NULL);
    SqlRDBSetLogger(&h, NULL, NULL);

    g_log_calls = 0;
    c2sql_internal_log(&h.logger, SQL_RDB_OK, NULL, NULL);
    TEST_ASSERT(g_log_calls == 0);
    TEST_ASSERT(h.logger.fn == NULL);

    c2sql_internal_mutex_destroy(&h.mutex);
}

static void test_set_logger_invalid_handle(void) {
    SqlRDBResult r = SqlRDBSetLogger(NULL, counting_logger, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_HANDLE);
}

static void test_set_logger_dead_handle(void) {
    SqlRDBHandle h;
    memset(&h, 0, sizeof(h));
    h.magic = SQL_RDB_HANDLE_DEAD;
    SqlRDBResult r = SqlRDBSetLogger(&h, counting_logger, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_HANDLE);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    /* Task 3.1: Mutex */
    test_mutex_threadsafe_init_destroy();
    test_mutex_noop_init_destroy();
    test_mutex_lock_unlock_threadsafe();
    test_mutex_lock_unlock_noop();
    test_mutex_threadsafe_flag_stored();

    /* Task 3.2: Error Context */
    test_err_ctx_initial_state();
    test_err_ctx_set_code_and_message();
    test_err_ctx_message_capped_at_255();
    test_err_ctx_no_memory_does_not_crash();
    test_err_ctx_overwrite();
    test_last_error_empty_handle();
    test_last_error_with_error_set();
    test_last_error_null_handle();
    test_last_error_out_code_null();

    /* Task 3.3: Logger */
    test_logger_unset_no_invoke();
    test_logger_set_and_invoked();
    test_logger_null_ctx_no_crash();
    test_logger_null_sql_ok();
    test_set_logger_public_api();
    test_set_logger_null_disables();
    test_set_logger_invalid_handle();
    test_set_logger_dead_handle();

    TEST_SUMMARY();
}
