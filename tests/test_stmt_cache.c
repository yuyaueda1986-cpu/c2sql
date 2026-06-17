/*
 * test_stmt_cache.c — Task 7.1: LRU statement cache unit tests
 *
 * Covers:
 *   - init/destroy on empty cache
 *   - get-miss / put-then-hit
 *   - LRU eviction on overflow (oldest evicted)
 *   - get bumps recency (least recently accessed becomes LRU)
 *   - capacity 0 = disabled mode (put immediately finalizes, get always misses)
 *   - destroy finalizes every remaining entry through the driver vtable
 *   - rebuild: shrink evicts LRU, grow keeps entries, rebuild-to-0 disables
 *   - put with existing key replaces (and finalizes) the previous statement
 *
 * Uses a fake driver whose only meaningful function is finalize. The "statement"
 * pointers are heap-allocated counters that the fake finalize frees.
 */
#include "harness.h"
#include "stmt_cache.h"
#include "db_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_finalize_count;

static SqlRDBResult fake_finalize(void *stmt) {
    g_finalize_count++;
    free(stmt);
    return SQL_RDB_OK;
}

static const SqlRDBDriver g_fake_driver = {
    .name     = "fake",
    .finalize = fake_finalize,
};

static void *make_stmt(int value) {
    int *p = (int *)malloc(sizeof(int));
    *p = value;
    return p;
}

static void reset_counters(void) {
    g_finalize_count = 0;
}

/* ------------------------------------------------------------------ */
/* Basic ops                                                           */
/* ------------------------------------------------------------------ */

static void test_init_then_destroy_no_entries(void) {
    reset_counters();
    SqlRDBStmtCache c;
    TEST_ASSERT(c2sql_internal_cache_init(&c, 4) == SQL_RDB_OK);
    c2sql_internal_cache_destroy(&c, &g_fake_driver, NULL);
    TEST_ASSERT(g_finalize_count == 0);
}

static void test_get_miss_on_empty(void) {
    reset_counters();
    SqlRDBStmtCache c;
    c2sql_internal_cache_init(&c, 4);
    void *out = (void *)0xdeadbeef;
    SqlRDBResult r = c2sql_internal_cache_get(&c, "SELECT 1", &out);
    TEST_ASSERT(r == SQL_RDB_ERR_NOT_FOUND);
    TEST_ASSERT(out == NULL);
    c2sql_internal_cache_destroy(&c, &g_fake_driver, NULL);
}

static void test_put_then_get_hit(void) {
    reset_counters();
    SqlRDBStmtCache c;
    c2sql_internal_cache_init(&c, 4);
    void *s = make_stmt(1);
    TEST_ASSERT(c2sql_internal_cache_put(&c, "INSERT", s, &g_fake_driver, NULL) == SQL_RDB_OK);
    void *out = NULL;
    TEST_ASSERT(c2sql_internal_cache_get(&c, "INSERT", &out) == SQL_RDB_OK);
    TEST_ASSERT(out == s);
    c2sql_internal_cache_destroy(&c, &g_fake_driver, NULL);
    TEST_ASSERT(g_finalize_count == 1);
}

/* ------------------------------------------------------------------ */
/* LRU semantics                                                        */
/* ------------------------------------------------------------------ */

static void test_lru_evicts_oldest(void) {
    reset_counters();
    SqlRDBStmtCache c;
    c2sql_internal_cache_init(&c, 2);
    void *a = make_stmt(1);
    void *b = make_stmt(2);
    void *d = make_stmt(3);
    c2sql_internal_cache_put(&c, "A", a, &g_fake_driver, NULL);
    c2sql_internal_cache_put(&c, "B", b, &g_fake_driver, NULL);
    c2sql_internal_cache_put(&c, "D", d, &g_fake_driver, NULL);
    TEST_ASSERT(g_finalize_count == 1);          /* A evicted */
    void *out = (void *)0xdeadbeef;
    TEST_ASSERT(c2sql_internal_cache_get(&c, "A", &out) == SQL_RDB_ERR_NOT_FOUND);
    TEST_ASSERT(out == NULL);
    TEST_ASSERT(c2sql_internal_cache_get(&c, "B", &out) == SQL_RDB_OK);
    TEST_ASSERT(out == b);
    TEST_ASSERT(c2sql_internal_cache_get(&c, "D", &out) == SQL_RDB_OK);
    TEST_ASSERT(out == d);
    c2sql_internal_cache_destroy(&c, &g_fake_driver, NULL);
    TEST_ASSERT(g_finalize_count == 3);          /* A + B + D total */
}

