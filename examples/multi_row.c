/*
 * multi_row.c — libc2sql sample: multi-row iteration and variable-length BLOB.
 *
 * Demonstrates:
 *   - SqlRDBWriteMany for batched inserts inside an implicit transaction
 *   - SqlRDBReadMany + SqlRDBStmtNext + SqlRDBStmtFree iteration pattern
 *   - SqlRDBWriteBlobField / SqlRDBReadBlobField for variable-length payloads
 *     (sizes larger than the schema fixed size are supported)
 *   - SqlRDBFreeResult to release a buffer returned by ReadBlobField
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
    char    label[16];
    uint8_t payload[8];     /* fixed-size BLOB slot for bulk CRUD */
} Doc;

static const SqlRDBColumnDef DOC_COLS[] = {
    { "id",      SQL_TYPE_INT32, offsetof(Doc, id),      4,  SQL_COL_FLAG_PRIMARY_KEY },
    { "label",   SQL_TYPE_TEXT,  offsetof(Doc, label),   16, SQL_COL_FLAG_NONE        },
    { "payload", SQL_TYPE_BLOB,  offsetof(Doc, payload), 8,  SQL_COL_FLAG_NULLABLE    },
};
#define DOC_COL_COUNT (sizeof(DOC_COLS) / sizeof(DOC_COLS[0]))

int main(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    assert(h != NULL);
    assert(SqlRDBRegisterStruct(h, "docs", DOC_COLS, DOC_COL_COUNT) == SQL_RDB_OK);

    /* Batched insert: 4 rows in a single implicit transaction. */
    Doc rows[] = {
        { 1, "alpha",   { 0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08 } },
        { 2, "beta",    { 0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18 } },
        { 3, "gamma",   { 0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28 } },
        { 4, "delta",   { 0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38 } },
    };
    assert(SqlRDBWriteMany(h, "docs", rows, 4, sizeof(Doc), NULL) == SQL_RDB_OK);

    /* Multi-row iteration with SqlRDBReadMany → SqlRDBStmtNext loop. */
    SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_GE, 2);
    SqlRDBStmt      *iter = NULL;
    assert(SqlRDBReadMany(h, "docs", cond, &iter) == SQL_RDB_OK);
    SqlRDBCondFree(cond);

    int seen = 0;
    Doc  row;
    while (SqlRDBStmtNext(iter, &row, NULL) == SQL_RDB_OK) {
        printf("  doc id=%d label=%s\n", row.id, row.label);
        seen++;
    }
    assert(seen == 3);
    assert(SqlRDBStmtFree(iter) == SQL_RDB_OK);

    /* Variable-length BLOB: write a 64-byte payload (8x larger than the
     * schema's fixed slot) and read it back. */
    uint8_t big[64];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 7);

    SqlRDBCondition *target = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    assert(SqlRDBWriteBlobField(h, "docs", target, "payload", big, sizeof(big)) == SQL_RDB_OK);
    SqlRDBCondFree(target);

    SqlRDBCondition *target2 = SqlRDBCondInt("id", SQL_OP_EQ, 1);
    void  *buf = NULL;
    size_t len = 0;
    assert(SqlRDBReadBlobField(h, "docs", target2, "payload", &buf, &len) == SQL_RDB_OK);
    SqlRDBCondFree(target2);

    assert(len == sizeof(big));
    assert(memcmp(buf, big, sizeof(big)) == 0);
    printf("  blob round-trip: %zu bytes\n", len);
    SqlRDBFreeResult(buf);

    /* Multiple-match writes are rejected to prevent silent fan-out. */
    SqlRDBCondition *broad = SqlRDBCondInt("id", SQL_OP_GT, 0);
    SqlRDBResult err = SqlRDBWriteBlobField(h, "docs", broad, "payload", "x", 1);
    SqlRDBCondFree(broad);
    assert(err == SQL_RDB_ERR_MULTIPLE_ROWS);

    assert(SqlRDBClose(h) == SQL_RDB_OK);
    printf("multi_row: ok\n");
    return 0;
}
