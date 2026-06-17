/*
 * error_ctx.c — Error context implementation.
 */
#include "error_ctx.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void c2sql_internal_err_clear(SqlRDBErrorCtx *e) {
    e->code       = SQL_RDB_OK;
    e->message[0] = '\0';
}

void c2sql_internal_err_set(SqlRDBErrorCtx *e, SqlRDBResult code, const char *fmt, ...) {
    e->code = code;
    if (fmt == NULL) {
        e->message[0] = '\0';
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, ap);
    va_end(ap);
    /* vsnprintf always NUL-terminates on POSIX; enforce on Windows too */
    e->message[sizeof(e->message) - 1] = '\0';
}
