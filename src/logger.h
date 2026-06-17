/*
 * logger.h — Per-handle logger callback storage and dispatch.
 */
#ifndef C2SQL_LOGGER_H
#define C2SQL_LOGGER_H

#include "c2sql.h"

typedef struct SqlRDBLoggerCtx {
    SqlRDBLoggerFn fn;
    void          *user;
} SqlRDBLoggerCtx;

/* Invoke the logger if set; does nothing if l == NULL or l->fn == NULL. */
void c2sql_internal_log(const SqlRDBLoggerCtx *l, SqlRDBResult code,
                        const char *sql, const char *msg);

#endif /* C2SQL_LOGGER_H */
