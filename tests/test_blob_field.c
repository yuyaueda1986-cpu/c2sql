/*
 * test_blob_field.c — Tests for Task 12: SqlRDBWriteBlobField / SqlRDBReadBlobField.
 *
 * Covers Requirements 8.5 and 5.2:
 *   - argument validation (NULL handle/struct/key/col)
 *   - unknown struct, unknown column, non-BLOB column
 *   - variable-length round-trip (write/read of sizes > schema fixed size)
 *   - row-identity semantics:
 *       0 rows match  → SQL_RDB_ERR_NOT_FOUND       (no side effect)
 *       2+ rows match → SQL_RDB_ERR_MULTIPLE_ROWS   (no side effect)
 *   - SqlRDBFreeResult releases the ReadBlobField buffer
 */
#include "c2sql.h"
#include "harness.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    int32_t id;
    char    label[16];
    uint8_t payload[8];   /* fixed-size BLOB column (variable API can overwrite with larger) */
} BlobRow;

static const SqlRDBColumnDef BLOB_COLS[] = {
    { "id",      SQL_TYPE_INT32, offsetof(BlobRow, id),      4,  SQL_COL_FLAG_PRIMARY_KEY },
    { "label",   SQL_TYPE_TEXT,  offsetof(BlobRow, label),   16, SQL_COL_FLAG_NONE        },
    { "payload", SQL_TYPE_BLOB,  offsetof(BlobRow, payload), 8,  SQL_COL_FLAG_NULLABLE    },
};
#define BLOB_COL_COUNT 3

static SqlRDBHandle *setup_two_rows(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    if (!h) return NULL;
    if (SqlRDBRegisterStruct(h, "blobs", BLOB_COLS, BLOB_COL_COUNT) != SQL_RDB_OK) {
        SqlRDBClose(h); return NULL;
    }
    BlobRow rows[] = {
        { 1, "first",  { 1,2,3,4,5,6,7,8 } },
        { 2, "second", { 8,7,6,5,4,3,2,1 } },
    };
    for (int i = 0; i < 2; i++) {
        if (SqlRDBWrite(h, "blobs", &rows[i], NULL) != SQL_RDB_OK) {
            SqlRDBClose(h); return NULL;
        }
    }
    return h;
}

/* ================================================================== */
/* Argument validation                                                 */
/* ================================================================== */

static void test_write_null_handle(void) {
    SqlRDBCondition *k = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    SqlRDBResult r = SqlRDBWriteBlobField(NULL, "blobs", k, "payload", "x", 1);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_HANDLE);
    SqlRDBCondFree(k);
}

static void test_read_null_handle(void) {
    SqlRDBCondition *k = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    void  *buf = (void *)0xCAFE;  /* sentinel - must NOT be overwritten on error */
    size_t len = 999;
    SqlRDBResult r = SqlRDBReadBlobField(NULL, "blobs", k, "payload", &buf, &len);
    TEST_ASSERT(r == SQL_RDB_ERR_INVALID_HANDLE);
    TEST_ASSERT(buf == (void *)0xCAFE);
    TEST_ASSERT(len == 999);
    SqlRDBCondFree(k);
}

static void test_write_null_args(void) {
    SqlRDBHandle *h = setup_two_rows();
    TEST_ASSERT(h != NULL);
    SqlRDBCondition *k = SqlRDBCondInt("id", SQL_OP_EQ, 1);

    TEST_ASSERT(SqlRDBWriteBlobField(h, NULL,    k,    "payload", "x", 1) == SQL_RDB_ERR_INVALID_ARG);
    TEST_ASSERT(SqlRDBWriteBlobField(h, "blobs", NULL, "payload", "x", 1) == SQL_RDB_ERR_INVALID_ARG);
    TEST_ASSERT(SqlRDBWriteBlobField(h, "blobs", k,    NULL,      "x", 1) == SQL_RDB_ERR_INVALID_ARG);
    TEST_ASSERT(SqlRDBWriteBlobField(h, "blobs", k,    "payload", NULL, 1) == SQL_RDB_ERR_INVALID_ARG);

    SqlRDBCondFree(k);
    SqlRDBClose(h);
}

