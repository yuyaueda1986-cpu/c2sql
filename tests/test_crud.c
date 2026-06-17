/*
 * test_crud.c — Integration tests for Task 10: CRUD public API.
 *
 * All tests use in-memory SQLite (":memory:") for isolation.
 * Written before implementation (TDD RED phase).
 */
#include "c2sql.h"
#include "harness.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Test struct definitions                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t id;
    char    name[32];
    double  score;
} TestPerson;

static const SqlRDBColumnDef PERSON_COLS[] = {
    { "id",    SQL_TYPE_INT32, offsetof(TestPerson, id),    4,             SQL_COL_FLAG_PRIMARY_KEY },
    { "name",  SQL_TYPE_TEXT,  offsetof(TestPerson, name),  32,            SQL_COL_FLAG_NONE        },
    { "score", SQL_TYPE_REAL,  offsetof(TestPerson, score), sizeof(double),SQL_COL_FLAG_NULLABLE    },
};
#define PERSON_COL_COUNT 3

typedef struct {
    int64_t rowid;
    char    data[64];
} TestItem;

static const SqlRDBColumnDef ITEM_COLS[] = {
    { "rowid", SQL_TYPE_INT64, offsetof(TestItem, rowid), 8,  SQL_COL_FLAG_NONE },
    { "data",  SQL_TYPE_TEXT,  offsetof(TestItem, data),  64, SQL_COL_FLAG_NONE },
};
#define ITEM_COL_COUNT 2

/* ------------------------------------------------------------------ */
/* Helper: open handle, register persons, insert 3 rows               */
/* ------------------------------------------------------------------ */

static SqlRDBHandle *setup_persons_with_data(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    if (!h) return NULL;
    if (SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT) != SQL_RDB_OK) {
        SqlRDBClose(h); return NULL;
    }
    TestPerson people[] = {
        { 1, "Alice", 9.5 },
        { 2, "Bob",   7.0 },
        { 3, "Carol", 8.5 },
    };
    for (int i = 0; i < 3; i++) {
        if (SqlRDBWrite(h, "persons", &people[i], NULL) != SQL_RDB_OK) {
            SqlRDBClose(h); return NULL;
        }
    }
    return h;
}

/* ================================================================== */
/* Task 10.1: SqlRDBRegisterStruct                                     */
/* ================================================================== */

static void test_register_null_handle(void) {
    SqlRDBResult r = SqlRDBRegisterStruct(NULL, "persons", PERSON_COLS, PERSON_COL_COUNT);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_HANDLE);
}

static void test_register_null_name(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    SqlRDBResult r = SqlRDBRegisterStruct(h, NULL, PERSON_COLS, PERSON_COL_COUNT);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    SqlRDBClose(h);
}

static void test_register_null_cols(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    SqlRDBResult r = SqlRDBRegisterStruct(h, "persons", NULL, PERSON_COL_COUNT);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    SqlRDBClose(h);
}

static void test_register_zero_col_count(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    SqlRDBResult r = SqlRDBRegisterStruct(h, "persons", PERSON_COLS, 0);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    SqlRDBClose(h);
}

static void test_register_valid(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    SqlRDBResult r = SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT);
    TEST_ASSERT(r == SQL_RDB_OK);
    SqlRDBClose(h);
}

static void test_register_two_different_structs(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT) == SQL_RDB_OK);
    TEST_ASSERT(SqlRDBRegisterStruct(h, "items",   ITEM_COLS,   ITEM_COL_COUNT)   == SQL_RDB_OK);
    SqlRDBClose(h);
}

static void test_register_duplicate(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT) == SQL_RDB_OK);
    SqlRDBResult r = SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT);
    TEST_ASSERT(r == SQL_RDB_ERR_DUPLICATE_SCHEMA);
    SqlRDBClose(h);
}

/* ================================================================== */
/* Task 10.2: SqlRDBWrite / SqlRDBWriteMany                            */
/* ================================================================== */

static void test_write_null_handle(void) {
    TestPerson p = { 1, "Alice", 9.5 };
    SqlRDBResult r = SqlRDBWrite(NULL, "persons", &p, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_HANDLE);
}

static void test_write_null_struct_name(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT) == SQL_RDB_OK);
    TestPerson p = { 1, "Alice", 9.5 };
    SqlRDBResult r = SqlRDBWrite(h, NULL, &p, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    SqlRDBClose(h);
}

