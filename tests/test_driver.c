/*
 * test_driver.c — Task 2: DBドライバ抽象とSQLite3初期実装テスト
 *
 * Task 2.1: vtable定義の完全性確認
 * Task 2.2: 接続・exec・トランザクション
 * Task 2.3: prepare/bind/step/column/reset/finalize
 */
#include "harness.h"
#include "db_driver.h"
#include "sqlite_driver.h"
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Task 2.1: vtable定義確認                                            */
/* ------------------------------------------------------------------ */

static void test_vtable_name(void) {
    TEST_ASSERT(g_sqlite3_driver.name != NULL);
    TEST_ASSERT(strcmp(g_sqlite3_driver.name, "sqlite3") == 0);
}

static void test_vtable_all_functions_set(void) {
    const SqlRDBDriver *d = &g_sqlite3_driver;
    TEST_ASSERT(d->open          != NULL);
    TEST_ASSERT(d->close         != NULL);
    TEST_ASSERT(d->exec          != NULL);
    TEST_ASSERT(d->prepare       != NULL);
    TEST_ASSERT(d->bind_int64    != NULL);
    TEST_ASSERT(d->bind_int32    != NULL);
    TEST_ASSERT(d->bind_real     != NULL);
    TEST_ASSERT(d->bind_text     != NULL);
    TEST_ASSERT(d->bind_blob     != NULL);
    TEST_ASSERT(d->bind_null     != NULL);
    TEST_ASSERT(d->step          != NULL);
    TEST_ASSERT(d->column_int64  != NULL);
    TEST_ASSERT(d->column_int32  != NULL);
    TEST_ASSERT(d->column_real   != NULL);
    TEST_ASSERT(d->column_text   != NULL);
    TEST_ASSERT(d->column_blob   != NULL);
    TEST_ASSERT(d->column_isnull != NULL);
    TEST_ASSERT(d->reset         != NULL);
    TEST_ASSERT(d->finalize      != NULL);
    TEST_ASSERT(d->begin         != NULL);
    TEST_ASSERT(d->commit        != NULL);
    TEST_ASSERT(d->rollback      != NULL);
    TEST_ASSERT(d->savepoint     != NULL);
    TEST_ASSERT(d->release_sp    != NULL);
    TEST_ASSERT(d->rollback_sp   != NULL);
    TEST_ASSERT(d->changes       != NULL);
}

/* ------------------------------------------------------------------ */
/* Task 2.2: 接続・exec・トランザクション                              */
/* ------------------------------------------------------------------ */

static void test_open_memory(void) {
    void *ctx = NULL;
    SqlRDBResult r = g_sqlite3_driver.open(":memory:", &ctx);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(ctx != NULL);
    g_sqlite3_driver.close(ctx);
}

static void test_open_invalid_path(void) {
    void *ctx = NULL;
    SqlRDBResult r = g_sqlite3_driver.open("/no_such_dir_c2sql/db.sqlite3", &ctx);
    TEST_ASSERT(r != SQL_RDB_OK);
    TEST_ASSERT(ctx == NULL);
}

static void test_exec_valid_sql(void) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    SqlRDBResult r = g_sqlite3_driver.exec(
        ctx, "CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)");
    TEST_ASSERT(r == SQL_RDB_OK);
    g_sqlite3_driver.close(ctx);
}

static void test_exec_invalid_sql(void) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    SqlRDBResult r = g_sqlite3_driver.exec(ctx, "NOT VALID SQL AT ALL");
    TEST_ASSERT(r == SQL_RDB_ERR_DRIVER);
    g_sqlite3_driver.close(ctx);
}

static void test_begin_commit(void) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    g_sqlite3_driver.exec(ctx, "CREATE TABLE t (v INTEGER)");
    TEST_ASSERT(g_sqlite3_driver.begin(ctx)  == SQL_RDB_OK);
    TEST_ASSERT(g_sqlite3_driver.exec(ctx, "INSERT INTO t VALUES (1)") == SQL_RDB_OK);
    TEST_ASSERT(g_sqlite3_driver.commit(ctx) == SQL_RDB_OK);
    g_sqlite3_driver.close(ctx);
}

static void test_begin_rollback_undoes_insert(void) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    g_sqlite3_driver.exec(ctx, "CREATE TABLE t (v INTEGER)");
    g_sqlite3_driver.begin(ctx);
    g_sqlite3_driver.exec(ctx, "INSERT INTO t VALUES (1)");
    g_sqlite3_driver.rollback(ctx);

    void *stmt = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT COUNT(*) FROM t", &stmt);
    bool has_row = false;
    g_sqlite3_driver.step(stmt, &has_row);
    TEST_ASSERT(has_row);
    int64_t count = -1;
    g_sqlite3_driver.column_int64(stmt, 0, &count);
    TEST_ASSERT(count == 0);
    g_sqlite3_driver.finalize(stmt);
    g_sqlite3_driver.close(ctx);
}

