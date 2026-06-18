/*
 * postgres_driver.h — PostgreSQL (libpq) driver declaration.
 *
 * Only compiled when the library is built with -DC2SQL_WITH_POSTGRES=ON
 * (which also defines HAVE_POSTGRES). Selected at runtime by SqlRDBInit when
 * the connection string starts with "postgresql://" or "postgres://".
 */
#ifndef C2SQL_POSTGRES_DRIVER_H
#define C2SQL_POSTGRES_DRIVER_H

#include "db_driver.h"

extern const SqlRDBDriver g_postgres_driver;

#endif /* C2SQL_POSTGRES_DRIVER_H */
