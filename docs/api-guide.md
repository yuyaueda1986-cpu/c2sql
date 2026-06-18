# libc2sql API利用者向け取扱説明書

Cアプリケーションからlibc2sqlを利用するための実用ガイド。インクルードはヘッダ1本（`#include "c2sql.h"`）、リンクは`-lc2sql -lsqlite3 -lpthread`。

## 目次

1. [3ステップAPI入門](#1-3ステップapi入門)
2. [構造体スキーマの記述](#2-構造体スキーマの記述)
3. [書き込み API](#3-書き込み-api)
4. [読み出し API](#4-読み出し-api)
5. [削除 API](#5-削除-api)
6. [検索条件の組み立て](#6-検索条件の組み立て)
7. [NULLビットマップ](#7-nullビットマップ)
8. [可変長BLOB](#8-可変長blob)
9. [トランザクション](#9-トランザクション)
10. [エラーハンドリングと診断ログ](#10-エラーハンドリングと診断ログ)
11. [設定 `SqlRDBConfig`](#11-設定-sqlrdbconfig)
12. [スレッド利用上の注意](#12-スレッド利用上の注意)
13. [典型的なミスとその回避](#13-典型的なミスとその回避)

---

## 1. 3ステップAPI入門

```c
#include "c2sql.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t id;
    char    name[32];
    double  score;
} Person;

static const SqlRDBColumnDef PERSON_COLS[] = {
    { "id",    SQL_TYPE_INT32, offsetof(Person, id),    4,              SQL_COL_FLAG_PRIMARY_KEY },
    { "name",  SQL_TYPE_TEXT,  offsetof(Person, name),  32,             SQL_COL_FLAG_NONE        },
    { "score", SQL_TYPE_REAL,  offsetof(Person, score), sizeof(double), SQL_COL_FLAG_NULLABLE    },
};

int main(void) {
    SqlRDBHandle *h = SqlRDBInit("app.db");                /* ① 接続 */
    SqlRDBRegisterStruct(h, "persons", PERSON_COLS, 3);    /* ② 登録 */

    Person alice = { 1, "Alice", 9.5 };
    SqlRDBWrite(h, "persons", &alice, NULL);               /* ③ CRUD */

    SqlRDBClose(h);
    return 0;
}
```

接続文字列の指定:

| DSN | 効果 |
|--|--|
| `":memory:"` | プロセス内in-memory DB（テスト・キャッシュ用） |
| `"path/to/file.db"` | 通常のSQLite3ファイル（無ければ作成） |
| `"file:foo.db?mode=ro"` | URI形式（読み取り専用など） |
| `"postgresql://user:pass@host:5432/db"` | PostgreSQL（`-DC2SQL_WITH_POSTGRES=ON` ビルド時。`postgres://` も可） |

`postgresql://` / `postgres://` で始まる接続文字列は PostgreSQL ドライバを選択する。
それ以外は SQLite として扱う。API・構造体・エラーコードはバックエンド非依存で不変。

---

## 2. 構造体スキーマの記述

```c
typedef struct SqlRDBColumnDef {
    const char  *name;
    SqlRDBType   type;     /* SQL_TYPE_INT32/INT64/REAL/TEXT/BLOB */
    size_t       offset;   /* offsetof(構造体, メンバ) */
    size_t       size;     /* sizeof メンバ */
    unsigned     flags;    /* SqlRDBColFlag のビット和 */
} SqlRDBColumnDef;
```

### 型と必要な`size`

| `type` | C側メンバ | `size`の決め方 |
|--|--|--|
| `SQL_TYPE_INT32` | `int32_t` | 4（厳密）。違反時 `INVALID_ARG` |
| `SQL_TYPE_INT64` | `int64_t` | 8（厳密） |
| `SQL_TYPE_REAL` | `double` | `sizeof(double)` |
| `SQL_TYPE_TEXT` | `char[N]`（NUL終端） | バッファ全長 `N` |
| `SQL_TYPE_BLOB` | `uint8_t[N]` | 固定長`N`。可変長は別API |

### フラグ

| `SqlRDBColFlag` | 効果 |
|--|--|
| `SQL_COL_FLAG_NONE` | 既定（NOT NULL） |
| `SQL_COL_FLAG_PRIMARY_KEY` | 主キー（スキーマあたり最大1列） |
| `SQL_COL_FLAG_NULLABLE` | NULLを許容する |
| `SQL_COL_FLAG_UNIQUE` | UNIQUE制約を付与（PKと併用時は冗長扱い） |

### 識別子の制約

- `struct_name`および`name`はASCII英字または`_`で始まり、英数字または`_`のみ
- 長さは最大128文字
- SQL予約語（`SELECT`, `ROWS`, `KEY`等）は不可。違反時`SQL_RDB_ERR_INVALID_NAME`

### 登録の流れ

```c
SqlRDBResult r = SqlRDBRegisterStruct(h, "persons", PERSON_COLS, 3);
if (r != SQL_RDB_OK) {
    SqlRDBResult code;
    fprintf(stderr, "register failed: %s\n", SqlRDBLastError(h, &code));
}
```

登録時にCREATE TABLE発行・既存テーブルとの突合・必要ならALTER TABLEまでが実行される。詳細は[DBメンテナ向け取扱説明書](maintenance-guide.md)参照。

---

## 3. 書き込み API

### 単一行 `SqlRDBWrite`

```c
SqlRDBResult SqlRDBWrite(
    SqlRDBHandle *h,
    const char   *struct_name,
    const void   *row,
    const uint8_t *null_map);  /* NULLビットマップ。不要ならNULL */
```

- PK登録ありの場合: PKが既存と一致すればUPDATE、なければINSERT（UPSERT）
- PK登録なしの場合: 常にINSERT
- 戻り値: `SQL_RDB_OK` / 各種エラーコード

### バッチ書き込み `SqlRDBWriteMany`

```c
SqlRDBResult SqlRDBWriteMany(
    SqlRDBHandle *h,
    const char   *struct_name,
    const void   *rows,           /* 構造体配列の先頭 */
    size_t        count,
    size_t        stride,          /* 通常は sizeof(構造体) */
    const uint8_t *null_maps);     /* count分連結。不要ならNULL */
```

- 既存TXが無ければ自動的に1トランザクションでラップ → COMMIT
- 1行でも失敗すれば全行ロールバック

---

## 4. 読み出し API

### 単一行 `SqlRDBRead`

```c
SqlRDBResult SqlRDBRead(
    SqlRDBHandle  *h,
    const char    *struct_name,
    const SqlRDBCondition *cond,
    void          *out_row,
    uint8_t       *out_null_map,   /* 不要ならNULL */
    const SqlRDBReadOpts *opts);   /* NULL=既定: 複数行ヒットでエラー */
```

行特定セマンティクス（**重要**）:

| 結果 | 戻り値 | `out_row`/`out_null_map` |
|--|--|--|
| 1行ヒット | `SQL_RDB_OK` | 上書き |
| 0行 | `SQL_RDB_ERR_NOT_FOUND` | **変更しない** |
| 2行以上 (既定) | `SQL_RDB_ERR_MULTIPLE_ROWS` | **変更しない** |
| 2行以上 (`opts->allow_multi=true`) | `SQL_RDB_OK` | 先頭行で上書き |
| 文字列が登録sizeを超過 | `SQL_RDB_WARN_TRUNCATED` | 切り詰めて上書き |

```c
SqlRDBReadOpts opts = { .allow_multi = true, .max_text_len = 0 };
SqlRDBRead(h, "persons", cond, &out, NULL, &opts);
```

### 複数行イテレータ `SqlRDBReadMany` / `SqlRDBStmtNext` / `SqlRDBStmtFree`

```c
SqlRDBCondition *cond = SqlRDBCondAll();
SqlRDBStmt      *iter = NULL;
SqlRDBReadMany(h, "persons", cond, &iter);
SqlRDBCondFree(cond);

Person row;
while (SqlRDBStmtNext(iter, &row, NULL) == SQL_RDB_OK) {
    /* use row */
}
SqlRDBStmtFree(iter);   /* 必須: ドライバ finalize と内部解放 */
```

`SqlRDBStmtNext`は終端で`SQL_RDB_ERR_NOT_FOUND`を返す。

---

## 5. 削除 API

```c
SqlRDBResult SqlRDBDelete(
    SqlRDBHandle  *h,
    const char    *struct_name,
    const SqlRDBCondition *cond,    /* NULL不可 (安全弁) */
    size_t        *out_deleted);    /* 影響行数、不要ならNULL */
```

- `cond == NULL` → `SQL_RDB_ERR_INVALID_ARG`（誤って全件削除しないための安全弁）
- 全件削除したい場合は明示的に `SqlRDBCondAll()` を渡す

```c
SqlRDBCondition *all = SqlRDBCondAll();
size_t deleted = 0;
SqlRDBDelete(h, "persons", all, &deleted);
SqlRDBCondFree(all);
```

---

## 5.5 件数カウント `SqlRDBCount`

```c
SqlRDBResult SqlRDBCount(
    SqlRDBHandle  *h,
    const char    *struct_name,
    const SqlRDBCondition *cond,   /* NULL または SqlRDBCondAll() で全件 */
    size_t        *out_count);     /* 一致件数を受け取る（NULL不可） */
```

- 読み取り専用。テーブルを変更しない。
- `SqlRDBDelete` と異なり `cond == NULL` を許容し「全件カウント」を意味する（カウントは破壊的でないため安全弁不要）。
- 条件中の未登録カラムは `SQL_RDB_ERR_UNKNOWN_COLUMN`。

```c
size_t total = 0;
SqlRDBCount(h, "persons", NULL, &total);              /* 全件 */

SqlRDBCondition *c = SqlRDBCondReal("score", SQL_OP_GE, 8.0);
size_t hi = 0;
SqlRDBCount(h, "persons", c, &hi);                    /* score>=8.0 の件数 */
SqlRDBCondFree(c);
```

### 容量ガード（`max_records`）

スキーマ仕様書（[tools/README.md](../tools/README.md)）に `"enforce_max_records": true`
を指定して `c2sql-gen` で生成すると、容量チェック付きの書き込みラッパ
`Write<Name>Guarded()` が生成される。

```c
/* 生成ヘッダ sessions_schema.h より */
SqlRDBResult WriteSessionsGuarded(SqlRDBHandle *h, const Session *row,
                                  const uint8_t *null_map);
```

- 既存PKへのUPSERT（テーブルが増えない更新）は容量超過でも許可。
- 新規INSERTで件数が `*_MAX_RECORDS` 以上になる場合は `SQL_RDB_ERR_CAPACITY_EXCEEDED` を返し、書き込まない。
- ベストエフォート（カウントと書き込みは単一トランザクションではない）。厳密な不変条件が必要なら、呼出側で `SqlRDBBeginTx`/`SqlRDBCommitTx` で囲む。

---

## 6. 検索条件の組み立て

### リーフ条件

```c
SqlRDBCondition *SqlRDBCondInt (const char *col, int op, int64_t value);
SqlRDBCondition *SqlRDBCondText(const char *col, int op, const char *value);
SqlRDBCondition *SqlRDBCondReal(const char *col, int op, double value);
SqlRDBCondition *SqlRDBCondBlob(const char *col, int op, const void *bytes, size_t len);
```

`op`に指定できる演算子:

| 列挙 | SQL |
|--|--|
| `SQL_OP_EQ` | `=` |
| `SQL_OP_NE` | `!=` |
| `SQL_OP_LT` | `<` |
| `SQL_OP_LE` | `<=` |
| `SQL_OP_GT` | `>` |
| `SQL_OP_GE` | `>=` |

### 合成

```c
SqlRDBCondition *SqlRDBCondAnd(SqlRDBCondition *a, SqlRDBCondition *b);
SqlRDBCondition *SqlRDBCondOr (SqlRDBCondition *a, SqlRDBCondition *b);
SqlRDBCondition *SqlRDBCondAll(void);   /* WHERE句なし相当 */
```

例: `score >= 8.0 AND name != 'Alice'`:

```c
SqlRDBCondition *c1 = SqlRDBCondReal("score", SQL_OP_GE, 8.0);
SqlRDBCondition *c2 = SqlRDBCondText("name",  SQL_OP_NE, "Alice");
SqlRDBCondition *q  = SqlRDBCondAnd(c1, c2);
/* 利用後: */
SqlRDBCondFree(q);   /* 再帰的に c1/c2 ごと解放 */
```

### 寿命と所有権

- リーフが参照する `value`（特に文字列・BLOBバイト列）は、**条件オブジェクトを使い終わるまで利用者側で生存させる**
- `SqlRDBCondFree`は受け取ったノードと子孫を再帰的に解放
- 同じ条件ASTを複数のAPI呼出で再利用しても良い

### 未登録カラムの扱い

リーフのカラム名がスキーマに無い場合、API呼出（Read/Delete等）が`SQL_RDB_ERR_UNKNOWN_COLUMN`を返す。条件構築時には検出されない。

---

## 7. NULLビットマップ

長さは `SQL_RDB_NULL_BITMAP_BYTES(col_count)` バイト。`bit N` がカラムN（登録順）に対応（LSB側から）。

### 書き込み時

```c
Person p = { .id = 1, .name = "Alice" };  /* score 未設定 */
uint8_t nm = 0x04;                         /* bit 2 = score → SQL NULL */
SqlRDBWrite(h, "persons", &p, &nm);
```

- bit=1のカラムが `SQL_COL_FLAG_NULLABLE` 未指定 → `SQL_RDB_ERR_NOT_NULL_VIOLATION`、書き込みは行われない
- `null_map = NULL` の場合は全カラムを値ありとして扱う

### 読み出し時

```c
uint8_t nm = 0;
SqlRDBRead(h, "persons", cond, &out, &nm, NULL);
if (nm & 0x04) {
    /* score は SQL NULL だった。out.score の値は未変更 */
}
```

- 出力時、SQL NULLのカラムは構造体メンバを**変更しない**ため、呼出側は事前にデフォルト値で初期化することを推奨

---

## 8. 可変長BLOB

構造体一括CRUDでは`SqlRDBColumnDef.size`を固定長として扱う。可変長の読み書きは専用API:

```c
SqlRDBResult SqlRDBWriteBlobField(
    SqlRDBHandle *h, const char *struct_name,
    const SqlRDBCondition *key, const char *col_name,
    const void *bytes, size_t len);

SqlRDBResult SqlRDBReadBlobField(
    SqlRDBHandle *h, const char *struct_name,
    const SqlRDBCondition *key, const char *col_name,
    void **out_bytes, size_t *out_len);

SqlRDBResult SqlRDBFreeResult(void *ptr);   /* out_bytes の解放 */
```

**重要**: 対象列は`SQL_TYPE_BLOB`のみ。それ以外は`INVALID_ARG`。

行特定セマンティクス:

| 結果 | Write | Read |
|--|--|--|
| 0行ヒット | `NOT_FOUND`（書き込まない） | `NOT_FOUND`（`out_bytes`/`out_len`不変） |
| 1行ヒット | OK（厳密サイズで上書き） | OK（内部確保バッファを返却） |
| 2行以上 | `MULTIPLE_ROWS`（書き込まない） | `MULTIPLE_ROWS`（不変） |

```c
uint8_t big[1024];
SqlRDBCondition *key = SqlRDBCondInt("id", SQL_OP_EQ, 42);
SqlRDBWriteBlobField(h, "docs", key, "payload", big, sizeof(big));
SqlRDBCondFree(key);

SqlRDBCondition *k2 = SqlRDBCondInt("id", SQL_OP_EQ, 42);
void  *buf = NULL;
size_t len = 0;
SqlRDBReadBlobField(h, "docs", k2, "payload", &buf, &len);
SqlRDBCondFree(k2);
/* use buf[0..len) */
SqlRDBFreeResult(buf);
```

---

## 9. トランザクション

```c
SqlRDBBeginTx(h);
SqlRDBWrite(h, "persons", &alice, NULL);
SqlRDBWrite(h, "persons", &bob,   NULL);
SqlRDBCommitTx(h);   /* 失敗時は SqlRDBRollbackTx(h) */
```

### ネスト（SAVEPOINT）

```c
SqlRDBBeginTx(h);                 /* depth 1 = BEGIN */
  SqlRDBWrite(h, "x", &a, NULL);

  SqlRDBBeginTx(h);               /* depth 2 = SAVEPOINT sp_2 */
    SqlRDBWrite(h, "x", &b, NULL);
  SqlRDBRollbackTx(h);            /* b のみ破棄 */

SqlRDBCommitTx(h);                 /* a は確定 */
```

- 最大ネスト深度: 16段。超過時 `SQL_RDB_ERR_NESTED_TX`
- depth==0で Commit/Rollback → `SQL_RDB_ERR_NO_ACTIVE_TX`
- `SqlRDBClose`時に進行中TXがあれば暗黙のrollback

`SqlRDBWriteMany`は外側TXが無い時のみ暗黙TXを張る。明示TX中に呼ばれた場合は外側TXに参加する。

---

## 10. エラーハンドリングと診断ログ

全ての公開APIは`SqlRDBResult`を返却。`0`がOK、正値が警告、負値がエラー。

```c
SqlRDBResult code = SQL_RDB_OK;
const char *msg = SqlRDBLastError(h, &code);
if (code != SQL_RDB_OK) {
    fprintf(stderr, "[%d] %s\n", code, msg);
}
```

### 主なエラーコード

| コード | 意味 |
|--|--|
| `SQL_RDB_ERR_INVALID_ARG` | 必須引数NULL等 |
| `SQL_RDB_ERR_INVALID_HANDLE` | NULL/freedハンドル |
| `SQL_RDB_ERR_INVALID_NAME` | 識別子規約違反 |
| `SQL_RDB_ERR_DB_OPEN` | DBファイルが開けない |
| `SQL_RDB_ERR_NO_MEMORY` | mallocが失敗 |
| `SQL_RDB_ERR_DUPLICATE_SCHEMA` | 同名で再登録 |
| `SQL_RDB_ERR_DUPLICATE_COLUMN` | カラム名重複 |
| `SQL_RDB_ERR_TOO_MANY_COLUMNS` | 上限64超過 |
| `SQL_RDB_ERR_UNKNOWN_STRUCT` | 未登録の構造体名 |
| `SQL_RDB_ERR_UNKNOWN_COLUMN` | 条件中の未登録カラム |
| `SQL_RDB_ERR_SCHEMA_MISMATCH` | 既存テーブルが登録スキーマと不一致 |
| `SQL_RDB_ERR_NOT_FOUND` | 該当行なし |
| `SQL_RDB_ERR_MULTIPLE_ROWS` | 単一行APIで複数ヒット |
| `SQL_RDB_ERR_NOT_NULL_VIOLATION` | NULL不許可列にNULL指定 |
| `SQL_RDB_ERR_CAPACITY_EXCEEDED` | 容量ガード（`max_records`）がINSERTを拒否（生成ラッパ使用時） |
| `SQL_RDB_ERR_NO_ACTIVE_TX` | Commit/Rollback時TX無し |
| `SQL_RDB_ERR_NESTED_TX` | ネスト深度超過 |
| `SQL_RDB_ERR_DRIVER` | バックエンドエラー（詳細は`SqlRDBLastError`） |
| `SQL_RDB_ERR_INTERNAL` | 内部整合性違反 |
| `SQL_RDB_WARN_TRUNCATED` | 文字列が登録サイズで切り詰められた（成功扱い） |

### ロガーコールバック

```c
static void my_logger(void *user, SqlRDBResult code, const char *sql, const char *msg) {
    fprintf((FILE *)user, "[c2sql %d] %s | %s\n", code, sql ? sql : "", msg ? msg : "");
}
SqlRDBSetLogger(h, my_logger, stderr);
```

- 生成SQL（プレースホルダ`?`のまま）、結果コード、補足メッセージが渡される
- 機密性の高いアプリでは、SQLログ自体がリスクになる点に注意

---

## 11. 設定 `SqlRDBConfig`

```c
typedef struct SqlRDBConfig {
    bool   threadsafe;       /* 既定 true */
    size_t stmt_cache_size;  /* 既定 64。0で無効化 */
    bool   auto_migrate;     /* 既定 false */
    int    multirow_default; /* 0=エラー (既定), 1=先頭行 */
    bool   require_strict;   /* 既定 false。true で非STRICTテーブルを拒否 */
} SqlRDBConfig;

SqlRDBSetConfig(h, &cfg);
```

`stmt_cache_size`を変更すると、超過分はLRU順に`finalize`される。

---

## 12. スレッド利用上の注意

- `config.threadsafe=true`（既定）なら、同一ハンドルを複数スレッドで共有しても安全（API入口でハンドルmutex取得）
- 性能が重要な場合は**スレッドごとに別ハンドル**を持つ（mutex競合なし、ファイルDBはWAL推奨）
- `SqlRDBStmt`イテレータは内部でハンドルmutexを取得するため、他スレッドが同ハンドルへの並行操作中はブロック
- ロガーコールバックはハンドルmutex保持中に呼ばれる。コールバック内で長時間ブロックしたり同一ハンドルAPIを再呼出してはならない

---

## 13. 典型的なミスとその回避

### `SqlRDBCondFree` し忘れ

リーフ・合成ノード問わず、最上位ノードに対して`SqlRDBCondFree`を1回呼べば再帰的に解放される。ループで毎回構築するパターン:

```c
for (int i = 0; i < N; i++) {
    SqlRDBCondition *c = SqlRDBCondInt("id", SQL_OP_EQ, ids[i]);
    SqlRDBRead(h, "persons", c, &out, NULL, NULL);
    SqlRDBCondFree(c);   /* 必須 */
}
```

### `SqlRDBStmtFree` し忘れ

`SqlRDBReadMany`が返した`SqlRDBStmt`は必ず`SqlRDBStmtFree`で解放（ドライバの`finalize`が走る）。途中で関数を抜ける場合も忘れずに。

### 文字列の生存期間

`SqlRDBCondText("name", SQL_OP_EQ, buf)` の `buf` は、条件ASTを使い終わるまで生存させる必要がある。スタック変数を返却関数から漏らさないこと。

### 構造体メンバ追加の手順

```
1. 構造体に末尾メンバを追加
2. SqlRDBColumnDef[] にも末尾追加
3. config.auto_migrate = true で SqlRDBSetConfig
4. SqlRDBRegisterStruct を呼ぶ → ALTER TABLE ADD COLUMN が実行される
```

順序差は自動修復されないため、**末尾追加**で運用する。詳細は[DBメンテナ向け取扱説明書](maintenance-guide.md)参照。

### `SqlRDBClose`後のハンドル再利用

Close後のハンドルポインタへの操作は`SQL_RDB_ERR_INVALID_HANDLE`を返す（生存ハンドルレジストリで追跡）。クラッシュはしないが、ポインタはアプリケーション側で`NULL`化して再利用しないこと。

### TX内で `SqlRDBClose`

TX進行中のCloseは暗黙rollbackを試みる。アプリケーション側で明示Commit/Rollbackしてから閉じるのが望ましい。

### `null_map`のサイズミス

`SQL_RDB_NULL_BITMAP_BYTES(col_count)`バイト必要。`uint8_t nm = 0;` の1バイト変数で十分なのは7カラム以下のスキーマだけ。

```c
uint8_t nm[SQL_RDB_NULL_BITMAP_BYTES(PERSON_COL_COUNT)] = {0};
```
