/*
 * Minimal test harness for libc2sql.
 * Include once per translation unit. Terminate main() with TEST_SUMMARY().
 */
#ifndef C2SQL_TEST_HARNESS_H
#define C2SQL_TEST_HARNESS_H

#include <stdio.h>

static int _h_pass = 0;
static int _h_fail = 0;

#define TEST_ASSERT(expr) do {                                          \
    if (expr) {                                                         \
        _h_pass++;                                                      \
    } else {                                                            \
        fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #expr);\
        _h_fail++;                                                      \
    }                                                                   \
} while (0)

#define TEST_SUMMARY() do {                                             \
    printf("Results: %d passed, %d failed\n", _h_pass, _h_fail);       \
    return (_h_fail > 0) ? 1 : 0;                                       \
} while (0)

#endif /* C2SQL_TEST_HARNESS_H */
