/*
 * mutex.h — Internal POSIX/Windows mutex abstraction for libc2sql.
 *
 * When threadsafe=false, all operations are no-ops.
 * When threadsafe=true, uses pthread_mutex_t on POSIX or CRITICAL_SECTION on Windows.
 */
#ifndef C2SQL_MUTEX_H
#define C2SQL_MUTEX_H

#include <stdbool.h>
#include "c2sql.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <pthread.h>
#endif

typedef struct SqlRDBMutex {
    bool threadsafe;
#if defined(_WIN32)
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t m;
#endif
} SqlRDBMutex;

SqlRDBResult c2sql_internal_mutex_init   (SqlRDBMutex *m, bool threadsafe);
void         c2sql_internal_mutex_lock   (SqlRDBMutex *m);
void         c2sql_internal_mutex_unlock (SqlRDBMutex *m);
void         c2sql_internal_mutex_destroy(SqlRDBMutex *m);

#endif /* C2SQL_MUTEX_H */
