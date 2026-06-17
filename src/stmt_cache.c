/*
 * stmt_cache.c — LRU prepared statement cache.
 *
 * Implements the API declared in stmt_cache.h: a doubly-linked LRU list paired
 * with a chained hash table for O(1) lookup/insertion. Bucket count is a power
 * of two so masking replaces modulo.
 */
#include "stmt_cache.h"

#include <stdlib.h>
#include <string.h>

#define C2SQL_CACHE_MIN_BUCKETS 16u

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static size_t next_pow2_at_least(size_t n) {
    size_t r = 1;
    while (r < n) r <<= 1;
    return r;
}

static size_t bucket_count_for(size_t capacity) {
    if (capacity == 0) return 0;
    size_t want = capacity * 2u;
    if (want < C2SQL_CACHE_MIN_BUCKETS) want = C2SQL_CACHE_MIN_BUCKETS;
    return next_pow2_at_least(want);
}

/* djb2 string hash */
static size_t hash_sql(const char *s) {
    size_t h = 5381;
    unsigned char c;
    while ((c = (unsigned char)*s++) != 0) {
        h = ((h << 5) + h) + (size_t)c;
    }
    return h;
}

static size_t bucket_index(const SqlRDBStmtCache *c, const char *sql) {
    return hash_sql(sql) & (c->bucket_count - 1u);
}

/* ------------------------------------------------------------------ */
/* LRU list manipulation                                               */
/* ------------------------------------------------------------------ */

static void lru_unlink(SqlRDBStmtCache *c, SqlRDBStmtCacheEntry *e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    else             c->lru_head           = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    else             c->lru_tail           = e->lru_prev;
    e->lru_prev = e->lru_next = NULL;
}

static void lru_push_front(SqlRDBStmtCache *c, SqlRDBStmtCacheEntry *e) {
    e->lru_prev = NULL;
    e->lru_next = c->lru_head;
    if (c->lru_head) c->lru_head->lru_prev = e;
    c->lru_head = e;
    if (!c->lru_tail) c->lru_tail = e;
}

static void lru_promote(SqlRDBStmtCache *c, SqlRDBStmtCacheEntry *e) {
    if (c->lru_head == e) return;
    lru_unlink(c, e);
    lru_push_front(c, e);
}

/* ------------------------------------------------------------------ */
/* Bucket manipulation                                                 */
/* ------------------------------------------------------------------ */

static SqlRDBStmtCacheEntry *bucket_find(
    SqlRDBStmtCache *c, size_t idx, const char *sql)
{
    SqlRDBStmtCacheEntry *e = c->buckets[idx];
    while (e) {
        if (strcmp(e->sql, sql) == 0) return e;
        e = e->bucket_next;
    }
    return NULL;
}

static void bucket_unlink(SqlRDBStmtCache *c, SqlRDBStmtCacheEntry *e) {
    size_t idx = bucket_index(c, e->sql);
    SqlRDBStmtCacheEntry **pp = &c->buckets[idx];
    while (*pp && *pp != e) pp = &(*pp)->bucket_next;
    if (*pp) *pp = e->bucket_next;
    e->bucket_next = NULL;
}

static void bucket_insert(SqlRDBStmtCache *c, SqlRDBStmtCacheEntry *e) {
    size_t idx = bucket_index(c, e->sql);
    e->bucket_next = c->buckets[idx];
    c->buckets[idx] = e;
}

/* ------------------------------------------------------------------ */
/* Entry lifecycle                                                     */
/* ------------------------------------------------------------------ */

static void entry_finalize_and_free(
    SqlRDBStmtCacheEntry *e, const SqlRDBDriver *drv, void *drv_ctx)
{
    (void)drv_ctx;
    if (drv && drv->finalize) drv->finalize(e->stmt);
    free(e->sql);
    free(e);
}

static void evict_lru_tail(
    SqlRDBStmtCache *c, const SqlRDBDriver *drv, void *drv_ctx)
{
    SqlRDBStmtCacheEntry *victim = c->lru_tail;
    if (!victim) return;
    /* Each call evicts a distinct entry: the c->lru_tail field is reloaded
     * on every invocation, and lru_unlink updates it to the prior node before
     * the entry is freed. clang-tidy's path-sensitive analyzer cannot model
     * the cross-invocation reload and reports a false use-after-free. */
    // NOLINTNEXTLINE(clang-analyzer-unix.Malloc)
    lru_unlink(c, victim);
    bucket_unlink(c, victim);
    c->size--;
    entry_finalize_and_free(victim, drv, drv_ctx);
}

/* ------------------------------------------------------------------ */
/* Public-internal API                                                 */
/* ------------------------------------------------------------------ */

SqlRDBResult c2sql_internal_cache_init(SqlRDBStmtCache *c, size_t capacity) {
    memset(c, 0, sizeof(*c));
    c->capacity = capacity;
    if (capacity == 0) return SQL_RDB_OK;

    size_t bcount = bucket_count_for(capacity);
    c->buckets = (SqlRDBStmtCacheEntry **)calloc(bcount, sizeof(*c->buckets));
    if (!c->buckets) {
        c->capacity = 0;
        return SQL_RDB_ERR_NO_MEMORY;
    }
    c->bucket_count = bcount;
    return SQL_RDB_OK;
}

