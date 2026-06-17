/*
 * transactions.c — libc2sql sample: explicit transactions and savepoints.
 *
 * Demonstrates:
 *   - SqlRDBBeginTx / SqlRDBCommitTx persist writes
 *   - SqlRDBRollbackTx discards writes
 *   - Nested SqlRDBBeginTx → SAVEPOINT semantics (inner rollback keeps outer
 *     work intact)
 *
 * Built as part of `make examples` and run as a ctest smoke test.
 */
#include "c2sql.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int32_t id;
    char    note[32];
} Entry;

static const SqlRDBColumnDef ENTRY_COLS[] = {
    { "id",   SQL_TYPE_INT32, offsetof(Entry, id),   4,  SQL_COL_FLAG_PRIMARY_KEY },
    { "note", SQL_TYPE_TEXT,  offsetof(Entry, note), 32, SQL_COL_FLAG_NONE        },
};
#define ENTRY_COL_COUNT (sizeof(ENTRY_COLS) / sizeof(ENTRY_COLS[0]))

static bool exists(SqlRDBHandle *h, int32_t id) {
    Entry            tmp  = {0};
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, id);
    SqlRDBResult     r    = SqlRDBRead(h, "entries", cond, &tmp, NULL, NULL);
    SqlRDBCondFree(cond);
    return r == SQL_RDB_OK;
}

int main(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    assert(h != NULL);
    assert(SqlRDBRegisterStruct(h, "entries", ENTRY_COLS, ENTRY_COL_COUNT) == SQL_RDB_OK);

    /* Commit example: row should persist after CommitTx */
    assert(SqlRDBBeginTx(h) == SQL_RDB_OK);
    Entry e1 = { 1, "kept-on-commit" };
    assert(SqlRDBWrite(h, "entries", &e1, NULL) == SQL_RDB_OK);
    assert(SqlRDBCommitTx(h) == SQL_RDB_OK);
    assert(exists(h, 1));

    /* Rollback example: row inserted inside TX disappears on RollbackTx */
    assert(SqlRDBBeginTx(h) == SQL_RDB_OK);
    Entry e2 = { 2, "discarded" };
    assert(SqlRDBWrite(h, "entries", &e2, NULL) == SQL_RDB_OK);
    assert(SqlRDBRollbackTx(h) == SQL_RDB_OK);
    assert(!exists(h, 2));

    /* Nesting: inner rollback only undoes inner work; outer commit keeps the outer row */
    assert(SqlRDBBeginTx(h) == SQL_RDB_OK);                  /* outer */
    Entry outer = { 3, "outer-row" };
    assert(SqlRDBWrite(h, "entries", &outer, NULL) == SQL_RDB_OK);

    assert(SqlRDBBeginTx(h) == SQL_RDB_OK);                  /* inner SAVEPOINT */
    Entry inner = { 4, "inner-row-rolled-back" };
    assert(SqlRDBWrite(h, "entries", &inner, NULL) == SQL_RDB_OK);
    assert(SqlRDBRollbackTx(h) == SQL_RDB_OK);               /* ROLLBACK TO sp_2 */

    assert(SqlRDBCommitTx(h) == SQL_RDB_OK);                 /* COMMIT outer */
    assert( exists(h, 3));
    assert(!exists(h, 4));

    /* Mismatched control flow surfaces as an error code, not undefined behavior. */
    assert(SqlRDBCommitTx(h)   == SQL_RDB_ERR_NO_ACTIVE_TX);
    assert(SqlRDBRollbackTx(h) == SQL_RDB_ERR_NO_ACTIVE_TX);

    assert(SqlRDBClose(h) == SQL_RDB_OK);
    printf("transactions: ok\n");
    return 0;
}