static void test_write_null_row(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT) == SQL_RDB_OK);
    SqlRDBResult r = SqlRDBWrite(h, "persons", NULL, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    SqlRDBClose(h);
}

static void test_write_unknown_struct(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    TestPerson p = { 1, "Alice", 9.5 };
    SqlRDBResult r = SqlRDBWrite(h, "nonexistent", &p, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_UNKNOWN_STRUCT);
    SqlRDBClose(h);
}

static void test_write_with_pk_upsert(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT) == SQL_RDB_OK);

    TestPerson p1 = { 42, "Alice", 9.5 };
    TEST_ASSERT(SqlRDBWrite(h, "persons", &p1, NULL) == SQL_RDB_OK);

    /* Upsert same PK: should update, not duplicate */
    TestPerson p2 = { 42, "Alice Updated", 10.0 };
    TEST_ASSERT(SqlRDBWrite(h, "persons", &p2, NULL) == SQL_RDB_OK);

    /* Verify only one row with id=42 exists, with updated name */
    TestPerson out = {0};
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 42);
    TEST_ASSERT(SqlRDBRead(h, "persons", cond, &out, NULL, NULL) == SQL_RDB_OK);
    TEST_ASSERT(out.id == 42);
    TEST_ASSERT(strcmp(out.name, "Alice Updated") == 0);
    SqlRDBCondFree(cond);

    SqlRDBClose(h);
}

static void test_write_no_pk_insert(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(SqlRDBRegisterStruct(h, "items", ITEM_COLS, ITEM_COL_COUNT) == SQL_RDB_OK);

    TestItem item = { 1, "hello" };
    TEST_ASSERT(SqlRDBWrite(h, "items", &item, NULL) == SQL_RDB_OK);
    /* Duplicate insert OK (no PK conflict) */
    TEST_ASSERT(SqlRDBWrite(h, "items", &item, NULL) == SQL_RDB_OK);

    SqlRDBClose(h);
}

static void test_write_many(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(SqlRDBRegisterStruct(h, "items", ITEM_COLS, ITEM_COL_COUNT) == SQL_RDB_OK);

    TestItem items[3] = {
        { 1, "first"  },
        { 2, "second" },
        { 3, "third"  },
    };
    SqlRDBResult r = SqlRDBWriteMany(h, "items", items, 3, sizeof(TestItem), NULL);
    TEST_ASSERT(r == SQL_RDB_OK);

    /* Verify all 3 rows were written */
    SqlRDBStmt *iter = NULL;
    SqlRDBCondition *cond = SqlRDBCondAll();
    TEST_ASSERT(SqlRDBReadMany(h, "items", cond, &iter) == SQL_RDB_OK);
    SqlRDBCondFree(cond);

    int count = 0;
    TestItem out;
    while (SqlRDBStmtNext(iter, &out, NULL) == SQL_RDB_OK) count++;
    TEST_ASSERT(count == 3);
    SqlRDBStmtFree(iter);

    SqlRDBClose(h);
}

static void test_write_null_violation(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT) == SQL_RDB_OK);

    TestPerson p = { 1, "Alice", 0.0 };
    /* bit 0 = column 0 (id), which has NO SQL_COL_FLAG_NULLABLE → violation */
    uint8_t null_map = 0x01;
    SqlRDBResult r = SqlRDBWrite(h, "persons", &p, &null_map);
    TEST_ASSERT(r == SQL_RDB_ERR_NOT_NULL_VIOLATION);

    SqlRDBClose(h);
}

/* ================================================================== */
/* Task 10.3: SqlRDBRead / ReadMany / StmtNext / StmtFree             */
/* ================================================================== */

static void test_read_null_handle(void) {
    TestPerson out = {0};
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBResult r = SqlRDBRead(NULL, "persons", cond, &out, NULL, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_HANDLE);
    SqlRDBCondFree(cond);
}

