/*
 * txn_manager.h — Transaction state for libc2sql.
 *
 * This header declares SqlRDBTxnState (embedded in SqlRDBHandle) and the
 * public transaction APIs (SqlRDBBeginTx/CommitTx/RollbackTx).
 *
 * Task 9 adds the state struct so SqlRDBClose can detect and roll back active
 * transactions.  Task 11 implements the full Begin/Commit/Rollback APIs.
 */
#ifndef C2SQL_TXN_MANAGER_H
#define C2SQL_TXN_MANAGER_H

#define C2SQL_MAX_TX_DEPTH 16
#define C2SQL_SP_NAME_LEN  32

typedef struct SqlRDBTxnState {
    int  depth;                                    /* 0 = no active transaction */
    char sp_names[C2SQL_MAX_TX_DEPTH][C2SQL_SP_NAME_LEN]; /* SAVEPOINT name stack */
} SqlRDBTxnState;

#endif /* C2SQL_TXN_MANAGER_H */
