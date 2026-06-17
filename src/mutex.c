/*
 * mutex.c — Mutex wrapper implementation.
 */
#include "mutex.h"

SqlRDBResult c2sql_internal_mutex_init(SqlRDBMutex *m, bool threadsafe) {
    m->threadsafe = threadsafe;
    if (!threadsafe) return SQL_RDB_OK;
#if defined(_WIN32)
    InitializeCriticalSection(&m->cs);
#else
    if (pthread_mutex_init(&m->m, NULL) != 0)
        return SQL_RDB_ERR_INTERNAL;
#endif
    return SQL_RDB_OK;
}

void c2sql_internal_mutex_lock(SqlRDBMutex *m) {
    if (!m->threadsafe) return;
#if defined(_WIN32)
    EnterCriticalSection(&m->cs);
#else
    pthread_mutex_lock(&m->m);
#endif
}

void c2sql_internal_mutex_unlock(SqlRDBMutex *m) {
    if (!m->threadsafe) return;
#if defined(_WIN32)
    LeaveCriticalSection(&m->cs);
#else
    pthread_mutex_unlock(&m->m);
#endif
}

void c2sql_internal_mutex_destroy(SqlRDBMutex *m) {
    if (!m->threadsafe) return;
#if defined(_WIN32)
    DeleteCriticalSection(&m->cs);
#else
    pthread_mutex_destroy(&m->m);
#endif
}
