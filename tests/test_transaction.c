/*
 * test_transaction.c — Integration tests for Task 11: Transaction control.
 *
 * Covers Requirements 10.1〜10.6:
 *   - BeginTx / CommitTx / RollbackTx happy paths
 *   - NESTED transactions mapped to SAVEPOINTs (up to depth 16)
 *   - depth==0 → SQL_RDB_ERR_NO_ACTIVE_TX on Commit/Rollback
 *   - depth>=16 → SQL_RDB_ERR_NESTED_TX on Begin
 *   - Mid-transaction writes participate in the active TX
 */
#include "c2sql.h"
#include "harness.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    int32_t id;
    char    name[32];
    double  score;
} TestPerson;

static const SqlRDBColumnDef PERSON_COLS[] = {
    { "id",    SQL_TYPE_INT32, offsetof(TestPerson, id),    4,              SQL_COL_FLAG_PRIMARY_KEY },
    { "name",  SQL_TYPE_TEXT,  offsetof(TestPerson, name),  32,             SQL_COL_FLAG_NONE        },
    { "score", SQL_TYPE_REAL,  offsetof(TestPerson, score), sizeof(double), SQL_COL_FLAG_NULLABLE    },
};
#define PERSON_COL_COUNT 3

static SqlRDBHandle *open_persons(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    if (!h) return NULL;
    if (SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT) != SQL_RDB_OK) {
        SqlRDBClose(h); return NULL;
    }
    return h;
}

static size_t count_persons(SqlRDBHandle *h) {
    SqlRDBCondition *c = SqlRDBCondAll();
    SqlRDBStmt *iter = NULL;
    if (SqlRDBReadMany(h, "persons", c, &iter) != SQL_RDB_OK) {
        SqlRDBCondFree(c);
        return SIZE_MAX;
    }
    SqlRDBCondFree(c);
    size_t n = 0;
    TestPerson out;
    while (SqlRDBStmtNext(iter, &out, NULL) == SQL_RDB_OK) n++;
    SqlRDBStmtFree(iter);
    return n;
}

static bool person_exists(SqlRDBHandle *h, int32_t id) {
    SqlRDBCondition *c = SqlRDBCondInt("id", SQL_OP_EQ, id);
    TestPerson out = {0};
    SqlRDBResult r = SqlRDBRead(h, "persons", c, &out, NULL, NULL);
    SqlRDBCondFree(c);
    return r == SQL_RDB_OK;
}

/* ================================================================== */
/* Error cases: invalid handle, NULL                                  */
/* ================================================================== */

static void test_begin_null_handle(void) {
    TEST_ASSERT(SqlRDBBeginTx(NULL)    == SQL_RDB_ERR_INVALID_HANDLE);
    TEST_ASSERT(SqlRDBCommitTx(NULL)   == SQL_RDB_ERR_INVALID_HANDLE);
    TEST_ASSERT(SqlRDBRollbackTx(NULL) == SQL_RDB_ERR_INVALID_HANDLE);
}

/* depth==0 → NO_ACTIVE_TX on Commit/Rollback (Req 10.4) */
static void test_commit_without_begin(void) {
    SqlRDBHandle *h = open_persons();
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(SqlRDBCommitTx(h)   == SQL_RDB_ERR_NO_ACTIVE_TX);
    TEST_ASSERT(SqlRDBRollbackTx(h) == SQL_RDB_ERR_NO_ACTIVE_TX);
    SqlRDBClose(h);
}

/* ================================================================== */
/* Happy paths                                                         */
/* ================================================================== */

/* Commit persists writes (Req 10.1, 10.2, 10.5) */
static void test_begin_write_commit(void) {
    SqlRDBHandle *h = open_persons();
    TEST_ASSERT(h != NULL);

    TEST_ASSERT(SqlRDBBeginTx(h) == SQL_RDB_OK);
    TestPerson p = { 1, "Alice", 9.5 };
    TEST_ASSERT(SqlRDBWrite(h, "persons", &p, NULL) == SQL_RDB_OK);
    TEST_ASSERT(SqlRDBCommitTx(h) == SQL_RDB_OK);

    TEST_ASSERT(person_exists(h, 1));
    /* second commit must fail (no active TX) */
    TEST_ASSERT(SqlRDBCommitTx(h) == SQL_RDB_ERR_NO_ACTIVE_TX);

    SqlRDBClose(h);
}

/* Rollback discards writes (Req 10.3, 10.5) */
static void test_begin_write_rollback(void) {
    SqlRDBHandle *h = open_persons();
    TEST_ASSERT(h != NULL);

    TEST_ASSERT(SqlRDBBeginTx(h) == SQL_RDB_OK);
    TestPerson p = { 7, "Ghost", 0.0 };
    TEST_ASSERT(SqlRDBWrite(h, "persons", &p, NULL) == SQL_RDB_OK);
    TEST_ASSERT(SqlRDBRollbackTx(h) == SQL_RDB_OK);

    TEST_ASSERT(!person_exists(h, 7));
    TEST_ASSERT(count_persons(h) == 0);

    SqlRDBClose(h);
}

/* ================================================================== */
/* Nesting (SAVEPOINT) — Req 10.6                                      */
/* ================================================================== */

