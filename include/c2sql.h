/**
 * c2sql.h — Public API for libc2sql
 *
 * C11 struct<->RDB mapping library.
 * Include this header and link with -lc2sql -lsqlite3.
 *
 * All functions return SqlRDBResult. On error, call SqlRDBLastError()
 * for a human-readable message.
 */
#ifndef C2SQL_H
#define C2SQL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Version                                                             */
/* ------------------------------------------------------------------ */
#define LIBC2SQL_VERSION_MAJOR 0
#define LIBC2SQL_VERSION_MINOR 1
#define LIBC2SQL_VERSION_PATCH 0

/* ------------------------------------------------------------------ */
/* Opaque types                                                        */
/* ------------------------------------------------------------------ */
typedef struct SqlRDBHandle    SqlRDBHandle;   /**< DB connection handle */
typedef struct SqlRDBCondition SqlRDBCondition;/**< Search condition AST  */
typedef struct SqlRDBStmt      SqlRDBStmt;     /**< Multi-row iterator    */

/* ------------------------------------------------------------------ */
/* Result codes                                                        */
/* ------------------------------------------------------------------ */
typedef enum SqlRDBResult {
    SQL_RDB_OK                   =  0,
    SQL_RDB_WARN_TRUNCATED       =  1,  /**< Value silently truncated     */
    SQL_RDB_ERR_INVALID_ARG      = -1,
    SQL_RDB_ERR_INVALID_HANDLE   = -2,
    SQL_RDB_ERR_INVALID_NAME     = -3,
    SQL_RDB_ERR_DB_OPEN          = -10,
    SQL_RDB_ERR_NO_MEMORY        = -11,
    SQL_RDB_ERR_DUPLICATE_SCHEMA = -20,
    SQL_RDB_ERR_DUPLICATE_COLUMN = -21,
    SQL_RDB_ERR_TOO_MANY_COLUMNS = -22,
    SQL_RDB_ERR_UNKNOWN_STRUCT   = -23,
    SQL_RDB_ERR_UNKNOWN_COLUMN   = -24,
    SQL_RDB_ERR_SCHEMA_MISMATCH  = -25,
    SQL_RDB_ERR_NOT_FOUND        = -30,
    SQL_RDB_ERR_MULTIPLE_ROWS    = -31,
    SQL_RDB_ERR_NOT_NULL_VIOLATION = -32,
    SQL_RDB_ERR_CAPACITY_EXCEEDED  = -33,  /**< max_records guard rejected an insert */
    SQL_RDB_ERR_NO_ACTIVE_TX     = -40,
    SQL_RDB_ERR_NESTED_TX        = -41,
    SQL_RDB_ERR_DRIVER           = -50,
    SQL_RDB_ERR_INTERNAL         = -99
} SqlRDBResult;

/* ------------------------------------------------------------------ */
/* SQL types                                                           */
/* ------------------------------------------------------------------ */
typedef enum SqlRDBType {
    SQL_TYPE_INT32 = 1,
    SQL_TYPE_INT64 = 2,
    SQL_TYPE_REAL  = 3,
    SQL_TYPE_TEXT  = 4,
    SQL_TYPE_BLOB  = 5
} SqlRDBType;

/* ------------------------------------------------------------------ */
/* Column flags (bitmask)                                              */
/* ------------------------------------------------------------------ */
typedef enum SqlRDBColFlag {
    SQL_COL_FLAG_NONE        = 0,
    SQL_COL_FLAG_PRIMARY_KEY = 1u << 0,
    SQL_COL_FLAG_NULLABLE    = 1u << 1,
    SQL_COL_FLAG_UNIQUE      = 1u << 2
} SqlRDBColFlag;

/* ------------------------------------------------------------------ */
/* Comparison operators                                                */
/* ------------------------------------------------------------------ */
typedef enum SqlRDBOp {
    SQL_OP_EQ = 1,
    SQL_OP_NE = 2,
    SQL_OP_LT = 3,
    SQL_OP_LE = 4,
    SQL_OP_GT = 5,
    SQL_OP_GE = 6
} SqlRDBOp;

