/*
 * generated_crud.c — libc2sql sample: using c2sql-gen output.
 *
 * Unlike basic_crud.c, this example does NOT hand-write a SqlRDBColumnDef[]
 * array. Instead it includes the header produced by tools/c2sql_gen.py from
 * specs/persons.json and calls the generated RegisterPersons() helper. The
 * Person struct, column table, and PERSONS_MAX_RECORDS metadata all come from
 * the generated unit, so struct layout and schema can never drift apart.
 *
 * Built and run by ctest as a smoke test for the code-generation pipeline.
 */
#include "c2sql.h"
#include "persons_schema.h" /* AUTO-GENERATED from specs/persons.json */

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    assert(h != NULL);

    /* Generated registration helper — no hand-written column array. */
    assert(RegisterPersons(h) == SQL_RDB_OK);

    Person alice = { 1, "Alice", 9.5 };
    assert(SqlRDBWrite(h, "persons", &alice, NULL) == SQL_RDB_OK);

    Person out = {0};
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    assert(SqlRDBRead(h, "persons", cond, &out, NULL, NULL) == SQL_RDB_OK);
    SqlRDBCondFree(cond);

    assert(out.id == 1);
    assert(strcmp(out.name, "Alice") == 0);
    assert(out.score == 9.5);

    printf("generated_crud: ok (table=persons, max_records=%d)\n",
           PERSONS_MAX_RECORDS);

    assert(SqlRDBClose(h) == SQL_RDB_OK);
    return 0;
}
