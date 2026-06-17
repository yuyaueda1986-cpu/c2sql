/*
 * basic_crud.c — libc2sql sample: 3-step API (Init → Register → Write/Read).
 *
 * Demonstrates:
 *   - SqlRDBInit / SqlRDBClose lifecycle
 *   - SqlRDBRegisterStruct with PRIMARY KEY and NULLABLE columns
 *   - SqlRDBWrite (UPSERT via PK) without a NULL bitmap (pass NULL)
 *   - SqlRDBRead single-row retrieval with a search condition
 *   - SqlRDBDelete with explicit condition
 *
 * This file is built as part of `make examples` and is also invoked by ctest
 * as a smoke test; assertions abort on any unexpected return code.
 */
#include "c2sql.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int32_t id;
    char    name[32];
    double  score;       /* may be NULL on disk (NULLABLE flag below) */
} Person;

static const SqlRDBColumnDef PERSON_COLS[] = {
    { "id",    SQL_TYPE_INT32, offsetof(Person, id),    4,              SQL_COL_FLAG_PRIMARY_KEY },
    { "name",  SQL_TYPE_TEXT,  offsetof(Person, name),  32,             SQL_COL_FLAG_NONE        },
    { "score", SQL_TYPE_REAL,  offsetof(Person, score), sizeof(double), SQL_COL_FLAG_NULLABLE    },
};
#define PERSON_COL_COUNT (sizeof(PERSON_COLS) / sizeof(PERSON_COLS[0]))

int main(void) {
    /* Step 1: open a database handle (in-memory for this example) */
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    assert(h != NULL);

    /* Step 2: register the struct → libc2sql issues CREATE TABLE for us */
    assert(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT) == SQL_RDB_OK);

    /* Step 3: write rows by passing struct pointers.
     * The null_map argument is optional — pass NULL when no column should be SQL NULL. */
    Person alice = { 1, "Alice", 9.5 };
    Person bob   = { 2, "Bob",   7.0 };
    assert(SqlRDBWrite(h, "persons", &alice, NULL) == SQL_RDB_OK);
    assert(SqlRDBWrite(h, "persons", &bob,   NULL) == SQL_RDB_OK);

    /* Same PK → UPSERT updates the existing row instead of inserting a duplicate. */
    Person bob_upd = { 2, "Bob (updated)", 8.0 };
    assert(SqlRDBWrite(h, "persons", &bob_upd, NULL) == SQL_RDB_OK);

    /* Read by primary key */
    Person out = {0};
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 2);
    assert(SqlRDBRead(h, "persons", cond, &out, NULL, NULL) == SQL_RDB_OK);
    SqlRDBCondFree(cond);

    assert(out.id == 2);
    assert(strcmp(out.name, "Bob (updated)") == 0);
    assert(out.score == 8.0);
    printf("read id=%d name=%s score=%.1f\n", out.id, out.name, out.score);

    /* Delete with an explicit condition (NULL would be rejected as a safety
     * measure to prevent accidental full-table deletes). */
    size_t deleted = 0;
    SqlRDBCondition *del_cond = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    assert(SqlRDBDelete(h, "persons", del_cond, &deleted) == SQL_RDB_OK);
    SqlRDBCondFree(del_cond);
    assert(deleted == 1);

    /* Close releases all schema, cache, and driver resources. */
    assert(SqlRDBClose(h) == SQL_RDB_OK);
    printf("basic_crud: ok\n");
    return 0;
}