/* ------------------------------------------------------------------ */
/* Column definition                                                   */
/* ------------------------------------------------------------------ */
typedef struct SqlRDBColumnDef {
    const char *name;   /**< Column name (caller-owned, must outlive handle) */
    SqlRDBType  type;   /**< SQL type */
    size_t      offset; /**< offsetof(StructType, member)                    */
    size_t      size;   /**< sizeof(member). BLOB: fixed-length in bulk CRUD */
    unsigned    flags;  /**< Bitwise OR of SqlRDBColFlag                     */
} SqlRDBColumnDef;

/*
 * NULL bitmap layout:
 *   Length = SQL_RDB_NULL_BITMAP_BYTES(col_count) bytes.
 *   Bit N (0-based, LSB side of byte N/8) = 1 means column N is NULL.
 *
 * Write path:
 *   bit=1  -> bind SQL NULL regardless of struct contents.
 *   bit=1 on a non-NULLABLE column -> SQL_RDB_ERR_NOT_NULL_VIOLATION.
 *   Passing NULL as null_map treats all columns as non-NULL.
 *
 * Read path:
 *   When column_isnull() is true, set corresponding bit in out_null_map
 *   and leave the struct member unchanged.
 *   Passing NULL as out_null_map silently ignores NULL columns.
 *
 * Output buffer invariant (SqlRDBRead / SqlRDBStmtNext / SqlRDBReadBlobField):
 *   SQL_RDB_ERR_NOT_FOUND / SQL_RDB_ERR_MULTIPLE_ROWS:
 *     out_row, out_null_map, *out_bytes, *out_len are NOT modified (Req 5.2, 5.3).
 *   Other errors: output buffer contents are undefined; do not use them.
 */
#define SQL_RDB_NULL_BITMAP_BYTES(col_count) (((col_count) + 7u) / 8u)

/* ------------------------------------------------------------------ */
/* Read options                                                        */
/* ------------------------------------------------------------------ */
typedef struct SqlRDBReadOpts {
    bool   allow_multi;  /**< true: return first row when multiple match */
    size_t max_text_len; /**< 0 = use registered size; otherwise limit   */
} SqlRDBReadOpts;

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */
typedef struct SqlRDBConfig {
    bool   threadsafe;      /**< Default: true. Serialize API via handle mutex */
    size_t stmt_cache_size; /**< Default: 64. 0 = disable cache               */
    bool   auto_migrate;    /**< Default: false. ALTER TABLE on schema delta   */
    int    multirow_default;/**< 0=error (default), 1=return first row         */
    bool   require_strict;  /**< Default: false. Reject non-STRICT tables      */
} SqlRDBConfig;

/* ------------------------------------------------------------------ */
/* Logger                                                              */
/* ------------------------------------------------------------------ */
/**
 * Log callback type.
 * @param user  User-supplied pointer passed to SqlRDBSetLogger.
 * @param code  Result code of the operation.
 * @param sql   The SQL statement that was executed (may be NULL).
 * @param msg   Human-readable message (may be NULL).
 *
 * The callback must return quickly; it is invoked while the handle
 * mutex is held in threadsafe mode.
 */
typedef void (*SqlRDBLoggerFn)(void *user, SqlRDBResult code,
                               const char *sql, const char *msg);

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */
/**
 * Open a database connection.
 * @param connection_string  Path to SQLite3 file or ":memory:".
 * @return  New handle, or NULL on failure (no SqlRDBHandle exists to
 *          store error details; check errno or rebuild with logs).
 */
SqlRDBHandle *SqlRDBInit (const char *connection_string);

/**
 * Close a database connection and free all resources.
 * Release order: active TX -> statement cache -> schema registry
 *                -> driver -> mutex -> free.
 * Double-close returns SQL_RDB_ERR_INVALID_HANDLE.
 */
SqlRDBResult  SqlRDBClose(SqlRDBHandle *handle);