static void test_lru_get_updates_recency(void) {
    reset_counters();
    SqlRDBStmtCache c;
    c2sql_internal_cache_init(&c, 2);
    void *a = make_stmt(1);
    void *b = make_stmt(2);
    void *d = make_stmt(3);
    c2sql_internal_cache_put(&c, "A", a, &g_fake_driver, NULL);
    c2sql_internal_cache_put(&c, "B", b, &g_fake_driver, NULL);
    void *tmp = NULL;
    c2sql_internal_cache_get(&c, "A", &tmp);     /* A becomes MRU */
    c2sql_internal_cache_put(&c, "D", d, &g_fake_driver, NULL);  /* should evict B */
    TEST_ASSERT(g_finalize_count == 1);
    void *out = NULL;
    TEST_ASSERT(c2sql_internal_cache_get(&c, "A", &out) == SQL_RDB_OK);
    TEST_ASSERT(out == a);
    TEST_ASSERT(c2sql_internal_cache_get(&c, "B", &out) == SQL_RDB_ERR_NOT_FOUND);
    TEST_ASSERT(c2sql_internal_cache_get(&c, "D", &out) == SQL_RDB_OK);
    TEST_ASSERT(out == d);
    c2sql_internal_cache_destroy(&c, &g_fake_driver, NULL);
}

/* ------------------------------------------------------------------ */
/* Disabled mode (capacity 0)                                           */
/* ------------------------------------------------------------------ */

static void test_capacity_zero_disabled(void) {
    reset_counters();
    SqlRDBStmtCache c;
    TEST_ASSERT(c2sql_internal_cache_init(&c, 0) == SQL_RDB_OK);
    void *s = make_stmt(1);
    TEST_ASSERT(c2sql_internal_cache_put(&c, "X", s, &g_fake_driver, NULL) == SQL_RDB_OK);
    TEST_ASSERT(g_finalize_count == 1);          /* finalized immediately */
    void *out = (void *)0xdeadbeef;
    TEST_ASSERT(c2sql_internal_cache_get(&c, "X", &out) == SQL_RDB_ERR_NOT_FOUND);
    TEST_ASSERT(out == NULL);
    c2sql_internal_cache_destroy(&c, &g_fake_driver, NULL);
    TEST_ASSERT(g_finalize_count == 1);
}

/* ------------------------------------------------------------------ */
/* Destroy finalizes all remaining entries                              */
/* ------------------------------------------------------------------ */

static void test_destroy_finalizes_all(void) {
    reset_counters();
    SqlRDBStmtCache c;
    c2sql_internal_cache_init(&c, 8);
    for (int i = 0; i < 5; i++) {
        char key[8];
        snprintf(key, sizeof(key), "K%d", i);
        c2sql_internal_cache_put(&c, key, make_stmt(i), &g_fake_driver, NULL);
    }
    TEST_ASSERT(g_finalize_count == 0);
    c2sql_internal_cache_destroy(&c, &g_fake_driver, NULL);
    TEST_ASSERT(g_finalize_count == 5);
}

/* ------------------------------------------------------------------ */
/* Rebuild                                                              */
/* ------------------------------------------------------------------ */

static void test_rebuild_shrinks_evicts_lru(void) {
    reset_counters();
    SqlRDBStmtCache c;
    c2sql_internal_cache_init(&c, 4);
    c2sql_internal_cache_put(&c, "A", make_stmt(1), &g_fake_driver, NULL);
    c2sql_internal_cache_put(&c, "B", make_stmt(2), &g_fake_driver, NULL);
    c2sql_internal_cache_put(&c, "D", make_stmt(3), &g_fake_driver, NULL);
    c2sql_internal_cache_put(&c, "E", make_stmt(4), &g_fake_driver, NULL);
    TEST_ASSERT(c2sql_internal_cache_rebuild(&c, 2, &g_fake_driver, NULL) == SQL_RDB_OK);
    TEST_ASSERT(g_finalize_count == 2);          /* A, B */
    void *out = NULL;
    TEST_ASSERT(c2sql_internal_cache_get(&c, "A", &out) == SQL_RDB_ERR_NOT_FOUND);
    TEST_ASSERT(c2sql_internal_cache_get(&c, "B", &out) == SQL_RDB_ERR_NOT_FOUND);
    TEST_ASSERT(c2sql_internal_cache_get(&c, "D", &out) == SQL_RDB_OK);
    TEST_ASSERT(c2sql_internal_cache_get(&c, "E", &out) == SQL_RDB_OK);
    c2sql_internal_cache_destroy(&c, &g_fake_driver, NULL);
    TEST_ASSERT(g_finalize_count == 4);
}