/* Inner rollback discards only inner writes; outer commit keeps outer writes. */
static void test_nested_inner_rollback_outer_commit(void) {
    SqlRDBHandle *h = open_persons();
    TEST_ASSERT(h != NULL);

    TEST_ASSERT(SqlRDBBeginTx(h) == SQL_RDB_OK);              /* depth 1 (BEGIN) */
    TestPerson outer = { 1, "Outer", 1.0 };
    TEST_ASSERT(SqlRDBWrite(h, "persons", &outer, NULL) == SQL_RDB_OK);

    TEST_ASSERT(SqlRDBBeginTx(h) == SQL_RDB_OK);              /* depth 2 (SAVEPOINT sp_2) */
    TestPerson inner = { 2, "Inner", 2.0 };
    TEST_ASSERT(SqlRDBWrite(h, "persons", &inner, NULL) == SQL_RDB_OK);
    TEST_ASSERT(SqlRDBRollbackTx(h) == SQL_RDB_OK);           /* ROLLBACK TO sp_2 */

    TEST_ASSERT(SqlRDBCommitTx(h) == SQL_RDB_OK);              /* COMMIT outer */

    TEST_ASSERT(person_exists(h, 1));
    TEST_ASSERT(!person_exists(h, 2));

    SqlRDBClose(h);
}

/* Inner commit (RELEASE sp_N) preserves inner writes; outer rollback discards all. */
static void test_nested_inner_commit_outer_rollback(void) {
    SqlRDBHandle *h = open_persons();
    TEST_ASSERT(h != NULL);

    TEST_ASSERT(SqlRDBBeginTx(h) == SQL_RDB_OK);
    TestPerson outer = { 1, "Outer", 1.0 };
    TEST_ASSERT(SqlRDBWrite(h, "persons", &outer, NULL) == SQL_RDB_OK);

    TEST_ASSERT(SqlRDBBeginTx(h) == SQL_RDB_OK);
    TestPerson inner = { 2, "Inner", 2.0 };
    TEST_ASSERT(SqlRDBWrite(h, "persons", &inner, NULL) == SQL_RDB_OK);
    TEST_ASSERT(SqlRDBCommitTx(h) == SQL_RDB_OK);              /* RELEASE sp_2 */

    TEST_ASSERT(SqlRDBRollbackTx(h) == SQL_RDB_OK);            /* outer rollback */

    TEST_ASSERT(!person_exists(h, 1));
    TEST_ASSERT(!person_exists(h, 2));

    SqlRDBClose(h);
}

/* 16 nested levels work, the 17th must return NESTED_TX. */
static void test_nested_overflow_returns_error(void) {
    SqlRDBHandle *h = open_persons();
    TEST_ASSERT(h != NULL);

    for (int i = 0; i < 16; i++) {
        TEST_ASSERT(SqlRDBBeginTx(h) == SQL_RDB_OK);
    }
    /* 17th must fail */
    TEST_ASSERT(SqlRDBBeginTx(h) == SQL_RDB_ERR_NESTED_TX);

    /* Unwind all 16 frames cleanly */
    for (int i = 0; i < 16; i++) {
        TEST_ASSERT(SqlRDBCommitTx(h) == SQL_RDB_OK);
    }
    TEST_ASSERT(SqlRDBCommitTx(h) == SQL_RDB_ERR_NO_ACTIVE_TX);

    SqlRDBClose(h);
}

/* Close while a transaction is active must roll back (no segfault, no leak). */
static void test_close_during_active_tx(void) {
    SqlRDBHandle *h = open_persons();
    TEST_ASSERT(h != NULL);

    TEST_ASSERT(SqlRDBBeginTx(h) == SQL_RDB_OK);
    TestPerson p = { 9, "Pending", 1.0 };
    TEST_ASSERT(SqlRDBWrite(h, "persons", &p, NULL) == SQL_RDB_OK);

    /* close without commit/rollback — must roll back implicitly */
    TEST_ASSERT(SqlRDBClose(h) == SQL_RDB_OK);
}

/* WriteMany inside an explicit TX must NOT issue its own implicit COMMIT. */
static void test_write_many_inside_tx(void) {
    SqlRDBHandle *h = open_persons();
    TEST_ASSERT(h != NULL);

    TEST_ASSERT(SqlRDBBeginTx(h) == SQL_RDB_OK);

    TestPerson rows[] = {
        { 1, "A", 1.0 },
        { 2, "B", 2.0 },
    };
    TEST_ASSERT(SqlRDBWriteMany(h, "persons", rows, 2, sizeof(TestPerson), NULL) == SQL_RDB_OK);

    /* roll back the outer TX — both rows must be gone */
    TEST_ASSERT(SqlRDBRollbackTx(h) == SQL_RDB_OK);
    TEST_ASSERT(count_persons(h) == 0);

    SqlRDBClose(h);
}

int main(void) {
    test_begin_null_handle();
    test_commit_without_begin();
    test_begin_write_commit();
    test_begin_write_rollback();
    test_nested_inner_rollback_outer_commit();
    test_nested_inner_commit_outer_rollback();
    test_nested_overflow_returns_error();
    test_close_during_active_tx();
    test_write_many_inside_tx();

    TEST_SUMMARY();
}
