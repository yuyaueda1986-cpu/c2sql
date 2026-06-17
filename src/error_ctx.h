/*
 * error_ctx.h — Per-handle error code and message storage.
 */
#ifndef C2SQL_ERROR_CTX_H
#define C2SQL_ERROR_CTX_H

#include "c2sql.h"

typedef struct SqlRDBErrorCtx {
    SqlRDBResult code;
    char         message[256];
} SqlRDBErrorCtx;

void c2sql_internal_err_clear(SqlRDBErrorCtx *e);
void c2sql_internal_err_set  (SqlRDBErrorCtx *e, SqlRDBResult code, const char *fmt, ...);

#endif /* C2SQL_ERROR_CTX_H */
