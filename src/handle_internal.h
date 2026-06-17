/*
 * handle_internal.h — Internal definition of SqlRDBHandle.
 *
 * Only include this from c2sql.c and test code that needs direct field access.
 * The public header (c2sql.h) exposes SqlRDBHandle as an opaque forward declaration.
 *
 * Fields are added incrementally across tasks:
 *   Task 3:  magic, config, error, logger, mutex
 *   Task 4:  registry
 *   Task 9:  driver, driver_ctx, cache, txn
 */
#ifndef C2SQL_HANDLE_INTERNAL_H
#define C2SQL_HANDLE_INTERNAL_H

#include "c2sql.h"
#include "mutex.h"
#include "error_ctx.h"
#include "logger.h"
#include "schema_registry.h"
#include "stmt_cache.h"
#include "db_driver.h"
#include "txn_manager.h"

#define SQL_RDB_HANDLE_MAGIC 0xC2DB1234U
#define SQL_RDB_HANDLE_DEAD  0xDEADBEEFU

struct SqlRDBHandle {
    unsigned              magic;      /* SQL_RDB_HANDLE_MAGIC; overwritten DEAD on close */
    SqlRDBConfig          config;
    const SqlRDBDriver   *driver;     /* vtable (points to g_sqlite3_driver) */
    void                 *driver_ctx; /* driver-private context (e.g. sqlite3*) */
    SqlRDBSchemaRegistry  registry;
    SqlRDBStmtCache       cache;
    SqlRDBTxnState        txn;
    SqlRDBErrorCtx        error;
    SqlRDBLoggerCtx       logger;
    SqlRDBMutex           mutex;
};

/* Inline validity check used at every public API entry point. */
static inline bool c2sql_handle_valid(const SqlRDBHandle *h) {
    return h != NULL && h->magic == SQL_RDB_HANDLE_MAGIC;
}

#endif /* C2SQL_HANDLE_INTERNAL_H */