static void test_read_null_args(void) {
    SqlRDBHandle *h = setup_two_rows();
    TEST_ASSERT(h != NULL);
    SqlRDBCondition *k = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    void  *buf;
    size_t len;

    TEST_ASSERT(SqlRDBReadBlobField(h, NULL,    k,    "payload", &buf, &len) == SQL_RDB_ERR_INVALID_ARG);
    TEST_ASSERT(SqlRDBReadBlobField(h, "blobs", NULL, "payload", &buf, &len) == SQL_RDB_ERR_INVALID_ARG);
    TEST_ASSERT(SqlRDBReadBlobField(h, "blobs", k,    NULL,      &buf, &len) == SQL_RDB_ERR_INVALID_ARG);
    TEST_ASSERT(SqlRDBReadBlobField(h, "blobs", k,    "payload", NULL, &len) == SQL_RDB_ERR_INVALID_ARG);
    TEST_ASSERT(SqlRDBReadBlobField(h, "blobs", k,    "payload", &buf, NULL) == SQL_RDB_ERR_INVALID_ARG);

    SqlRDBCondFree(k);
    SqlRDBClose(h);
}

static void test_unknown_struct(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    SqlRDBCondition *k = SqlRDBCondInt("id", SQL_OP_EQ, 1);

    TEST_ASSERT(SqlRDBWriteBlobField(h, "nope", k, "payload", "x", 1) == SQL_RDB_ERR_UNKNOWN_STRUCT);
    void  *buf;
    size_t len;
    TEST_ASSERT(SqlRDBReadBlobField(h, "nope", k, "payload", &buf, &len) == SQL_RDB_ERR_UNKNOWN_STRUCT);

    SqlRDBCondFree(k);
    SqlRDBClose(h);
}

static void test_unknown_column(void) {
    SqlRDBHandle *h = setup_two_rows();
    TEST_ASSERT(h != NULL);
    SqlRDBCondition *k = SqlRDBCondInt("id", SQL_OP_EQ, 1);

    TEST_ASSERT(SqlRDBWriteBlobField(h, "blobs", k, "missing", "x", 1) == SQL_RDB_ERR_UNKNOWN_COLUMN);
    void  *buf;
    size_t len;
    TEST_ASSERT(SqlRDBReadBlobField(h, "blobs", k, "missing", &buf, &len) == SQL_RDB_ERR_UNKNOWN_COLUMN);

    SqlRDBCondFree(k);
    SqlRDBClose(h);
}

static void test_non_blob_column(void) {
    SqlRDBHandle *h = setup_two_rows();
    TEST_ASSERT(h != NULL);
    SqlRDBCondition *k = SqlRDBCondInt("id", SQL_OP_EQ, 1);

    /* 'label' is TEXT, not BLOB → INVALID_ARG */
    TEST_ASSERT(SqlRDBWriteBlobField(h, "blobs", k, "label", "x", 1) == SQL_RDB_ERR_INVALID_ARG);
    void  *buf;
    size_t len;
    TEST_ASSERT(SqlRDBReadBlobField(h, "blobs", k, "label", &buf, &len) == SQL_RDB_ERR_INVALID_ARG);

    SqlRDBCondFree(k);
    SqlRDBClose(h);
}

/* ================================================================== */
/* Row-identity semantics                                              */
/* ================================================================== */

static void test_write_not_found(void) {
    SqlRDBHandle *h = setup_two_rows();
    TEST_ASSERT(h != NULL);
    SqlRDBCondition *k = SqlRDBCondInt("id", SQL_OP_EQ, 9999);

    TEST_ASSERT(SqlRDBWriteBlobField(h, "blobs", k, "payload", "x", 1) == SQL_RDB_ERR_NOT_FOUND);

    SqlRDBCondFree(k);
    SqlRDBClose(h);
}

static void test_read_not_found_preserves_outputs(void) {
    SqlRDBHandle *h = setup_two_rows();
    TEST_ASSERT(h != NULL);
    SqlRDBCondition *k = SqlRDBCondInt("id", SQL_OP_EQ, 9999);

    void  *buf = (void *)0xDEAD;
    size_t len = 42;
    TEST_ASSERT(SqlRDBReadBlobField(h, "blobs", k, "payload", &buf, &len) == SQL_RDB_ERR_NOT_FOUND);
    TEST_ASSERT(buf == (void *)0xDEAD);
    TEST_ASSERT(len == 42);

    SqlRDBCondFree(k);
    SqlRDBClose(h);
}

static void test_write_multiple_rows_no_side_effect(void) {
    SqlRDBHandle *h = setup_two_rows();
    TEST_ASSERT(h != NULL);
    SqlRDBCondition *k = SqlRDBCondInt("id", SQL_OP_GT, 0);  /* matches both rows */

    const char new_payload[16] = "0123456789ABCDEF";
    TEST_ASSERT(SqlRDBWriteBlobField(h, "blobs", k, "payload", new_payload, 16)
                == SQL_RDB_ERR_MULTIPLE_ROWS);
    SqlRDBCondFree(k);

    /* Confirm no row was modified */
    SqlRDBCondition *k1 = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    void  *buf = NULL;
    size_t len = 0;
    TEST_ASSERT(SqlRDBReadBlobField(h, "blobs", k1, "payload", &buf, &len) == SQL_RDB_OK);
    TEST_ASSERT(len == 8);
    const uint8_t expected1[8] = { 1,2,3,4,5,6,7,8 };
    TEST_ASSERT(memcmp(buf, expected1, 8) == 0);
    SqlRDBFreeResult(buf);
    SqlRDBCondFree(k1);

    SqlRDBClose(h);
}

