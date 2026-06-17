/*
 * test_type_mapping.c — Task 8.1 / 8.2: type mapping and NULL bitmap helpers.
 *
 * Covers:
 *   - NULL bitmap bit_get / bit_set across byte boundaries
 *   - per-type bind+read roundtrip (INT32, INT64, REAL, TEXT, BLOB) via SQLite
 *   - INT32 range truncation (stored int64 outside int32 range)
 *   - TEXT truncation when stored value exceeds registered size
 *   - NULL bind through null_map (NULLABLE column)
 *   - NOT NULL violation on null_map bit for non-NULLABLE column
 *   - NULL read sets out_null_map bit and leaves struct member untouched
 *   - tm_read_row leaves the destination buffer untouched until rows are read
 */
#include "harness.h"
#include "type_mapping.h"
#include "schema_registry.h"
#include "sqlite_driver.h"
#include "db_driver.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct TR {
    int32_t  i32;
    int64_t  i64;
    double   r;
    char     t[16];
    uint8_t  b[8];
} TR;

static SqlRDBColumnDef cols_all[] = {
    {"i32", SQL_TYPE_INT32, offsetof(TR, i32), sizeof(int32_t), 0},
    {"i64", SQL_TYPE_INT64, offsetof(TR, i64), sizeof(int64_t), 0},
    {"r",   SQL_TYPE_REAL,  offsetof(TR, r),   sizeof(double),  0},
    {"t",   SQL_TYPE_TEXT,  offsetof(TR, t),   16, 0},
    {"b",   SQL_TYPE_BLOB,  offsetof(TR, b),   8,  0},
};

static const SqlRDBSchema schema_all = {
    .name      = (char *)"tr",
    .cols      = cols_all,
    .col_count = 5,
    .pk_index  = -1,
};

/* ------------------------------------------------------------------ */
/* NULL bitmap helpers                                                  */
/* ------------------------------------------------------------------ */

static void test_null_bit_get_set_basic(void) {
    uint8_t map[2] = {0};
    TEST_ASSERT(!c2sql_internal_null_bit_get(map, 0));
    TEST_ASSERT(!c2sql_internal_null_bit_get(map, 7));
    TEST_ASSERT(!c2sql_internal_null_bit_get(map, 8));
    c2sql_internal_null_bit_set(map, 0);
    c2sql_internal_null_bit_set(map, 7);
    c2sql_internal_null_bit_set(map, 9);
    TEST_ASSERT(c2sql_internal_null_bit_get(map, 0));
    TEST_ASSERT(c2sql_internal_null_bit_get(map, 7));
    TEST_ASSERT(!c2sql_internal_null_bit_get(map, 8));
    TEST_ASSERT(c2sql_internal_null_bit_get(map, 9));
    TEST_ASSERT(map[0] == 0x81);   /* bits 0 and 7 */
    TEST_ASSERT(map[1] == 0x02);   /* bit 1 of byte 1 = column 9 */
}

static void test_null_bit_get_null_map(void) {
    /* Passing NULL must read as "no nulls" so callers can omit the bitmap. */
    TEST_ASSERT(!c2sql_internal_null_bit_get(NULL, 0));
    TEST_ASSERT(!c2sql_internal_null_bit_get(NULL, 17));
    /* bit_set with NULL must be a no-op (no crash). */
    c2sql_internal_null_bit_set(NULL, 3);
}

/* ------------------------------------------------------------------ */
/* Roundtrip via SQLite driver                                          */
/* ------------------------------------------------------------------ */

static void setup_table(void **out_ctx) {
    g_sqlite3_driver.open(":memory:", out_ctx);
    g_sqlite3_driver.exec(*out_ctx,
        "CREATE TABLE tr (i32 INTEGER, i64 INTEGER, r REAL, t TEXT, b BLOB)");
}