/* ------------------------------------------------------------------ */
/* Schema registration                                                 */
/* ------------------------------------------------------------------ */
/**
 * Register a C struct as a DB table schema.
 * Creates the table (CREATE TABLE IF NOT EXISTS) if it does not exist.
 * On subsequent calls with the same struct_name, validates the existing
 * table against the new definition.
 *
 * @param handle       Valid handle.
 * @param struct_name  Table/struct name (ASCII alphanumeric + underscore).
 * @param cols         Array of column definitions (deep-copied).
 * @param col_count    Length of cols[].
 */
SqlRDBResult SqlRDBRegisterStruct(
    SqlRDBHandle          *handle,
    const char            *struct_name,
    const SqlRDBColumnDef *cols,
    size_t                 col_count);

/* ------------------------------------------------------------------ */
/* CRUD                                                                */
/* ------------------------------------------------------------------ */
/**
 * Write one row. UPSERT when schema has a primary key; INSERT otherwise.
 * @param null_map  NULL bitmap (SQL_RDB_NULL_BITMAP_BYTES(col_count) bytes)
 *                  or NULL to treat all columns as non-NULL.
 */
SqlRDBResult SqlRDBWrite(
    SqlRDBHandle  *h,
    const char    *struct_name,
    const void    *row,
    const uint8_t *null_map);

/**
 * Write multiple rows in a single transaction.
 * @param rows      Pointer to the first row.
 * @param count     Number of rows.
 * @param stride    Byte distance between consecutive rows (e.g. sizeof(T)).
 * @param null_maps Concatenated null bitmaps (count * NULL_BITMAP_BYTES each),
 *                  or NULL to treat all columns in all rows as non-NULL.
 */
SqlRDBResult SqlRDBWriteMany(
    SqlRDBHandle  *h,
    const char    *struct_name,
    const void    *rows,
    size_t         count,
    size_t         stride,
    const uint8_t *null_maps);

/**
 * Read a single row matching cond.
 * Returns SQL_RDB_ERR_MULTIPLE_ROWS if more than one row matches and
 * opts->allow_multi is false (the default).
 * Returns SQL_RDB_ERR_NOT_FOUND if no row matches.
 * On error, out_row and out_null_map are NOT modified.
 */
SqlRDBResult SqlRDBRead(
    SqlRDBHandle          *h,
    const char            *struct_name,
    const SqlRDBCondition *cond,
    void                  *out_row,
    uint8_t               *out_null_map,
    const SqlRDBReadOpts  *opts);

/**
 * Begin a multi-row iteration. Release with SqlRDBStmtFree().
 * @param out_stmt  Receives the iterator handle.
 */
SqlRDBResult SqlRDBReadMany(
    SqlRDBHandle          *h,
    const char            *struct_name,
    const SqlRDBCondition *cond,
    SqlRDBStmt           **out_stmt);

/** Fetch the next row from an iterator. Returns SQL_RDB_ERR_NOT_FOUND at end. */
SqlRDBResult SqlRDBStmtNext(SqlRDBStmt *stmt, void *out_row, uint8_t *out_null_map);

/** Finalize the iterator and free all associated resources. */
SqlRDBResult SqlRDBStmtFree(SqlRDBStmt *stmt);

/**
 * Delete rows matching cond.
 * cond must not be NULL; pass SqlRDBCondAll() to delete all rows.
 * @param out_deleted  Receives the number of deleted rows (may be NULL).
 */
SqlRDBResult SqlRDBDelete(
    SqlRDBHandle          *h,
    const char            *struct_name,
    const SqlRDBCondition *cond,
    size_t                *out_deleted);

/**
 * Count the rows matching cond (read-only; never modifies the table).
 * Unlike SqlRDBDelete, a NULL cond is allowed and means "count all rows"
 * (equivalent to SqlRDBCondAll()), since counting cannot be destructive.
 * @param out_count  Receives the number of matching rows (must be non-NULL).
 */
SqlRDBResult SqlRDBCount(
    SqlRDBHandle          *h,
    const char            *struct_name,
    const SqlRDBCondition *cond,
    size_t                *out_count);