static void test_rebuild_grows_keeps_entries(void) {
    reset_counters();
    SqlRDBStmtCache c;
    c2sql_internal_cache_init(&c, 2);
    c2sql_internal_cache_put(&c, "A", make_stmt(1), &g_fake_driver, NULL);
    c2sql_internal_cache_put(&c, "B", make_stmt(2), &g_fake_driver, NULL);
    TEST_ASSERT(c2sql_internal_cache_rebuild(&c, 8, &g_fake_driver, NULL) == SQL_RDB_OK);
    TEST_ASSERT(g_finalize_count == 0);
    void *out = NULL;
    TEST_ASSERT(c2sql_internal_cache_get(&c, "A", &out) == SQL_RDB_OK);
    TEST_ASSERT(c2sql_internal_cache_get(&c, "B", &out) == SQL_RDB_OK);
    c2sql_internal_cache_destroy(&c, &g_fake_driver, NULL);
    TEST_ASSERT(g_finalize_count == 2);
}

static void test_rebuild_to_zero_disables(void) {
    reset_counters();
    SqlRDBStmtCache c;
    c2sql_internal_cache_init(&c, 4);
    c2sql_internal_cache_put(&c, "A", make_stmt(1), &g_fake_driver, NULL);
    c2sql_internal_cache_put(&c, "B", make_stmt(2), &g_fake_driver, NULL);
    TEST_ASSERT(c2sql_internal_cache_rebuild(&c, 0, &g_fake_driver, NULL) == SQL_RDB_OK);
    TEST_ASSERT(g_finalize_count == 2);
    void *s = make_stmt(3);
    c2sql_internal_cache_put(&c, "X", s, &g_fake_driver, NULL);
    TEST_ASSERT(g_finalize_count == 3);          /* disabled → immediate finalize */
    void *out = (void *)0xdeadbeef;
    TEST_ASSERT(c2sql_internal_cache_get(&c, "X", &out) == SQL_RDB_ERR_NOT_FOUND);
    TEST_ASSERT(out == NULL);
    c2sql_internal_cache_destroy(&c, &g_fake_driver, NULL);
}

/* ------------------------------------------------------------------ */
/* Put-replace semantics                                                */
/* ------------------------------------------------------------------ */

static void test_put_same_key_replaces(void) {
    reset_counters();
    SqlRDBStmtCache c;
    c2sql_internal_cache_init(&c, 4);
    void *a1 = make_stmt(1);
    void *a2 = make_stmt(2);
    c2sql_internal_cache_put(&c, "A", a1, &g_fake_driver, NULL);
    c2sql_internal_cache_put(&c, "A", a2, &g_fake_driver, NULL);
    TEST_ASSERT(g_finalize_count == 1);          /* a1 finalized */
    void *out = NULL;
    TEST_ASSERT(c2sql_internal_cache_get(&c, "A", &out) == SQL_RDB_OK);
    TEST_ASSERT(out == a2);
    c2sql_internal_cache_destroy(&c, &g_fake_driver, NULL);
    TEST_ASSERT(g_finalize_count == 2);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int main(void) {
    test_init_then_destroy_no_entries();
    test_get_miss_on_empty();
    test_put_then_get_hit();
    test_lru_evicts_oldest();
    test_lru_get_updates_recency();
    test_capacity_zero_disabled();
    test_destroy_finalizes_all();
    test_rebuild_shrinks_evicts_lru();
    test_rebuild_grows_keeps_entries();
    test_rebuild_to_zero_disables();
    test_put_same_key_replaces();
    TEST_SUMMARY();
}
