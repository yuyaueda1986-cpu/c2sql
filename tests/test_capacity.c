/*
 * test_capacity.c — SqlRDBCount and the generated capacity guard.
 *
 * Exercises the Phase 2 additions end to end using the generated "sessions"
 * schema (max_records=3, enforce_max_records=true):
 *   - SqlRDBCount on empty / populated tables, with and without a condition
 *   - WriteSessionsGuarded rejecting an INSERT past capacity
 *   - UPSERT of an existing key remaining allowed at capacity
 *   - capacity freeing up after a delete
 *   - SqlRDBCount argument / error paths
 */
#include "c2sql.h"
#include "sessions_schema.h" /* AUTO-GENERATED from specs/sessions.json */
#include "harness.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(RegisterSessions(h) == SQL_RDB_OK);

    size_t n = 12345;
    TEST_ASSERT(SqlRDBCount(h, "sessions", NULL, &n) == SQL_RDB_OK);
    TEST_ASSERT(n == 0);

    /* Fill to capacity (3) via the guarded writer. */
    for (int i = 1; i <= 3; i++) {
        Session s = { .id = i };
        snprintf(s.token, sizeof(s.token), "tok-%d", i);
        TEST_ASSERT(WriteSessionsGuarded(h, &s, NULL) == SQL_RDB_OK);
    }
    TEST_ASSERT(SqlRDBCount(h, "sessions", NULL, &n) == SQL_RDB_OK);
    TEST_ASSERT(n == 3);

    /* A 4th distinct INSERT is rejected and leaves the table unchanged. */
    Session s4 = { .id = 4 };
    snprintf(s4.token, sizeof(s4.token), "tok-4");
    TEST_ASSERT(WriteSessionsGuarded(h, &s4, NULL) == SQL_RDB_ERR_CAPACITY_EXCEEDED);
    TEST_ASSERT(SqlRDBCount(h, "sessions", NULL, &n) == SQL_RDB_OK);
    TEST_ASSERT(n == 3);

    /* UPSERT of an existing key at capacity is allowed and updates in place. */
    Session upd = { .id = 2 };
    snprintf(upd.token, sizeof(upd.token), "tok-2-updated");
    TEST_ASSERT(WriteSessionsGuarded(h, &upd, NULL) == SQL_RDB_OK);
    TEST_ASSERT(SqlRDBCount(h, "sessions", NULL, &n) == SQL_RDB_OK);
    TEST_ASSERT(n == 3);

    Session out = {0};
    SqlRDBCondition *c = SqlRDBCondInt("id", SQL_OP_EQ, 2);
    TEST_ASSERT(SqlRDBRead(h, "sessions", c, &out, NULL, NULL) == SQL_RDB_OK);
    SqlRDBCondFree(c);
    TEST_ASSERT(strcmp(out.token, "tok-2-updated") == 0);

    /* Conditional count: ids 2 and 3 satisfy id >= 2. */
    SqlRDBCondition *ge2 = SqlRDBCondInt("id", SQL_OP_GE, 2);
    TEST_ASSERT(SqlRDBCount(h, "sessions", ge2, &n) == SQL_RDB_OK);
    SqlRDBCondFree(ge2);
    TEST_ASSERT(n == 2);

    /* Freeing a slot lets a new INSERT through the guard again. */
    SqlRDBCondition *del = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    size_t deleted = 0;
    TEST_ASSERT(SqlRDBDelete(h, "sessions", del, &deleted) == SQL_RDB_OK);
    SqlRDBCondFree(del);
    TEST_ASSERT(deleted == 1);
    TEST_ASSERT(WriteSessionsGuarded(h, &s4, NULL) == SQL_RDB_OK);
    TEST_ASSERT(SqlRDBCount(h, "sessions", NULL, &n) == SQL_RDB_OK);
    TEST_ASSERT(n == 3);

    /* Argument / error paths. */
    TEST_ASSERT(SqlRDBCount(h, "nope", NULL, &n) == SQL_RDB_ERR_UNKNOWN_STRUCT);
    TEST_ASSERT(SqlRDBCount(h, "sessions", NULL, NULL) == SQL_RDB_ERR_INVALID_ARG);
    TEST_ASSERT(SqlRDBCount(NULL, "sessions", NULL, &n) == SQL_RDB_ERR_INVALID_HANDLE);

    SqlRDBCondition *bad = SqlRDBCondInt("nosuch", SQL_OP_EQ, 1);
    TEST_ASSERT(SqlRDBCount(h, "sessions", bad, &n) == SQL_RDB_ERR_UNKNOWN_COLUMN);
    SqlRDBCondFree(bad);

    TEST_ASSERT(SqlRDBClose(h) == SQL_RDB_OK);
    TEST_SUMMARY();
}
