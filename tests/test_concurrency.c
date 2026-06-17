/*
 * test_concurrency.c — Task 15.3: thread safety, multi-handle, and perf smoke.
 */
#define _POSIX_C_SOURCE 200809L  /* CLOCK_MONOTONIC, pthread_*                  */

/* The original header comment continues below — duplicated header here is kept
 * minimal so the feature-test macro is the very first thing the compiler sees. */
#if 0
 *
 * Goals (per design.md):
 *   - A single handle in threadsafe mode survives N writer/reader threads
 *     without data corruption or deadlock.
 *   - N threads each opening an independent handle run without interference.
 *   - Bulk-insert and PK-lookup perf stays inside the design baselines:
 *       10,000 inserts on :memory: SQLite ≤ ~100 ms
 *       PK lookup (single Read) ≤ ~0.1 ms average
 *     The thresholds are kept generous to avoid flakes on slow CI hosts.
 */
#endif
#include "c2sql.h"
#include "harness.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int32_t id;
    char    label[16];
    int64_t value;
} Row;

static const SqlRDBColumnDef ROW_COLS[] = {
    { "id",    SQL_TYPE_INT32, offsetof(Row, id),    4,  SQL_COL_FLAG_PRIMARY_KEY },
    { "label", SQL_TYPE_TEXT,  offsetof(Row, label), 16, SQL_COL_FLAG_NONE        },
    { "value", SQL_TYPE_INT64, offsetof(Row, value), 8,  SQL_COL_FLAG_NONE        },
};
#define ROW_COL_COUNT 3

static double elapsed_ms(struct timespec a, struct timespec b) {
    return (double)(b.tv_sec - a.tv_sec) * 1000.0
         + (double)(b.tv_nsec - a.tv_nsec) / 1.0e6;
}

/* ================================================================== */
/* Same handle, multiple threads — Req 12.2 / UC-7                    */
/* ================================================================== */

#define SHARED_THREADS     4
#define SHARED_ROWS_PER_TH 250  /* total 1000 unique PKs                 */

typedef struct {
    SqlRDBHandle *h;
    int           tid;
    int           inserts_ok;
    int           reads_ok;
} SharedArg;

static void *shared_worker(void *arg) {
    SharedArg *a = arg;
    for (int i = 0; i < SHARED_ROWS_PER_TH; i++) {
        Row r;
        r.id    = a->tid * 100000 + i;
        snprintf(r.label, sizeof(r.label), "t%d-%d", a->tid, i);
        r.value = (int64_t)r.id * 7;
        if (SqlRDBWrite(a->h, "items", &r, NULL) == SQL_RDB_OK) a->inserts_ok++;
    }
    /* Read each of the rows we just inserted back. */
    for (int i = 0; i < SHARED_ROWS_PER_TH; i++) {
        int32_t          id   = a->tid * 100000 + i;
        SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, id);
        Row              out  = {0};
        if (SqlRDBRead(a->h, "items", cond, &out, NULL, NULL) == SQL_RDB_OK
            && out.id == id) a->reads_ok++;
        SqlRDBCondFree(cond);
    }
    return NULL;
}

static void test_shared_handle_threads(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(SqlRDBRegisterStruct(h, "items", ROW_COLS, ROW_COL_COUNT) == SQL_RDB_OK);

    pthread_t  threads[SHARED_THREADS];
    SharedArg  args[SHARED_THREADS] = {0};
    for (int i = 0; i < SHARED_THREADS; i++) {
        args[i].h   = h;
        args[i].tid = i + 1;
        TEST_ASSERT(pthread_create(&threads[i], NULL, shared_worker, &args[i]) == 0);
    }

    int total_inserts = 0, total_reads = 0;
    for (int i = 0; i < SHARED_THREADS; i++) {
        TEST_ASSERT(pthread_join(threads[i], NULL) == 0);
        total_inserts += args[i].inserts_ok;
        total_reads   += args[i].reads_ok;
    }

    TEST_ASSERT(total_inserts == SHARED_THREADS * SHARED_ROWS_PER_TH);
    TEST_ASSERT(total_reads   == SHARED_THREADS * SHARED_ROWS_PER_TH);

    /* Independent count via ReadMany on the shared handle */
    SqlRDBCondition *all  = SqlRDBCondAll();
    SqlRDBStmt      *iter = NULL;
    TEST_ASSERT(SqlRDBReadMany(h, "items", all, &iter) == SQL_RDB_OK);
    SqlRDBCondFree(all);
    int rows_seen = 0;
    Row probe;
    while (SqlRDBStmtNext(iter, &probe, NULL) == SQL_RDB_OK) rows_seen++;
    SqlRDBStmtFree(iter);
    TEST_ASSERT(rows_seen == SHARED_THREADS * SHARED_ROWS_PER_TH);

    SqlRDBClose(h);
}