static void test_read_null_out_row(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBResult r = SqlRDBRead(h, "persons", cond, NULL, NULL, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    SqlRDBCondFree(cond);
    SqlRDBClose(h);
}

static void test_read_unknown_struct(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    TestPerson out = {0};
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBResult r = SqlRDBRead(h, "no_such_struct", cond, &out, NULL, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_UNKNOWN_STRUCT);
    SqlRDBCondFree(cond);
    SqlRDBClose(h);
}

static void test_read_not_found(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);

    /* Sentinel values; must remain unchanged on NOT_FOUND */
    TestPerson out = { 99, "sentinel", -1.0 };
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 999);
    SqlRDBResult r = SqlRDBRead(h, "persons", cond, &out, NULL, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_NOT_FOUND);
    TEST_ASSERT(out.id == 99);
    TEST_ASSERT(strcmp(out.name, "sentinel") == 0);
    SqlRDBCondFree(cond);
    SqlRDBClose(h);
}

static void test_read_single_match(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);

    TestPerson out = {0};
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 2);
    SqlRDBResult r = SqlRDBRead(h, "persons", cond, &out, NULL, NULL);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(out.id == 2);
    TEST_ASSERT(strcmp(out.name, "Bob") == 0);
    SqlRDBCondFree(cond);
    SqlRDBClose(h);
}

static void test_read_multiple_rows_error(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);

    /* Sentinel values; must remain unchanged on MULTIPLE_ROWS */
    TestPerson out = { 77, "sentinel", -2.0 };
    /* score > 5 matches all 3 rows */
    SqlRDBCondition *cond = SqlRDBCondReal("score", SQL_OP_GT, 5.0);
    SqlRDBResult r = SqlRDBRead(h, "persons", cond, &out, NULL, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_MULTIPLE_ROWS);
    TEST_ASSERT(out.id == 77);
    TEST_ASSERT(strcmp(out.name, "sentinel") == 0);
    SqlRDBCondFree(cond);
    SqlRDBClose(h);
}

static void test_read_allow_multi(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);

    TestPerson out = {0};
    SqlRDBCondition *cond = SqlRDBCondReal("score", SQL_OP_GT, 5.0);
    SqlRDBReadOpts opts = { .allow_multi = true, .max_text_len = 0 };
    SqlRDBResult r = SqlRDBRead(h, "persons", cond, &out, NULL, &opts);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(out.id == 1 || out.id == 2 || out.id == 3);
    SqlRDBCondFree(cond);
    SqlRDBClose(h);
}

static void test_read_no_cond_all(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);

    TestPerson out = {0};
    /* NULL cond = match all, but since no allow_multi → MULTIPLE_ROWS */
    SqlRDBResult r = SqlRDBRead(h, "persons", NULL, &out, NULL, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_MULTIPLE_ROWS);
    SqlRDBClose(h);
}

static void test_read_many_iteration(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);

    SqlRDBCondition *cond = SqlRDBCondAll();
    SqlRDBStmt *iter = NULL;
    SqlRDBResult r = SqlRDBReadMany(h, "persons", cond, &iter);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(iter != NULL);
    SqlRDBCondFree(cond);

    int count = 0;
    TestPerson out;
    while (SqlRDBStmtNext(iter, &out, NULL) == SQL_RDB_OK) {
        count++;
        TEST_ASSERT(out.id == 1 || out.id == 2 || out.id == 3);
    }
    TEST_ASSERT(count == 3);

    SqlRDBStmtFree(iter);
    SqlRDBClose(h);
}

static void test_read_many_filtered(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);

    SqlRDBCondition *cond = SqlRDBCondReal("score", SQL_OP_GE, 8.5);
    SqlRDBStmt *iter = NULL;
    TEST_ASSERT(SqlRDBReadMany(h, "persons", cond, &iter) == SQL_RDB_OK);
    SqlRDBCondFree(cond);

    int count = 0;
    TestPerson out;
    while (SqlRDBStmtNext(iter, &out, NULL) == SQL_RDB_OK) count++;
    /* Alice (9.5) and Carol (8.5) match */
    TEST_ASSERT(count == 2);

    SqlRDBStmtFree(iter);
    SqlRDBClose(h);
}

static void test_read_many_null_out_stmt(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);
    SqlRDBCondition *cond = SqlRDBCondAll();
    SqlRDBResult r = SqlRDBReadMany(h, "persons", cond, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    SqlRDBCondFree(cond);
    SqlRDBClose(h);
}

