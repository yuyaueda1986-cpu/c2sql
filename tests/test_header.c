/*
 * test_header.c — Task 1.2: c2sql.h の型・列挙・マクロ定義の確認
 *
 * TDD RED phase: c2sql.h が存在しないのでコンパイル失敗する
 */
#include "c2sql.h"
#include "harness.h"

/* --- コンパイル時: 列挙値の確認 --- */
_Static_assert(SQL_RDB_OK               ==  0,  "SQL_RDB_OK must be 0");
_Static_assert(SQL_RDB_WARN_TRUNCATED   ==  1,  "SQL_RDB_WARN_TRUNCATED must be 1");
_Static_assert(SQL_RDB_ERR_INVALID_ARG  == -1,  "SQL_RDB_ERR_INVALID_ARG must be -1");
_Static_assert(SQL_RDB_ERR_INVALID_HANDLE == -2,"SQL_RDB_ERR_INVALID_HANDLE must be -2");
_Static_assert(SQL_RDB_ERR_INVALID_NAME == -3,  "SQL_RDB_ERR_INVALID_NAME must be -3");
_Static_assert(SQL_RDB_ERR_DB_OPEN      == -10, "SQL_RDB_ERR_DB_OPEN must be -10");
_Static_assert(SQL_RDB_ERR_NO_MEMORY    == -11, "SQL_RDB_ERR_NO_MEMORY must be -11");
_Static_assert(SQL_RDB_ERR_DUPLICATE_SCHEMA  == -20, "");
_Static_assert(SQL_RDB_ERR_DUPLICATE_COLUMN  == -21, "");
_Static_assert(SQL_RDB_ERR_TOO_MANY_COLUMNS  == -22, "");
_Static_assert(SQL_RDB_ERR_UNKNOWN_STRUCT    == -23, "");
_Static_assert(SQL_RDB_ERR_UNKNOWN_COLUMN    == -24, "");
_Static_assert(SQL_RDB_ERR_SCHEMA_MISMATCH   == -25, "");
_Static_assert(SQL_RDB_ERR_NOT_FOUND         == -30, "");
_Static_assert(SQL_RDB_ERR_MULTIPLE_ROWS     == -31, "");
_Static_assert(SQL_RDB_ERR_NOT_NULL_VIOLATION== -32, "");
_Static_assert(SQL_RDB_ERR_NO_ACTIVE_TX      == -40, "");
_Static_assert(SQL_RDB_ERR_NESTED_TX         == -41, "");
_Static_assert(SQL_RDB_ERR_DRIVER            == -50, "");
_Static_assert(SQL_RDB_ERR_INTERNAL          == -99, "");

_Static_assert(SQL_TYPE_INT32 == 1, "SQL_TYPE_INT32 must be 1");
_Static_assert(SQL_TYPE_INT64 == 2, "SQL_TYPE_INT64 must be 2");
_Static_assert(SQL_TYPE_REAL  == 3, "SQL_TYPE_REAL must be 3");
_Static_assert(SQL_TYPE_TEXT  == 4, "SQL_TYPE_TEXT must be 4");
_Static_assert(SQL_TYPE_BLOB  == 5, "SQL_TYPE_BLOB must be 5");

_Static_assert(SQL_COL_FLAG_NONE        == 0, "SQL_COL_FLAG_NONE must be 0");
_Static_assert(SQL_COL_FLAG_PRIMARY_KEY == 1, "SQL_COL_FLAG_PRIMARY_KEY must be 1");
_Static_assert(SQL_COL_FLAG_NULLABLE    == 2, "SQL_COL_FLAG_NULLABLE must be 2");
_Static_assert(SQL_COL_FLAG_UNIQUE      == 4, "SQL_COL_FLAG_UNIQUE must be 4");

_Static_assert(SQL_OP_EQ == 1, "SQL_OP_EQ must be 1");
_Static_assert(SQL_OP_NE == 2, "SQL_OP_NE must be 2");
_Static_assert(SQL_OP_LT == 3, "SQL_OP_LT must be 3");
_Static_assert(SQL_OP_LE == 4, "SQL_OP_LE must be 4");
_Static_assert(SQL_OP_GT == 5, "SQL_OP_GT must be 5");
_Static_assert(SQL_OP_GE == 6, "SQL_OP_GE must be 6");

_Static_assert(LIBC2SQL_VERSION_MAJOR == 0, "");
_Static_assert(LIBC2SQL_VERSION_MINOR == 1, "");
_Static_assert(LIBC2SQL_VERSION_PATCH == 0, "");

/* NULLビットマップマクロの境界確認 */
_Static_assert(SQL_RDB_NULL_BITMAP_BYTES(0) == 0, "0 cols -> 0 bytes");
_Static_assert(SQL_RDB_NULL_BITMAP_BYTES(1) == 1, "1 col  -> 1 byte");
_Static_assert(SQL_RDB_NULL_BITMAP_BYTES(8) == 1, "8 cols -> 1 byte");
_Static_assert(SQL_RDB_NULL_BITMAP_BYTES(9) == 2, "9 cols -> 2 bytes");
_Static_assert(SQL_RDB_NULL_BITMAP_BYTES(16) == 2, "16 cols -> 2 bytes");
_Static_assert(SQL_RDB_NULL_BITMAP_BYTES(17) == 3, "17 cols -> 3 bytes");

int main(void) {
    /* 公開構造体をゼロ初期化できること */
    SqlRDBColumnDef col = {0};
    (void)col;

    SqlRDBReadOpts opts = {0};
    (void)opts;

    SqlRDBConfig cfg = {0};
    (void)cfg;

    /* フラグのビット演算が正しく動作すること */
    unsigned flags = SQL_COL_FLAG_PRIMARY_KEY | SQL_COL_FLAG_NULLABLE;
    TEST_ASSERT((flags & SQL_COL_FLAG_PRIMARY_KEY) != 0);
    TEST_ASSERT((flags & SQL_COL_FLAG_UNIQUE) == 0);

    /* SqlRDBColumnDef のメンバにアクセスできること */
    SqlRDBColumnDef c2 = {
        .name   = "id",
        .type   = SQL_TYPE_INT32,
        .offset = 0,
        .size   = 4,
        .flags  = SQL_COL_FLAG_PRIMARY_KEY
    };
    TEST_ASSERT(c2.type == SQL_TYPE_INT32);
    TEST_ASSERT(c2.flags == SQL_COL_FLAG_PRIMARY_KEY);

    /* SqlRDBConfig のデフォルト値確認 (ゼロ初期化) */
    TEST_ASSERT(cfg.threadsafe == false);
    TEST_ASSERT(cfg.stmt_cache_size == 0);

    TEST_SUMMARY();
}