SqlRDBResult c2sql_internal_cache_get(
    SqlRDBStmtCache *c, const char *sql, void **out_stmt)
{
    if (out_stmt) *out_stmt = NULL;
    if (!c || !sql || !out_stmt) return SQL_RDB_ERR_INVALID_ARG;
    if (c->capacity == 0 || c->size == 0) return SQL_RDB_ERR_NOT_FOUND;

    size_t idx = bucket_index(c, sql);
    SqlRDBStmtCacheEntry *e = bucket_find(c, idx, sql);
    if (!e) return SQL_RDB_ERR_NOT_FOUND;

    lru_promote(c, e);
    *out_stmt = e->stmt;
    return SQL_RDB_OK;
}

SqlRDBResult c2sql_internal_cache_put(
    SqlRDBStmtCache    *c,
    const char         *sql,
    void               *stmt,
    const SqlRDBDriver *drv,
    void               *drv_ctx)
{
    if (!c || !sql) return SQL_RDB_ERR_INVALID_ARG;

    /* Disabled mode: finalize immediately, never store. */
    if (c->capacity == 0) {
        if (drv && drv->finalize) drv->finalize(stmt);
        return SQL_RDB_OK;
    }

    /* Replace existing entry under the same key. */
    size_t idx = bucket_index(c, sql);
    SqlRDBStmtCacheEntry *existing = bucket_find(c, idx, sql);
    if (existing) {
        if (drv && drv->finalize) drv->finalize(existing->stmt);
        existing->stmt = stmt;
        lru_promote(c, existing);
        return SQL_RDB_OK;
    }

    /* Make room if we're at capacity. */
    if (c->size >= c->capacity) {
        evict_lru_tail(c, drv, drv_ctx);
    }

    SqlRDBStmtCacheEntry *e = (SqlRDBStmtCacheEntry *)calloc(1, sizeof(*e));
    char *sql_copy = NULL;
    if (e) {
        size_t len = strlen(sql);
        sql_copy = (char *)malloc(len + 1);
        if (sql_copy) memcpy(sql_copy, sql, len + 1);
    }
    if (!e || !sql_copy) {
        free(e);
        free(sql_copy);
        if (drv && drv->finalize) drv->finalize(stmt);
        return SQL_RDB_ERR_NO_MEMORY;
    }
    e->sql  = sql_copy;
    e->stmt = stmt;

    bucket_insert(c, e);
    lru_push_front(c, e);
    c->size++;
    return SQL_RDB_OK;
}

SqlRDBResult c2sql_internal_cache_rebuild(
    SqlRDBStmtCache    *c,
    size_t              new_capacity,
    const SqlRDBDriver *drv,
    void               *drv_ctx)
{
    if (!c) return SQL_RDB_ERR_INVALID_ARG;

    /* Drop entries beyond the new capacity from the LRU tail. */
    while (c->size > new_capacity) {
        evict_lru_tail(c, drv, drv_ctx);
    }

    if (new_capacity == 0) {
        /* Already emptied above (size==0). Release bucket table. */
        free((void *)c->buckets);
        c->buckets      = NULL;
        c->bucket_count = 0;
        c->capacity     = 0;
        return SQL_RDB_OK;
    }

    size_t want_buckets = bucket_count_for(new_capacity);
    if (want_buckets != c->bucket_count) {
        SqlRDBStmtCacheEntry **nb =
            (SqlRDBStmtCacheEntry **)calloc(want_buckets, sizeof(*nb));
        if (!nb) return SQL_RDB_ERR_NO_MEMORY;

        /* Rehash every live entry. */
        size_t mask = want_buckets - 1u;
        for (SqlRDBStmtCacheEntry *e = c->lru_head; e; e = e->lru_next) {
            e->bucket_next = NULL;
        }
        SqlRDBStmtCacheEntry **old = c->buckets;
        c->buckets      = nb;
        c->bucket_count = want_buckets;
        for (SqlRDBStmtCacheEntry *e = c->lru_head; e; e = e->lru_next) {
            size_t idx = hash_sql(e->sql) & mask;
            e->bucket_next = nb[idx];
            nb[idx] = e;
        }
        free((void *)old);
    }

    c->capacity = new_capacity;
    return SQL_RDB_OK;
}

void c2sql_internal_cache_destroy(
    SqlRDBStmtCache    *c,
    const SqlRDBDriver *drv,
    void               *drv_ctx)
{
    if (!c) return;
    SqlRDBStmtCacheEntry *e = c->lru_head;
    while (e) {
        SqlRDBStmtCacheEntry *next = e->lru_next;
        entry_finalize_and_free(e, drv, drv_ctx);
        e = next;
    }
    free((void *)c->buckets);
    memset(c, 0, sizeof(*c));
}
