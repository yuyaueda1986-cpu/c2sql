/*
 * test_pg.c — PostgreSQL backend integration test.
 *
 * Runs the public API against a real PostgreSQL server. The DSN is taken from
 * the C2SQL_PG_DSN environment variable (e.g.
 * "postgresql://user:pass@localhost:5432/c2sql_test"). When the variable is
 * unset the test is skipped (exit 0) so the suite stays green on machines
 * without a database. Only built when C2SQL_WITH_POSTGRES=ON.
 */
#include "c2sql.h"
#include "harness.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define PERSON_COL_COUNT (sizeof(PERSON_COLS) / sizeof(PERSON_COLS[0]))

typedef struct {
    int32_t id;
    uint8_t blob[8];
} Doc;

static const SqlRDBColumnDef DOC_COLS[] = {
    { "id",   SQL_TYPE_INT32, offsetof(Doc, id),   4, SQL_COL_FLAG_PRIMARY_KEY },
    { "blob", SQL_TYPE_BLOB,  offsetof(Doc, blob), 8, SQL_COL_FLAG_NONE        },
};
#define DOC_COL_COUNT (sizeof(DOC_COLS) / sizeof(DOC_COLS[0]))

int main(void) {
    const char *dsn = getenv("C2SQL_PG_DSN");
    if (!dsn || !*dsn) {
        printf("test_pg: skipped (set C2SQL_PG_DSN to run)\n");
        return 0;
    }

    SqlRDBHandle *h = SqlRDBInit(dsn);
    TEST_ASSERT(h != NULL);
    if (!h) { fprintf(stderr, "cannot connect to %s\n", dsn); return 1; }

    /* auto_migrate so the trailing-column add path is exercised too. */
    SqlRDBConfig cfg = { .threadsafe = true, .stmt_cache_size = 64,
                         .auto_migrate = true, .multirow_default = 0,
                         .require_strict = false };
    TEST_ASSERT(SqlRDBSetConfig(h, &cfg) == SQL_RDB_OK);

    TEST_ASSERT(SqlRDBRegisterStruct(h, "pg_persons", PERSON_COLS, PERSON_COL_COUNT) == SQL_RDB_OK);

    /* Start clean (table may persist from a previous run). */
    SqlRDBCondition *all = SqlRDBCondAll();
    SqlRDBDelete(h, "pg_persons", all, NULL);
    SqlRDBCondFree(all);

    size_t n = 999;
    TEST_ASSERT(SqlRDBCount(h, "pg_persons", NULL, &n) == SQL_RDB_OK);
    TEST_ASSERT(n == 0);

    /* INSERT via UPSERT */
    Person alice = { 1, "Alice", 9.5 };
    Person bob   = { 2, "Bob",   7.0 };
    TEST_ASSERT(SqlRDBWrite(h, "pg_persons", &alice, NULL) == SQL_RDB_OK);
    TEST_ASSERT(SqlRDBWrite(h, "pg_persons", &bob,   NULL) == SQL_RDB_OK);

    /* UPSERT update of an existing PK */
    Person bob2 = { 2, "Bob2", 8.0 };
    TEST_ASSERT(SqlRDBWrite(h, "pg_persons", &bob2, NULL) == SQL_RDB_OK);

    TEST_ASSERT(SqlRDBCount(h, "pg_persons", NULL, &n) == SQL_RDB_OK);
    TEST_ASSERT(n == 2);

    /* Read by PK */
    Person out = {0};
    SqlRDBCondition *c = SqlRDBCondInt("id", SQL_OP_EQ, 2);
    TEST_ASSERT(SqlRDBRead(h, "pg_persons", c, &out, NULL, NULL) == SQL_RDB_OK);
    SqlRDBCondFree(c);
    TEST_ASSERT(out.id == 2);
    TEST_ASSERT(strcmp(out.name, "Bob2") == 0);
    TEST_ASSERT(out.score == 8.0);

    /* NULL handling: write score as SQL NULL, read it back as NULL */
    Person carol = { 3, "Carol", 0.0 };
    uint8_t nm = 0x04;  /* bit 2 = score */
    TEST_ASSERT(SqlRDBWrite(h, "pg_persons", &carol, &nm) == SQL_RDB_OK);
    Person carol_out = { 0, "", 1.0 };
    uint8_t out_nm = 0;
    SqlRDBCondition *c3 = SqlRDBCondInt("id", SQL_OP_EQ, 3);
    TEST_ASSERT(SqlRDBRead(h, "pg_persons", c3, &carol_out, &out_nm, NULL) == SQL_RDB_OK);
    SqlRDBCondFree(c3);
    TEST_ASSERT((out_nm & 0x04) != 0);                 /* score reported as SQL NULL */
    TEST_ASSERT(strcmp(carol_out.name, "Carol") == 0); /* non-null sibling read OK */

    /* ReadMany iterator: count rows with score >= 0 (alice 9.5, bob2 8.0) */
    SqlRDBCondition *ge = SqlRDBCondReal("score", SQL_OP_GE, 0.0);
    SqlRDBStmt *it = NULL;
    TEST_ASSERT(SqlRDBReadMany(h, "pg_persons", ge, &it) == SQL_RDB_OK);
    SqlRDBCondFree(ge);
    int seen = 0;
    Person row;
    while (SqlRDBStmtNext(it, &row, NULL) == SQL_RDB_OK) seen++;
    SqlRDBStmtFree(it);
    TEST_ASSERT(seen == 2);

    /* Transaction + SAVEPOINT rollback */
    TEST_ASSERT(SqlRDBBeginTx(h) == SQL_RDB_OK);
    Person dave = { 4, "Dave", 1.0 };
    TEST_ASSERT(SqlRDBWrite(h, "pg_persons", &dave, NULL) == SQL_RDB_OK);
    TEST_ASSERT(SqlRDBBeginTx(h) == SQL_RDB_OK);          /* savepoint */
    Person eve = { 5, "Eve", 2.0 };
    TEST_ASSERT(SqlRDBWrite(h, "pg_persons", &eve, NULL) == SQL_RDB_OK);
    TEST_ASSERT(SqlRDBRollbackTx(h) == SQL_RDB_OK);       /* drop eve */
    TEST_ASSERT(SqlRDBCommitTx(h) == SQL_RDB_OK);         /* keep dave */
    SqlRDBCondition *c5 = SqlRDBCondInt("id", SQL_OP_EQ, 5);
    TEST_ASSERT(SqlRDBCount(h, "pg_persons", c5, &n) == SQL_RDB_OK);
    SqlRDBCondFree(c5);
    TEST_ASSERT(n == 0);

    /* Delete one, verify changes count */
    SqlRDBCondition *cd = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    size_t deleted = 0;
    TEST_ASSERT(SqlRDBDelete(h, "pg_persons", cd, &deleted) == SQL_RDB_OK);
    SqlRDBCondFree(cd);
    TEST_ASSERT(deleted == 1);

    /* Variable-length BLOB round-trip */
    TEST_ASSERT(SqlRDBRegisterStruct(h, "pg_docs", DOC_COLS, DOC_COL_COUNT) == SQL_RDB_OK);
    SqlRDBCondition *dall = SqlRDBCondAll();
    SqlRDBDelete(h, "pg_docs", dall, NULL);
    SqlRDBCondFree(dall);
    Doc d = { 1, { 0 } };
    TEST_ASSERT(SqlRDBWrite(h, "pg_docs", &d, NULL) == SQL_RDB_OK);

    const uint8_t payload[5] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00 };
    SqlRDBCondition *dk = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    TEST_ASSERT(SqlRDBWriteBlobField(h, "pg_docs", dk, "blob", payload, sizeof(payload)) == SQL_RDB_OK);
    SqlRDBCondFree(dk);

    SqlRDBCondition *dk2 = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    void  *buf = NULL;
    size_t len = 0;
    TEST_ASSERT(SqlRDBReadBlobField(h, "pg_docs", dk2, "blob", &buf, &len) == SQL_RDB_OK);
    SqlRDBCondFree(dk2);
    TEST_ASSERT(len == sizeof(payload));
    TEST_ASSERT(buf && memcmp(buf, payload, sizeof(payload)) == 0);
    SqlRDBFreeResult(buf);

    TEST_ASSERT(SqlRDBClose(h) == SQL_RDB_OK);
    TEST_SUMMARY();
}