/* ------------------------------------------------------------------ */
/* BLOB field auxiliary API (Req 8.5)                                  */
/* ------------------------------------------------------------------ */
/*
 * Use these APIs for variable-length BLOBs. Bulk CRUD treats
 * SqlRDBColumnDef.size as a fixed-length BLOB.
 *
 * key must uniquely identify one row; 0-match -> NOT_FOUND,
 * 2+-match -> MULTIPLE_ROWS (write is NOT performed in that case).
 */

/** Write bytes to a single BLOB column of the matching row. */
SqlRDBResult SqlRDBWriteBlobField(
    SqlRDBHandle          *h,
    const char            *struct_name,
    const SqlRDBCondition *key,
    const char            *col_name,
    const void            *bytes,
    size_t                 len);

/**
 * Read a BLOB column from the matching row into a library-allocated buffer.
 * Free with SqlRDBFreeResult(*out_bytes).
 * On error, *out_bytes and *out_len are NOT modified.
 */
SqlRDBResult SqlRDBReadBlobField(
    SqlRDBHandle          *h,
    const char            *struct_name,
    const SqlRDBCondition *key,
    const char            *col_name,
    void                 **out_bytes,
    size_t                *out_len);

/* ------------------------------------------------------------------ */
/* Condition AST builder                                               */
/* ------------------------------------------------------------------ */
/*
 * String / pointer values passed to these constructors are NOT copied.
 * The caller must keep them alive until SqlRDBCondFree() is called.
 */

/** col op value (integer) */
SqlRDBCondition *SqlRDBCondInt (const char *col, int op, int64_t value);
/** col op value (text, NUL-terminated) */
SqlRDBCondition *SqlRDBCondText(const char *col, int op, const char *value);
/** col op value (real) */
SqlRDBCondition *SqlRDBCondReal(const char *col, int op, double value);
/** col op value (blob) */
SqlRDBCondition *SqlRDBCondBlob(const char *col, int op, const void *bytes, size_t len);
/** Logical AND of two conditions (takes ownership of a and b). */
SqlRDBCondition *SqlRDBCondAnd (SqlRDBCondition *a, SqlRDBCondition *b);
/** Logical OR of two conditions (takes ownership of a and b). */
SqlRDBCondition *SqlRDBCondOr  (SqlRDBCondition *a, SqlRDBCondition *b);
/** Match-all sentinel; required to authorize a full-table DELETE. */
SqlRDBCondition *SqlRDBCondAll (void);
/** Recursively free all nodes in the condition tree. */
void              SqlRDBCondFree(SqlRDBCondition *cond);

/* ------------------------------------------------------------------ */
/* Transactions                                                        */
/* ------------------------------------------------------------------ */
/**
 * Begin a transaction (or a SAVEPOINT if already inside one).
 * Nesting depth limit: 16.
 */
SqlRDBResult SqlRDBBeginTx   (SqlRDBHandle *h);
/** Commit the innermost transaction / release the innermost SAVEPOINT. */
SqlRDBResult SqlRDBCommitTx  (SqlRDBHandle *h);
/** Roll back the innermost transaction / roll back to the innermost SAVEPOINT. */
SqlRDBResult SqlRDBRollbackTx(SqlRDBHandle *h);

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */
/** Register a log callback. Pass fn=NULL to disable logging. */
SqlRDBResult  SqlRDBSetLogger (SqlRDBHandle *h, SqlRDBLoggerFn fn, void *user);

/**
 * Return a pointer to the last error message stored in the handle.
 * The pointer is valid until the next API call on the same handle.
 * @param out_code  If non-NULL, receives the last error code.
 * @return  NUL-terminated message string, or "" if no error.
 */
const char   *SqlRDBLastError (const SqlRDBHandle *h, SqlRDBResult *out_code);

/* ------------------------------------------------------------------ */
/* Configuration & memory                                              */
/* ------------------------------------------------------------------ */
/** Apply configuration changes. Safe to call after SqlRDBInit(). */
SqlRDBResult  SqlRDBSetConfig (SqlRDBHandle *h, const SqlRDBConfig *cfg);

/**
 * Free a buffer returned by SqlRDBReadBlobField().
 * Passing NULL is a no-op (like free(NULL)).
 */
SqlRDBResult  SqlRDBFreeResult(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* C2SQL_H */