static void test_savepoint_release(void) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    g_sqlite3_driver.exec(ctx, "CREATE TABLE t (v INTEGER)");
    TEST_ASSERT(g_sqlite3_driver.savepoint(ctx, "sp1")   == SQL_RDB_OK);
    g_sqlite3_driver.exec(ctx, "INSERT INTO t VALUES (42)");
    TEST_ASSERT(g_sqlite3_driver.release_sp(ctx, "sp1") == SQL_RDB_OK);

    void *stmt = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT COUNT(*) FROM t", &stmt);
    bool has_row = false;
    g_sqlite3_driver.step(stmt, &has_row);
    int64_t count = 0;
    g_sqlite3_driver.column_int64(stmt, 0, &count);
    TEST_ASSERT(count == 1);
    g_sqlite3_driver.finalize(stmt);
    g_sqlite3_driver.close(ctx);
}

static void test_savepoint_rollback_undoes_insert(void) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    g_sqlite3_driver.exec(ctx, "CREATE TABLE t (v INTEGER)");
    g_sqlite3_driver.savepoint(ctx, "sp_test");
    g_sqlite3_driver.exec(ctx, "INSERT INTO t VALUES (99)");
    /* ROLLBACK TO keeps the savepoint alive; RELEASE removes it */
    g_sqlite3_driver.rollback_sp(ctx, "sp_test");
    g_sqlite3_driver.release_sp(ctx, "sp_test");

    void *stmt = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT COUNT(*) FROM t", &stmt);
    bool has_row = false;
    g_sqlite3_driver.step(stmt, &has_row);
    int64_t count = -1;
    g_sqlite3_driver.column_int64(stmt, 0, &count);
    TEST_ASSERT(count == 0);
    g_sqlite3_driver.finalize(stmt);
    g_sqlite3_driver.close(ctx);
}

static void test_changes_after_delete(void) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    g_sqlite3_driver.exec(ctx, "CREATE TABLE t (v INTEGER)");
    g_sqlite3_driver.exec(ctx, "INSERT INTO t VALUES (1)");
    g_sqlite3_driver.exec(ctx, "INSERT INTO t VALUES (2)");
    g_sqlite3_driver.exec(ctx, "INSERT INTO t VALUES (3)");
    g_sqlite3_driver.exec(ctx, "DELETE FROM t WHERE v < 3");
    int n = g_sqlite3_driver.changes(ctx);
    TEST_ASSERT(n == 2);
    g_sqlite3_driver.close(ctx);
}

/* ------------------------------------------------------------------ */
/* Task 2.3: prepare/bind/step/column/reset/finalize                   */
/* ------------------------------------------------------------------ */

static void *open_with_table(const char *ddl) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    g_sqlite3_driver.exec(ctx, ddl);
    return ctx;
}

static void test_prepare_invalid_sql(void) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    void *stmt = NULL;
    SqlRDBResult r = g_sqlite3_driver.prepare(ctx, "NOT SQL AT ALL", &stmt);
    TEST_ASSERT(r == SQL_RDB_ERR_DRIVER);
    TEST_ASSERT(stmt == NULL);
    g_sqlite3_driver.close(ctx);
}

