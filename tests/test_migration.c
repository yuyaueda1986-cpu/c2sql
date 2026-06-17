/*
 * test_migration.c — Tests for Task 13: schema match and auto-migration.
 *
 * Strategy: use SqlRDBInit then reach through handle_internal.h to call the
 * driver directly to pre-create tables with arbitrary schemas, before invoking
 * the public SqlRDBRegisterStruct so we can verify the migration code path.
 *
 * Covers Requirements 3.2 (existing match), 3.3 (mismatch → error),
 * 3.4 (auto_migrate adds trailing columns), 3.5 (non-STRICT handling).
 */
#include "c2sql.h"
#include "handle_internal.h"   /* direct driver access for test setup */
#include "harness.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    int32_t id;
    char    name[32];
    double  score;
} Person;

static const SqlRDBColumnDef PERSON_COLS[] = {
    { "id",    SQL_TYPE_INT32, offsetof(Person, id),    4,              SQL_COL_FLAG_PRIMARY_KEY },
    { "name",  SQL_TYPE_TEXT,  offsetof(Person, name),  32,             SQL_COL_FLAG_NONE        },
    { "score", SQL_TYPE_REAL,  offsetof(Person, score), sizeof(double), SQL_COL_FLAG_NULLABLE    },
};
#define PERSON_COL_COUNT 3

/* Drop the table directly (for tests that pre-create a custom layout). */
static void drop_table(SqlRDBHandle *h, const char *name) {
    char sql[128];
    snprintf(sql, sizeof(sql), "DROP TABLE IF EXISTS \"%s\"", name);
    h->driver->exec(h->driver_ctx, sql);
}

static void exec_or_fail(SqlRDBHandle *h, const char *sql) {
    SqlRDBResult r = h->driver->exec(h->driver_ctx, sql);
    if (r != SQL_RDB_OK) {
        fprintf(stderr, "  exec failed: %s\n", sql);
    }
    TEST_ASSERT(r == SQL_RDB_OK);
}

/* ================================================================== */
/* 13.1: schema match / mismatch                                       */
/* ================================================================== */

/* New table — must register successfully (regression: existing CRUD flow) */
static void test_register_new_table(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT) == SQL_RDB_OK);
    SqlRDBClose(h);
}

/* Pre-existing identical table — must accept */
static void test_register_matching_existing_table(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    exec_or_fail(h,
        "CREATE TABLE \"persons\" ("
        "\"id\" INTEGER NOT NULL PRIMARY KEY,"
        "\"name\" TEXT NOT NULL,"
        "\"score\" REAL"
        ") STRICT");
    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT) == SQL_RDB_OK);
    SqlRDBClose(h);
}

/* Pre-existing table with an extra column — must reject */
static void test_register_extra_column(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    exec_or_fail(h,
        "CREATE TABLE \"persons\" ("
        "\"id\" INTEGER NOT NULL PRIMARY KEY,"
        "\"name\" TEXT NOT NULL,"
        "\"score\" REAL,"
        "\"extra\" INTEGER"
        ") STRICT");
    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT)
                == SQL_RDB_ERR_SCHEMA_MISMATCH);
    SqlRDBClose(h);
}

/* Pre-existing table with a missing trailing column — reject when auto_migrate off */
static void test_register_missing_column_no_automigrate(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    exec_or_fail(h,
        "CREATE TABLE \"persons\" ("
        "\"id\" INTEGER NOT NULL PRIMARY KEY,"
        "\"name\" TEXT NOT NULL"
        ") STRICT");
    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT)
                == SQL_RDB_ERR_SCHEMA_MISMATCH);
    SqlRDBClose(h);
}

/* Reordered columns — never auto-fixed (Req 3.4) */
static void test_register_reordered_columns(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    exec_or_fail(h,
        "CREATE TABLE \"persons\" ("
        "\"name\" TEXT NOT NULL,"            /* was second; now first */
        "\"id\"   INTEGER NOT NULL PRIMARY KEY,"
        "\"score\" REAL"
        ") STRICT");
    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT)
                == SQL_RDB_ERR_SCHEMA_MISMATCH);
    SqlRDBClose(h);
}

/* Different type class — reject */
static void test_register_diff_type(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    exec_or_fail(h,
        "CREATE TABLE \"persons\" ("
        "\"id\" INTEGER NOT NULL PRIMARY KEY,"
        "\"name\" TEXT NOT NULL,"
        "\"score\" TEXT"                       /* schema expects REAL */
        ") STRICT");
    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT)
                == SQL_RDB_ERR_SCHEMA_MISMATCH);
    SqlRDBClose(h);
}