static void test_bind_read_roundtrip_all_types(void) {
    void *ctx = NULL;
    setup_table(&ctx);

    TR in = {
        .i32 = -42,
        .i64 = (int64_t)5000000000LL,
        .r   = 3.5,
        .t   = "hello",
        .b   = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04},
    };

    void *ins = NULL;
    g_sqlite3_driver.prepare(ctx,
        "INSERT INTO tr (i32, i64, r, t, b) VALUES (?, ?, ?, ?, ?)", &ins);
    SqlRDBResult br = c2sql_internal_tm_bind_row(
        &g_sqlite3_driver, ins, &schema_all, &in, NULL);
    TEST_ASSERT(br == SQL_RDB_OK);
    bool has_row = false;
    g_sqlite3_driver.step(ins, &has_row);
    g_sqlite3_driver.finalize(ins);

    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT i32, i64, r, t, b FROM tr", &sel);
    g_sqlite3_driver.step(sel, &has_row);
    TEST_ASSERT(has_row);

    TR out;
    memset(&out, 0, sizeof(out));
    uint8_t nm = 0;
    SqlRDBResult rr = c2sql_internal_tm_read_row(
        &g_sqlite3_driver, sel, &schema_all, &out, &nm);
    TEST_ASSERT(rr == SQL_RDB_OK);
    TEST_ASSERT(out.i32 == in.i32);
    TEST_ASSERT(out.i64 == in.i64);
    TEST_ASSERT(out.r   == in.r);
    TEST_ASSERT(strcmp(out.t, in.t) == 0);
    TEST_ASSERT(memcmp(out.b, in.b, 8) == 0);
    TEST_ASSERT(nm == 0);   /* no NULL columns */

    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

/* ------------------------------------------------------------------ */
/* INT32 range truncation                                               */
/* ------------------------------------------------------------------ */

static void test_int32_read_truncation_warns(void) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    g_sqlite3_driver.exec(ctx, "CREATE TABLE tr (v INTEGER)");
    /* Store a value outside int32 range. */
    void *ins = NULL;
    g_sqlite3_driver.prepare(ctx, "INSERT INTO tr VALUES (?)", &ins);
    g_sqlite3_driver.bind_int64(ins, 1, (int64_t)5000000000LL);
    bool has_row = false;
    g_sqlite3_driver.step(ins, &has_row);
    g_sqlite3_driver.finalize(ins);

    SqlRDBColumnDef col = {"v", SQL_TYPE_INT32, 0, sizeof(int32_t), 0};
    SqlRDBSchema sc = {.name=(char*)"tr", .cols=&col, .col_count=1, .pk_index=-1};

    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT v FROM tr", &sel);
    g_sqlite3_driver.step(sel, &has_row);
    int32_t out = 0;
    SqlRDBResult r = c2sql_internal_tm_read_row(
        &g_sqlite3_driver, sel, &sc, &out, NULL);
    TEST_ASSERT(r == SQL_RDB_WARN_TRUNCATED);
    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

static void test_int32_in_range_no_warn(void) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    g_sqlite3_driver.exec(ctx, "CREATE TABLE tr (v INTEGER)");
    void *ins = NULL;
    g_sqlite3_driver.prepare(ctx, "INSERT INTO tr VALUES (?)", &ins);
    g_sqlite3_driver.bind_int64(ins, 1, -123);
    bool has_row = false;
    g_sqlite3_driver.step(ins, &has_row);
    g_sqlite3_driver.finalize(ins);

    SqlRDBColumnDef col = {"v", SQL_TYPE_INT32, 0, sizeof(int32_t), 0};
    SqlRDBSchema sc = {.name=(char*)"tr", .cols=&col, .col_count=1, .pk_index=-1};

    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT v FROM tr", &sel);
    g_sqlite3_driver.step(sel, &has_row);
    int32_t out = 0;
    SqlRDBResult r = c2sql_internal_tm_read_row(
        &g_sqlite3_driver, sel, &sc, &out, NULL);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(out == -123);
    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

/* ------------------------------------------------------------------ */
/* TEXT truncation                                                      */
/* ------------------------------------------------------------------ */