static void test_bind_int64_roundtrip(void) {
    void *ctx = open_with_table("CREATE TABLE t (v INTEGER)");
    void *ins = NULL;
    g_sqlite3_driver.prepare(ctx, "INSERT INTO t VALUES (?)", &ins);
    g_sqlite3_driver.bind_int64(ins, 1, (int64_t)9876543210LL);
    bool has_row = false;
    g_sqlite3_driver.step(ins, &has_row);
    g_sqlite3_driver.finalize(ins);

    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT v FROM t", &sel);
    g_sqlite3_driver.step(sel, &has_row);
    TEST_ASSERT(has_row);
    int64_t v = 0;
    g_sqlite3_driver.column_int64(sel, 0, &v);
    TEST_ASSERT(v == (int64_t)9876543210LL);
    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

static void test_bind_int32_roundtrip(void) {
    void *ctx = open_with_table("CREATE TABLE t (v INTEGER)");
    void *ins = NULL;
    g_sqlite3_driver.prepare(ctx, "INSERT INTO t VALUES (?)", &ins);
    g_sqlite3_driver.bind_int32(ins, 1, 42);
    bool has_row = false;
    g_sqlite3_driver.step(ins, &has_row);
    g_sqlite3_driver.finalize(ins);

    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT v FROM t", &sel);
    g_sqlite3_driver.step(sel, &has_row);
    TEST_ASSERT(has_row);
    int32_t v = 0;
    g_sqlite3_driver.column_int32(sel, 0, &v);
    TEST_ASSERT(v == 42);
    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

static void test_bind_real_roundtrip(void) {
    void *ctx = open_with_table("CREATE TABLE t (v REAL)");
    void *ins = NULL;
    g_sqlite3_driver.prepare(ctx, "INSERT INTO t VALUES (?)", &ins);
    g_sqlite3_driver.bind_real(ins, 1, 3.14);
    bool has_row = false;
    g_sqlite3_driver.step(ins, &has_row);
    g_sqlite3_driver.finalize(ins);

    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT v FROM t", &sel);
    g_sqlite3_driver.step(sel, &has_row);
    TEST_ASSERT(has_row);
    double v = 0.0;
    g_sqlite3_driver.column_real(sel, 0, &v);
    TEST_ASSERT(v == 3.14);
    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

static void test_bind_text_roundtrip(void) {
    void *ctx = open_with_table("CREATE TABLE t (v TEXT)");
    void *ins = NULL;
    g_sqlite3_driver.prepare(ctx, "INSERT INTO t VALUES (?)", &ins);
    g_sqlite3_driver.bind_text(ins, 1, "hello", -1);
    bool has_row = false;
    g_sqlite3_driver.step(ins, &has_row);
    g_sqlite3_driver.finalize(ins);

    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT v FROM t", &sel);
    g_sqlite3_driver.step(sel, &has_row);
    TEST_ASSERT(has_row);
    const char *ptr = NULL;
    size_t len = 0;
    g_sqlite3_driver.column_text(sel, 0, &ptr, &len);
    TEST_ASSERT(ptr != NULL);
    TEST_ASSERT(strcmp(ptr, "hello") == 0);
    TEST_ASSERT(len == 5);
    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

static void test_bind_blob_roundtrip(void) {
    void *ctx = open_with_table("CREATE TABLE t (v BLOB)");
    const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    void *ins = NULL;
    g_sqlite3_driver.prepare(ctx, "INSERT INTO t VALUES (?)", &ins);
    g_sqlite3_driver.bind_blob(ins, 1, data, sizeof(data));
    bool has_row = false;
    g_sqlite3_driver.step(ins, &has_row);
    g_sqlite3_driver.finalize(ins);

    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT v FROM t", &sel);
    g_sqlite3_driver.step(sel, &has_row);
    TEST_ASSERT(has_row);
    const void *ptr = NULL;
    size_t len = 0;
    g_sqlite3_driver.column_blob(sel, 0, &ptr, &len);
    TEST_ASSERT(ptr != NULL);
    TEST_ASSERT(len == sizeof(data));
    TEST_ASSERT(memcmp(ptr, data, sizeof(data)) == 0);
    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

static void test_bind_null_column_isnull(void) {
    void *ctx = open_with_table("CREATE TABLE t (v INTEGER)");
    void *ins = NULL;
    g_sqlite3_driver.prepare(ctx, "INSERT INTO t VALUES (?)", &ins);
    g_sqlite3_driver.bind_null(ins, 1);
    bool has_row = false;
    g_sqlite3_driver.step(ins, &has_row);
    g_sqlite3_driver.finalize(ins);

    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT v FROM t", &sel);
    g_sqlite3_driver.step(sel, &has_row);
    TEST_ASSERT(has_row);
    bool is_null = false;
    g_sqlite3_driver.column_isnull(sel, 0, &is_null);
    TEST_ASSERT(is_null);
    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

static void test_column_isnull_false_for_value(void) {
    void *ctx = open_with_table("CREATE TABLE t (v INTEGER)");
    g_sqlite3_driver.exec(ctx, "INSERT INTO t VALUES (5)");
    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT v FROM t", &sel);
    bool has_row = false;
    g_sqlite3_driver.step(sel, &has_row);
    bool is_null = true;
    g_sqlite3_driver.column_isnull(sel, 0, &is_null);
    TEST_ASSERT(!is_null);
    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

static void test_step_no_rows(void) {
    void *ctx = open_with_table("CREATE TABLE t (v INTEGER)");
    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT v FROM t", &sel);
    bool has_row = true;
    SqlRDBResult r = g_sqlite3_driver.step(sel, &has_row);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(!has_row);
    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

static void test_reset_allows_restep(void) {
    void *ctx = open_with_table("CREATE TABLE t (v INTEGER)");
    g_sqlite3_driver.exec(ctx, "INSERT INTO t VALUES (7)");
    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT v FROM t", &sel);
    bool has_row = false;
    g_sqlite3_driver.step(sel, &has_row);
    TEST_ASSERT(has_row);

    g_sqlite3_driver.reset(sel);
    has_row = false;
    g_sqlite3_driver.step(sel, &has_row);
    TEST_ASSERT(has_row);
    int64_t v = 0;
    g_sqlite3_driver.column_int64(sel, 0, &v);
    TEST_ASSERT(v == 7);
    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void) {
    /* Task 2.1: vtable定義 */
    test_vtable_name();
    test_vtable_all_functions_set();

    /* Task 2.2: 接続・exec・トランザクション */
    test_open_memory();
    test_open_invalid_path();
    test_exec_valid_sql();
    test_exec_invalid_sql();
    test_begin_commit();
    test_begin_rollback_undoes_insert();
    test_savepoint_release();
    test_savepoint_rollback_undoes_insert();
    test_changes_after_delete();

    /* Task 2.3: prepare/bind/step/column/reset/finalize */
    test_prepare_invalid_sql();
    test_bind_int64_roundtrip();
    test_bind_int32_roundtrip();
    test_bind_real_roundtrip();
    test_bind_text_roundtrip();
    test_bind_blob_roundtrip();
    test_bind_null_column_isnull();
    test_column_isnull_false_for_value();
    test_step_no_rows();
    test_reset_allows_restep();

    TEST_SUMMARY();
}
