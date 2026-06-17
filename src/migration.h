/*
 * migration.h — Table sync and auto-migration for libc2sql.
 *
 * c2sql_internal_migrate_table:
 *   - Issues CREATE TABLE IF NOT EXISTS (no-op for existing tables).
 *   - Reads PRAGMA table_info / index_list / index_info and compares against
 *     the registered SqlRDBSchema (name, order, type class, NOT NULL,
 *     PRIMARY KEY, UNIQUE).
 *   - If the existing table prefix matches but trailing columns are missing
 *     and h->config.auto_migrate is true, issues ALTER TABLE ADD COLUMN for
 *     each missing column and re-validates.
 *   - Detects whether the DB-side table is STRICT; on a non-STRICT table the
 *     logger is notified, and SQL_RDB_ERR_SCHEMA_MISMATCH is returned only
 *     when h->config.require_strict is true.
 */
#ifndef C2SQL_MIGRATION_H
#define C2SQL_MIGRATION_H

#include "c2sql.h"

struct SqlRDBHandle;
struct SqlRDBSchema;

SqlRDBResult c2sql_internal_migrate_table(
    struct SqlRDBHandle       *h,
    const struct SqlRDBSchema *schema);

#endif /* C2SQL_MIGRATION_H */