static void test_text_read_truncates_and_warns(void) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    g_sqlite3_driver.exec(ctx, "CREATE TABLE tr (v TEXT)");
    g_sqlite3_driver.exec(ctx,
        "INSERT INTO tr VALUES ('this string is definitely too long')");

    char buf[8];
    SqlRDBColumnDef col = {"v", SQL_TYPE_TEXT, 0, sizeof(buf), 0};
    SqlRDBSchema sc = {.name=(char*)"tr", .cols=&col, .col_count=1, .pk_index=-1};

    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT v FROM tr", &sel);
    bool has_row = false;
    g_sqlite3_driver.step(sel, &has_row);
    memset(buf, 0xAA, sizeof(buf));
    SqlRDBResult r = c2sql_internal_tm_read_row(
        &g_sqlite3_driver, sel, &sc, buf, NULL);
    TEST_ASSERT(r == SQL_RDB_WARN_TRUNCATED);
    TEST_ASSERT(buf[sizeof(buf) - 1] == '\0');         /* NUL-terminated */
    TEST_ASSERT(strncmp(buf, "this st", 7) == 0);      /* truncated to size-1 */
    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

static void test_text_exact_fit_no_warn(void) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    g_sqlite3_driver.exec(ctx, "CREATE TABLE tr (v TEXT)");
    g_sqlite3_driver.exec(ctx, "INSERT INTO tr VALUES ('1234567')");  /* 7 chars */

    char buf[8];
    SqlRDBColumnDef col = {"v", SQL_TYPE_TEXT, 0, sizeof(buf), 0};
    SqlRDBSchema sc = {.name=(char*)"tr", .cols=&col, .col_count=1, .pk_index=-1};

    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT v FROM tr", &sel);
    bool has_row = false;
    g_sqlite3_driver.step(sel, &has_row);
    SqlRDBResult r = c2sql_internal_tm_read_row(
        &g_sqlite3_driver, sel, &sc, buf, NULL);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(strcmp(buf, "1234567") == 0);
    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

/* ------------------------------------------------------------------ */
/* NULL bind via null_map                                               */
/* ------------------------------------------------------------------ */

static void test_null_bind_sets_db_null(void) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    g_sqlite3_driver.exec(ctx, "CREATE TABLE tr (v INTEGER)");

    SqlRDBColumnDef col = {"v", SQL_TYPE_INT32, 0, sizeof(int32_t),
                           SQL_COL_FLAG_NULLABLE};
    SqlRDBSchema sc = {.name=(char*)"tr", .cols=&col, .col_count=1, .pk_index=-1};

    int32_t in = 99;
    uint8_t nm = 0x01;   /* bit 0 = NULL */
    void *ins = NULL;
    g_sqlite3_driver.prepare(ctx, "INSERT INTO tr VALUES (?)", &ins);
    SqlRDBResult br = c2sql_internal_tm_bind_row(
        &g_sqlite3_driver, ins, &sc, &in, &nm);
    TEST_ASSERT(br == SQL_RDB_OK);
    bool has_row = false;
    g_sqlite3_driver.step(ins, &has_row);
    g_sqlite3_driver.finalize(ins);

    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT v FROM tr", &sel);
    g_sqlite3_driver.step(sel, &has_row);
    bool is_null = false;
    g_sqlite3_driver.column_isnull(sel, 0, &is_null);
    TEST_ASSERT(is_null);
    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

static void test_null_bind_not_null_violation(void) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    g_sqlite3_driver.exec(ctx, "CREATE TABLE tr (v INTEGER NOT NULL)");

    /* No NULLABLE flag => any null_map bit must trigger violation. */
    SqlRDBColumnDef col = {"v", SQL_TYPE_INT32, 0, sizeof(int32_t), 0};
    SqlRDBSchema sc = {.name=(char*)"tr", .cols=&col, .col_count=1, .pk_index=-1};

    int32_t in = 5;
    uint8_t nm = 0x01;
    void *ins = NULL;
    g_sqlite3_driver.prepare(ctx, "INSERT INTO tr VALUES (?)", &ins);
    SqlRDBResult br = c2sql_internal_tm_bind_row(
        &g_sqlite3_driver, ins, &sc, &in, &nm);
    TEST_ASSERT(br == SQL_RDB_ERR_NOT_NULL_VIOLATION);
    g_sqlite3_driver.finalize(ins);
    g_sqlite3_driver.close(ctx);
}

