/*
 * stmt_cache.h — Internal LRU statement cache for libc2sql.
 *
 * Caches prepared statements keyed by their SQL string. Operations are O(1)
 * via a doubly-linked LRU list combined with a chained hash table.
 *
 * Ownership model:
 *   - On successful c2sql_internal_cache_put, the cache takes ownership of
 *     the supplied driver statement and finalizes it on eviction/destroy.
 *   - On c2sql_internal_cache_get, the entry stays in the cache; the returned
 *     pointer is valid until the entry is evicted or the cache is destroyed.
 *
 * Disabled mode:
 *   - If capacity == 0 the cache is in disabled mode. put immediately calls
 *     driver->finalize on the supplied statement and returns OK; get always
 *     returns SQL_RDB_ERR_NOT_FOUND. This implements the "every call
 *     prepare/finalize" mode from the design.
 *
 * Concurrency:
 *   - No internal locking; the owning SqlRDBHandle's mutex serializes access.
 */
#ifndef C2SQL_STMT_CACHE_H
#define C2SQL_STMT_CACHE_H

#include <stddef.h>
#include "c2sql.h"
#include "db_driver.h"

typedef struct SqlRDBStmtCacheEntry {
    char                        *sql;          /* strdup'd key, NUL-terminated */
    void                        *stmt;         /* driver-prepared statement */
    struct SqlRDBStmtCacheEntry *lru_prev;     /* toward MRU head */
    struct SqlRDBStmtCacheEntry *lru_next;     /* toward LRU tail */
    struct SqlRDBStmtCacheEntry *bucket_next;  /* chain within hash bucket */
} SqlRDBStmtCacheEntry;

typedef struct SqlRDBStmtCache {
    size_t                  capacity;      /* 0 = disabled */
    size_t                  size;          /* live entries */
    SqlRDBStmtCacheEntry   *lru_head;      /* most recently used */
    SqlRDBStmtCacheEntry   *lru_tail;      /* least recently used */
    SqlRDBStmtCacheEntry  **buckets;       /* chained hash table */
    size_t                  bucket_count;  /* power of two, or 0 when disabled */
} SqlRDBStmtCache;

/*
 * Initialize an empty cache with the given capacity. capacity == 0 starts the
 * cache in disabled mode (no entries are ever stored).
 *
 * Returns SQL_RDB_OK on success or SQL_RDB_ERR_NO_MEMORY if bucket allocation
 * fails. On failure the cache is left zero-initialized and safe to destroy.
 */
SqlRDBResult c2sql_internal_cache_init(SqlRDBStmtCache *c, size_t capacity);

/*
 * Look up sql in the cache.
 *   On hit:  *out_stmt is set to the cached statement, the entry is promoted
 *            to the MRU position, and SQL_RDB_OK is returned.
 *   On miss: *out_stmt is set to NULL and SQL_RDB_ERR_NOT_FOUND is returned.
 *
 * out_stmt must be non-NULL. sql must be a NUL-terminated string.
 */
SqlRDBResult c2sql_internal_cache_get(SqlRDBStmtCache *c, const char *sql, void **out_stmt);

/*
 * Store stmt under sql.
 *   - If sql already exists, the old statement is finalized via drv->finalize
 *     and replaced; the entry is moved to the MRU position.
 *   - If the cache is at capacity, the LRU entry is finalized and removed
 *     before insertion.
 *   - If capacity == 0 (disabled), stmt is finalized immediately and OK is
 *     returned.
 *
 * Returns SQL_RDB_OK on success or SQL_RDB_ERR_NO_MEMORY if allocation fails;
 * on memory failure the supplied stmt is finalized to avoid leaking it.
 */
SqlRDBResult c2sql_internal_cache_put(
    SqlRDBStmtCache    *c,
    const char         *sql,
    void               *stmt,
    const SqlRDBDriver *drv,
    void               *drv_ctx);

/*
 * Resize the cache to new_capacity. Excess entries are evicted from the LRU
 * tail and finalized via drv->finalize. new_capacity == 0 puts the cache into
 * disabled mode and finalizes every remaining entry.
 *
 * Returns SQL_RDB_OK on success or SQL_RDB_ERR_NO_MEMORY if the new bucket
 * table cannot be allocated; on failure the original cache state is preserved.
 */
SqlRDBResult c2sql_internal_cache_rebuild(
    SqlRDBStmtCache    *c,
    size_t              new_capacity,
    const SqlRDBDriver *drv,
    void               *drv_ctx);

/*
 * Finalize every remaining statement via drv->finalize, free all internal
 * allocations, and zero out the cache. Safe to call on a zero-initialized
 * cache and idempotent against itself.
 */
void c2sql_internal_cache_destroy(
    SqlRDBStmtCache    *c,
    const SqlRDBDriver *drv,
    void               *drv_ctx);

#endif /* C2SQL_STMT_CACHE_H */