/* ================================================================== */
/* Multiple independent handles, multiple threads — Req 12.1 / UC-6   */
/* ================================================================== */

#define INDEP_THREADS     4
#define INDEP_ROWS_PER_TH 200

typedef struct { int tid; int ok; } IndepArg;

static void *indep_worker(void *arg) {
    IndepArg *a = arg;
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    if (!h) return NULL;
    if (SqlRDBRegisterStruct(h, "items", ROW_COLS, ROW_COL_COUNT) != SQL_RDB_OK) {
        SqlRDBClose(h); return NULL;
    }

    for (int i = 0; i < INDEP_ROWS_PER_TH; i++) {
        Row r;
        r.id    = i + 1;
        snprintf(r.label, sizeof(r.label), "h%d-%d", a->tid, i);
        r.value = i;
        if (SqlRDBWrite(h, "items", &r, NULL) == SQL_RDB_OK) a->ok++;
    }
    SqlRDBClose(h);
    return NULL;
}

static void test_independent_handles_threads(void) {
    pthread_t threads[INDEP_THREADS];
    IndepArg  args[INDEP_THREADS] = {0};
    for (int i = 0; i < INDEP_THREADS; i++) {
        args[i].tid = i;
        TEST_ASSERT(pthread_create(&threads[i], NULL, indep_worker, &args[i]) == 0);
    }
    for (int i = 0; i < INDEP_THREADS; i++) {
        TEST_ASSERT(pthread_join(threads[i], NULL) == 0);
        TEST_ASSERT(args[i].ok == INDEP_ROWS_PER_TH);
    }
}

/* ================================================================== */
/* Performance smoke — Req 11.3                                        */
/* ================================================================== */

#define PERF_ROWS 10000

static void test_perf_bulk_insert(void) {
    SqlRDBHandle *h = SqlRDBInit(":memory:");
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(SqlRDBRegisterStruct(h, "items", ROW_COLS, ROW_COL_COUNT) == SQL_RDB_OK);

    /* Pre-build the batch so allocation cost is excluded from the measurement. */
    Row *batch = malloc(sizeof(Row) * PERF_ROWS);
    TEST_ASSERT(batch != NULL);
    for (int i = 0; i < PERF_ROWS; i++) {
        batch[i].id    = i + 1;
        snprintf(batch[i].label, sizeof(batch[i].label), "r%d", i);
        batch[i].value = i;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    SqlRDBResult r = SqlRDBWriteMany(h, "items", batch, PERF_ROWS, sizeof(Row), NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    free(batch);

    TEST_ASSERT(r == SQL_RDB_OK);
    double ms_insert = elapsed_ms(t0, t1);

    /* PK lookup: average over a large enough sample to be stable. */
    const int LOOKUPS = 1000;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < LOOKUPS; i++) {
        int32_t          id   = (i * 9973) % PERF_ROWS + 1;  /* prime stride to scatter */
        SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, id);
        Row              out  = {0};
        SqlRDBResult     rr   = SqlRDBRead(h, "items", cond, &out, NULL, NULL);
        SqlRDBCondFree(cond);
        TEST_ASSERT(rr == SQL_RDB_OK);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms_lookup_avg = elapsed_ms(t0, t1) / (double)LOOKUPS;

    printf("  perf: %d inserts in %.2f ms; PK lookup avg %.4f ms\n",
           PERF_ROWS, ms_insert, ms_lookup_avg);

    /* The design baselines are 100 ms / 0.1 ms. We give 10x headroom so the
     * test never flakes on shared/slow CI runners but still catches a real
     * regression (an O(N²) bug would be orders of magnitude worse). */
    TEST_ASSERT(ms_insert     < 1000.0);
    TEST_ASSERT(ms_lookup_avg <    1.0);

    SqlRDBClose(h);
}

int main(void) {
    test_shared_handle_threads();
    test_independent_handles_threads();
    test_perf_bulk_insert();
    TEST_SUMMARY();
}