/* ------------------------------------------------------------------ */
/* NULL read preserves struct member                                    */
/* ------------------------------------------------------------------ */

static void test_null_read_preserves_member_and_sets_bit(void) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    g_sqlite3_driver.exec(ctx, "CREATE TABLE tr (i INTEGER, t TEXT)");
    g_sqlite3_driver.exec(ctx, "INSERT INTO tr VALUES (NULL, NULL)");

    typedef struct { int32_t i; char t[16]; } R;
    SqlRDBColumnDef cols[2] = {
        {"i", SQL_TYPE_INT32, offsetof(R, i), sizeof(int32_t), SQL_COL_FLAG_NULLABLE},
        {"t", SQL_TYPE_TEXT,  offsetof(R, t), 16,              SQL_COL_FLAG_NULLABLE},
    };
    SqlRDBSchema sc = {.name=(char*)"tr", .cols=cols, .col_count=2, .pk_index=-1};

    R out;
    out.i = 0x1234ABCD;
    strcpy(out.t, "SENTINEL");
    uint8_t nm = 0;

    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT i, t FROM tr", &sel);
    bool has_row = false;
    g_sqlite3_driver.step(sel, &has_row);
    SqlRDBResult r = c2sql_internal_tm_read_row(
        &g_sqlite3_driver, sel, &sc, &out, &nm);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(out.i == 0x1234ABCD);             /* untouched */
    TEST_ASSERT(strcmp(out.t, "SENTINEL") == 0);  /* untouched */
    TEST_ASSERT((nm & 0x03) == 0x03);             /* bits 0 and 1 set */
    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

/* ------------------------------------------------------------------ */
/* tm_read_row with no out_null_map gracefully drops the bit            */
/* ------------------------------------------------------------------ */

static void test_null_read_without_out_null_map(void) {
    void *ctx = NULL;
    g_sqlite3_driver.open(":memory:", &ctx);
    g_sqlite3_driver.exec(ctx, "CREATE TABLE tr (i INTEGER)");
    g_sqlite3_driver.exec(ctx, "INSERT INTO tr VALUES (NULL)");

    SqlRDBColumnDef col = {"i", SQL_TYPE_INT32, 0, sizeof(int32_t),
                           SQL_COL_FLAG_NULLABLE};
    SqlRDBSchema sc = {.name=(char*)"tr", .cols=&col, .col_count=1, .pk_index=-1};

    int32_t out = 0xABCD;
    void *sel = NULL;
    g_sqlite3_driver.prepare(ctx, "SELECT i FROM tr", &sel);
    bool has_row = false;
    g_sqlite3_driver.step(sel, &has_row);
    SqlRDBResult r = c2sql_internal_tm_read_row(
        &g_sqlite3_driver, sel, &sc, &out, NULL);
    TEST_ASSERT(r == SQL_RDB_OK);
    TEST_ASSERT(out == 0xABCD);   /* preserved even without out_null_map */
    g_sqlite3_driver.finalize(sel);
    g_sqlite3_driver.close(ctx);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void) {
    test_null_bit_get_set_basic();
    test_null_bit_get_null_map();
    test_bind_read_roundtrip_all_types();
    test_int32_read_truncation_warns();
    test_int32_in_range_no_warn();
    test_text_read_truncates_and_warns();
    test_text_exact_fit_no_warn();
    test_null_bind_sets_db_null();
    test_null_bind_not_null_violation();
    test_null_read_preserves_member_and_sets_bit();
    test_null_read_without_out_null_map();
    TEST_SUMMARY();
}
