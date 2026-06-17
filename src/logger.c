/*
 * logger.c — Logger dispatch implementation.
 */
#include "logger.h"

void c2sql_internal_log(const SqlRDBLoggerCtx *l, SqlRDBResult code,
                        const char *sql, const char *msg) {
    if (l == NULL || l->fn == NULL) return;
    l->fn(l->user, code, sql, msg);
}