static void test_read_multiple_rows_preserves_outputs(void) {
    SqlRDBHandle *h = setup_two_rows();
    TEST_ASSERT(h != NULL);
    SqlRDBCondition *k = SqlRDBCondInt("id", SQL_OP_GT, 0);

    void  *buf = (void *)0xBEEF;
    size_t len = 77;
    TEST_ASSERT(SqlRDBReadBlobField(h, "blobs", k, "payload", &buf, &len)
                == SQL_RDB_ERR_MULTIPLE_ROWS);
    TEST_ASSERT(buf == (void *)0xBEEF);
    TEST_ASSERT(len == 77);

    SqlRDBCondFree(k);
    SqlRDBClose(h);
}

/* ================================================================== */
/* Round-trip happy path                                               */
/* ================================================================== */

static void test_round_trip_variable_size(void) {
    SqlRDBHandle *h = setup_two_rows();
    TEST_ASSERT(h != NULL);

    /* Write a 100-byte payload to row id=1 — much larger than schema size(8) */
    uint8_t big[100];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i ^ 0x5A);

    SqlRDBCondition *k = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    TEST_ASSERT(SqlRDBWriteBlobField(h, "blobs", k, "payload", big, sizeof(big)) == SQL_RDB_OK);
    SqlRDBCondFree(k);

    /* Read back */
    SqlRDBCondition *k2 = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    void  *buf = NULL;
    size_t len = 0;
    TEST_ASSERT(SqlRDBReadBlobField(h, "blobs", k2, "payload", &buf, &len) == SQL_RDB_OK);
    TEST_ASSERT(len == sizeof(big));
    TEST_ASSERT(memcmp(buf, big, sizeof(big)) == 0);

    /* Free with the public helper — must not crash. */
    TEST_ASSERT(SqlRDBFreeResult(buf) == SQL_RDB_OK);
    SqlRDBCondFree(k2);

    /* Untouched row 2 still has its original 8-byte payload */
    SqlRDBCondition *k3 = SqlRDBCondInt("id", SQL_OP_EQ, 2);
    void  *buf3 = NULL;
    size_t len3 = 0;
    TEST_ASSERT(SqlRDBReadBlobField(h, "blobs", k3, "payload", &buf3, &len3) == SQL_RDB_OK);
    TEST_ASSERT(len3 == 8);
    const uint8_t expected2[8] = { 8,7,6,5,4,3,2,1 };
    TEST_ASSERT(memcmp(buf3, expected2, 8) == 0);
    SqlRDBFreeResult(buf3);
    SqlRDBCondFree(k3);

    SqlRDBClose(h);
}

static void test_write_then_read_zero_length(void) {
    SqlRDBHandle *h = setup_two_rows();
    TEST_ASSERT(h != NULL);

    SqlRDBCondition *k = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    /* Zero-length BLOB is legal in SQLite */
    TEST_ASSERT(SqlRDBWriteBlobField(h, "blobs", k, "payload", "", 0) == SQL_RDB_OK);
    SqlRDBCondFree(k);

    SqlRDBCondition *k2 = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    void  *buf = (void *)0xAA;
    size_t len = 99;
    TEST_ASSERT(SqlRDBReadBlobField(h, "blobs", k2, "payload", &buf, &len) == SQL_RDB_OK);
    TEST_ASSERT(len == 0);
    /* buf is allowed to be NULL or a 0-sized allocation; SqlRDBFreeResult must accept it */
    SqlRDBFreeResult(buf);
    SqlRDBCondFree(k2);

    SqlRDBClose(h);
}

int main(void) {
    test_write_null_handle();
    test_read_null_handle();
    test_write_null_args();
    test_read_null_args();
    test_unknown_struct();
    test_unknown_column();
    test_non_blob_column();

    test_write_not_found();
    test_read_not_found_preserves_outputs();
    test_write_multiple_rows_no_side_effect();
    test_read_multiple_rows_preserves_outputs();

    test_round_trip_variable_size();
    test_write_then_read_zero_length();

    TEST_SUMMARY();
}