/* Different NOT NULL — reject */
static void test_register_diff_notnull(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    exec_or_fail(h,
        "CREATE TABLE \"persons\" ("
        "\"id\" INTEGER NOT NULL PRIMARY KEY,"
        "\"name\" TEXT,"                        /* schema expects NOT NULL */
        "\"score\" REAL"
        ") STRICT");
    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT)
                == SQL_RDB_ERR_SCHEMA_MISMATCH);
    SqlRDBClose(h);
}

/* Non-STRICT table — accepted when require_strict=false (default) */
static void test_register_non_strict_default(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    exec_or_fail(h,
        "CREATE TABLE \"persons\" ("
        "\"id\" INTEGER NOT NULL PRIMARY KEY,"
        "\"name\" TEXT NOT NULL,"
        "\"score\" REAL"
        ")");                                  /* no STRICT */
    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT) == SQL_RDB_OK);
    SqlRDBClose(h);
}

/* Non-STRICT table — rejected when require_strict=true */
static void test_register_non_strict_with_require_strict(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    SqlRDBConfig cfg = {
        .threadsafe = true, .stmt_cache_size = 64, .auto_migrate = false,
        .multirow_default = 0, .require_strict = true,
    };
    TEST_ASSERT(SqlRDBSetConfig(h, &cfg) == SQL_RDB_OK);
    exec_or_fail(h,
        "CREATE TABLE \"persons\" ("
        "\"id\" INTEGER NOT NULL PRIMARY KEY,"
        "\"name\" TEXT NOT NULL,"
        "\"score\" REAL"
        ")");
    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT)
                == SQL_RDB_ERR_SCHEMA_MISMATCH);
    SqlRDBClose(h);
}

/* ================================================================== */
/* 13.2: auto_migrate                                                  */
/* ================================================================== */

/* Trailing missing column + auto_migrate=true → ALTER TABLE adds it. */
static void test_automigrate_adds_trailing_column(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);

    SqlRDBConfig cfg = {
        .threadsafe = true, .stmt_cache_size = 64, .auto_migrate = true,
        .multirow_default = 0, .require_strict = false,
    };
    TEST_ASSERT(SqlRDBSetConfig(h, &cfg) == SQL_RDB_OK);

    /* Pre-existing table missing 'score' (nullable trailing column) */
    exec_or_fail(h,
        "CREATE TABLE \"persons\" ("
        "\"id\" INTEGER NOT NULL PRIMARY KEY,"
        "\"name\" TEXT NOT NULL"
        ") STRICT");

    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT) == SQL_RDB_OK);

    /* After migration: write/read must work and observe the score column */
    Person p = { 1, "Alice", 9.5 };
    TEST_ASSERT(SqlRDBWrite(h, "persons", &p, NULL) == SQL_RDB_OK);

    Person out = {0};
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    TEST_ASSERT(SqlRDBRead(h, "persons", cond, &out, NULL, NULL) == SQL_RDB_OK);
    TEST_ASSERT(out.id == 1);
    TEST_ASSERT(strcmp(out.name, "Alice") == 0);
    TEST_ASSERT(out.score == 9.5);
    SqlRDBCondFree(cond);

    SqlRDBClose(h);
}

/* auto_migrate does NOT fix reordered columns (Req 3.4) */
static void test_automigrate_does_not_fix_reorder(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    SqlRDBConfig cfg = {
        .threadsafe = true, .stmt_cache_size = 64, .auto_migrate = true,
        .multirow_default = 0, .require_strict = false,
    };
    TEST_ASSERT(SqlRDBSetConfig(h, &cfg) == SQL_RDB_OK);

    exec_or_fail(h,
        "CREATE TABLE \"persons\" ("
        "\"name\" TEXT NOT NULL,"
        "\"id\" INTEGER NOT NULL PRIMARY KEY,"
        "\"score\" REAL"
        ") STRICT");

    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT)
                == SQL_RDB_ERR_SCHEMA_MISMATCH);

    SqlRDBClose(h);
    (void)drop_table;  /* silence unused-helper warning if any */
}

int main(void) {
    /* 13.1 */
    test_register_new_table();
    test_register_matching_existing_table();
    test_register_extra_column();
    test_register_missing_column_no_automigrate();
    test_register_reordered_columns();
    test_register_diff_type();
    test_register_diff_notnull();
    test_register_non_strict_default();
    test_register_non_strict_with_require_strict();

    /* 13.2 */
    test_automigrate_adds_trailing_column();
    test_automigrate_does_not_fix_reorder();

    TEST_SUMMARY();
}