static void test_stmt_free_null(void) {
    /* StmtFree(NULL) must not crash */
    SqlRDBResult r = SqlRDBStmtFree(NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
}

static void test_stmt_next_null(void) {
    TestPerson out = {0};
    SqlRDBResult r = SqlRDBStmtNext(NULL, &out, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
}

/* ================================================================== */
/* Task 10.4: SqlRDBDelete                                             */
/* ================================================================== */

static void test_delete_null_handle(void) {
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBResult r = SqlRDBDelete(NULL, "persons", cond, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_HANDLE);
    SqlRDBCondFree(cond);
}

static void test_delete_null_cond(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);
    /* NULL cond must be rejected to prevent accidental full-table delete */
    SqlRDBResult r = SqlRDBDelete(h, "persons", NULL, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    SqlRDBClose(h);
}

static void test_delete_null_struct_name(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBResult r = SqlRDBDelete(h, NULL, cond, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_ARG);
    SqlRDBCondFree(cond);
    SqlRDBClose(h);
}

static void test_delete_unknown_struct(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBResult r = SqlRDBDelete(h, "no_such_struct", cond, NULL);
    TEST_ASSERT(r == SQL_RDB_ERR_UNKNOWN_STRUCT);
    SqlRDBCondFree(cond);
    SqlRDBClose(h);
}

static void test_delete_with_condition(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);

    size_t deleted = 0;
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 2);
    SqlRDBResult r = SqlRDBDelete(h, "persons", cond, &deleted);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(deleted == 1);
    SqlRDBCondFree(cond);

    /* Verify person 2 is gone */
    TestPerson out = {0};
    SqlRDBCondition *check = SqlRDBCondInt("id", SQL_OP_EQ, 2);
    TEST_ASSERT(SqlRDBRead(h, "persons", check, &out, NULL, NULL) == SQL_RDB_ERR_NOT_FOUND);
    SqlRDBCondFree(check);

    /* Verify persons 1 and 3 still exist */
    SqlRDBCondition *c1 = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    TEST_ASSERT(SqlRDBRead(h, "persons", c1, &out, NULL, NULL) == SQL_RDB_OK);
    TEST_ASSERT(out.id == 1);
    SqlRDBCondFree(c1);

    SqlRDBClose(h);
}

static void test_delete_all(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);

    size_t deleted = 0;
    SqlRDBCondition *cond = SqlRDBCondAll();
    SqlRDBResult r = SqlRDBDelete(h, "persons", cond, &deleted);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(deleted == 3);
    SqlRDBCondFree(cond);

    /* Table should be empty */
    SqlRDBStmt *iter = NULL;
    SqlRDBCondition *c = SqlRDBCondAll();
    TEST_ASSERT(SqlRDBReadMany(h, "persons", c, &iter) == SQL_RDB_OK);
    SqlRDBCondFree(c);

    TestPerson out;
    int count = 0;
    while (SqlRDBStmtNext(iter, &out, NULL) == SQL_RDB_OK) count++;
    TEST_ASSERT(count == 0);
    SqlRDBStmtFree(iter);

    SqlRDBClose(h);
}

static void test_delete_zero_matches(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);

    size_t deleted = 99;  /* sentinel */
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 999);
    SqlRDBResult r = SqlRDBDelete(h, "persons", cond, &deleted);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(deleted == 0);
    SqlRDBCondFree(cond);

    SqlRDBClose(h);
}

/* ================================================================== */
/* Task 15.2: AND/OR composite-condition end-to-end                     */
/* ================================================================== */

/* AND combined condition narrows ReadMany results across the CRUD path. */
static void test_read_many_and_condition(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);

    /* score > 7.5 AND id < 3 → Alice (id=1, score=9.5) only */
    SqlRDBCondition *left  = SqlRDBCondReal("score", SQL_OP_GT, 7.5);
    SqlRDBCondition *right = SqlRDBCondInt ("id",    SQL_OP_LT, 3);
    SqlRDBCondition *cond  = SqlRDBCondAnd(left, right);

    SqlRDBStmt *iter = NULL;
    TEST_ASSERT(SqlRDBReadMany(h, "persons", cond, &iter) == SQL_RDB_OK);
    SqlRDBCondFree(cond);

    int count = 0;
    TestPerson out;
    while (SqlRDBStmtNext(iter, &out, NULL) == SQL_RDB_OK) {
        count++;
        TEST_ASSERT(out.id == 1);
    }
    TEST_ASSERT(count == 1);
    SqlRDBStmtFree(iter);

    SqlRDBClose(h);
}

/* OR combined condition surfaces multiple branches in a single SELECT. */
static void test_read_many_or_condition(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);

    /* id == 1 OR id == 3 → Alice + Carol (2 rows) */
    SqlRDBCondition *left  = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBCondition *right = SqlRDBCondInt("id", SQL_OP_EQ, 3);
    SqlRDBCondition *cond  = SqlRDBCondOr(left, right);

    SqlRDBStmt *iter = NULL;
    TEST_ASSERT(SqlRDBReadMany(h, "persons", cond, &iter) == SQL_RDB_OK);
    SqlRDBCondFree(cond);

    int seen_a = 0, seen_c = 0, others = 0;
    TestPerson out;
    while (SqlRDBStmtNext(iter, &out, NULL) == SQL_RDB_OK) {
        if      (out.id == 1) seen_a++;
        else if (out.id == 3) seen_c++;
        else                  others++;
    }
    TEST_ASSERT(seen_a == 1);
    TEST_ASSERT(seen_c == 1);
    TEST_ASSERT(others == 0);
    SqlRDBStmtFree(iter);

    SqlRDBClose(h);
}

/* Delete with an AND condition removes exactly the matching rows. */
static void test_delete_and_condition(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);

    /* score >= 8.0 AND id != 1 → Carol only (id=3, score=8.5) */
    SqlRDBCondition *left  = SqlRDBCondReal("score", SQL_OP_GE, 8.0);
    SqlRDBCondition *right = SqlRDBCondInt ("id",    SQL_OP_NE, 1);
    SqlRDBCondition *cond  = SqlRDBCondAnd(left, right);

    size_t deleted = 0;
    TEST_ASSERT(SqlRDBDelete(h, "persons", cond, &deleted) == SQL_RDB_OK);
    SqlRDBCondFree(cond);
    TEST_ASSERT(deleted == 1);

    /* Verify Alice (id=1) and Bob (id=2) remain, Carol is gone. */
    TestPerson tmp = {0};
    SqlRDBCondition *c3 = SqlRDBCondInt("id", SQL_OP_EQ, 3);
    TEST_ASSERT(SqlRDBRead(h, "persons", c3, &tmp, NULL, NULL) == SQL_RDB_ERR_NOT_FOUND);
    SqlRDBCondFree(c3);

    SqlRDBCondition *c1 = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    TEST_ASSERT(SqlRDBRead(h, "persons", c1, &tmp, NULL, NULL) == SQL_RDB_OK);
    SqlRDBCondFree(c1);

    SqlRDBClose(h);
}

static void test_delete_out_deleted_null(void) {
    SqlRDBHandle *h = setup_persons_with_data();
    TEST_ASSERT(h != NULL);

    /* out_deleted=NULL is allowed */
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBResult r = SqlRDBDelete(h, "persons", cond, NULL);
    TEST_ASSERT(r == SQL_RDB_OK);
    SqlRDBCondFree(cond);

    SqlRDBClose(h);
}

/* ================================================================== */
/* main                                                               */
/* ================================================================== */

int main(void) {
    /* 10.1 RegisterStruct */
    test_register_null_handle();
    test_register_null_name();
    test_register_null_cols();
    test_register_zero_col_count();
    test_register_valid();
    test_register_two_different_structs();
    test_register_duplicate();

    /* 10.2 Write / WriteMany */
    test_write_null_handle();
    test_write_null_struct_name();
    test_write_null_row();
    test_write_unknown_struct();
    test_write_with_pk_upsert();
    test_write_no_pk_insert();
    test_write_many();
    test_write_null_violation();

    /* 10.3 Read / ReadMany / StmtNext / StmtFree */
    test_read_null_handle();
    test_read_null_out_row();
    test_read_unknown_struct();
    test_read_not_found();
    test_read_single_match();
    test_read_multiple_rows_error();
    test_read_allow_multi();
    test_read_no_cond_all();
    test_read_many_iteration();
    test_read_many_filtered();
    test_read_many_null_out_stmt();
    test_stmt_free_null();
    test_stmt_next_null();

    /* 10.4 Delete */
    test_delete_null_handle();
    test_delete_null_cond();
    test_delete_null_struct_name();
    test_delete_unknown_struct();
    test_delete_with_condition();
    test_delete_all();
    test_delete_zero_matches();
    test_delete_out_deleted_null();

    /* 15.2 AND/OR composite-condition E2E */
    test_read_many_and_condition();
    test_read_many_or_condition();
    test_delete_and_condition();

    TEST_SUMMARY();
}
