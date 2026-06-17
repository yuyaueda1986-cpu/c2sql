# libc2sql DBメンテナ向け取扱説明書

libc2sqlが生成・管理するSQLite3データベースの運用ガイド。テーブル構造の理解、バックアップ、マイグレーション、トラブルシュートを扱う。アプリケーション開発者向けの内容は[API利用者向け取扱説明書](api-guide.md)を参照。

## 目次

1. [libc2sqlが生成するテーブルの構造](#1-libc2sqlが生成するテーブルの構造)
2. [sqlite3 CLIによる検査](#2-sqlite3-cliによる検査)
3. [スキーマ突合の判定ルール](#3-スキーマ突合の判定ルール)
4. [自動マイグレーション](#4-自動マイグレーション)
5. [手動マイグレーションが必要なケース](#5-手動マイグレーションが必要なケース)
6. [BLOB列の運用](#6-blob列の運用)
7. [バックアップ・リストア](#7-バックアップリストア)
8. [診断ログの収集](#8-診断ログの収集)
9. [パフォーマンスとトラブルシュート](#9-パフォーマンスとトラブルシュート)
10. [互換性とアップグレード](#10-互換性とアップグレード)

---

## 1. libc2sqlが生成するテーブルの構造

libc2sqlは登録された構造体スキーマから以下の形式のDDLを発行する:

```sql
CREATE TABLE IF NOT EXISTS "persons" (
    "id"    INTEGER NOT NULL PRIMARY KEY,
    "name"  TEXT    NOT NULL,
    "score" REAL
) STRICT;
```

特徴:

| 要素 | 仕様 |
|--|--|
| テーブル名 | `SqlRDBRegisterStruct` の `struct_name` 引数（識別子検証通過済み） |
| 列名 | `SqlRDBColumnDef.name`（同上） |
| `STRICT` | 既定で常に付与。SQLite3.37+で型チェックが厳格になる |
| `NOT NULL` | `SQL_COL_FLAG_NULLABLE` 未指定の列に自動付与 |
| `PRIMARY KEY` | `SQL_COL_FLAG_PRIMARY_KEY` 列に付与（スキーマあたり最大1列） |
| `UNIQUE` | `SQL_COL_FLAG_UNIQUE` 列に付与（PK列は冗長になるため省略） |
| 既定値 | 設定しない |
| 外部キー | 初期版では未対応 |

### 型クラスの対応表

| `SqlRDBType` | SQLite型 | 備考 |
|--|--|--|
| `SQL_TYPE_INT32` / `SQL_TYPE_INT64` | `INTEGER` | DB側では一律INTEGER。読み出し時にlibc2sqlがINT32範囲チェック |
| `SQL_TYPE_REAL` | `REAL` | IEEE 754 倍精度 |
| `SQL_TYPE_TEXT` | `TEXT` | NUL終端UTF-8想定 |
| `SQL_TYPE_BLOB` | `BLOB` | 構造体一括CRUDでは固定長扱い、可変長は補助API |

### NULL表現

- 構造体メンバ自体にNULL可否は無く、書き込み・読み出し時に**NULLビットマップ**引数で表現
- NULL列は構造体内の領域を消費せず、SQLite側でNULL格納

---

## 2. sqlite3 CLIによる検査

libc2sqlが作成したDBは普通のSQLiteファイルなので、`sqlite3` CLIで直接検査・操作できる。

```sh
$ sqlite3 app.db
sqlite> .schema persons
CREATE TABLE "persons" (
    "id" INTEGER NOT NULL PRIMARY KEY,
    "name" TEXT NOT NULL,
    "score" REAL
) STRICT;

sqlite> SELECT * FROM persons LIMIT 5;

sqlite> .mode column
sqlite> .headers on
sqlite> PRAGMA table_info("persons");
cid  name   type     notnull  dflt_value  pk
---  -----  -------  -------  ----------  --
0    id     INTEGER  1                    1
1    name   TEXT     1                    0
2    score  REAL     0                    0

sqlite> PRAGMA index_list("persons");
sqlite> PRAGMA foreign_key_check;
sqlite> PRAGMA integrity_check;
```

整合性チェックは長時間DBを使うシステムで定期実行を推奨。

---

## 3. スキーマ突合の判定ルール

`SqlRDBRegisterStruct` 呼出時、libc2sqlは以下の順で検証する:

```
CREATE TABLE IF NOT EXISTS ... STRICT
  ↓
PRAGMA table_info("<name>")        → 既存カラムを読み込み
PRAGMA index_list / index_info     → UNIQUE制約を判別
SELECT sql FROM sqlite_schema ...  → STRICT属性を判別
  ↓
登録スキーマと6項目比較
  ├ カラム名 (大文字小文字を区別、完全一致)
  ├ カラム順序 (cid順 vs 登録順)
  ├ 型クラス  (INTEGER/REAL/TEXT/BLOB)
  ├ NOT NULL  (NULLABLE未指定列 ↔ notnull=1)
  ├ PRIMARY KEY (PK指定列 ↔ pk>0)
  └ UNIQUE    (UNIQUE指定列 ↔ origin='u' な単一列インデックス)
```

突合結果と挙動:

| 状態 | `auto_migrate=false` (既定) | `auto_migrate=true` |
|--|--|--|
| 全項目一致 | OK | OK |
| 末尾カラム不足 (`schema` 側が多い) | `SCHEMA_MISMATCH` | `ALTER TABLE ADD COLUMN` 後再突合 → OK |
| カラム順序差 | `SCHEMA_MISMATCH` | `SCHEMA_MISMATCH`（順序差は自動修復しない） |
| カラム数超過 (`DB` 側が多い) | `SCHEMA_MISMATCH` | `SCHEMA_MISMATCH` |
| 型クラス差 | `SCHEMA_MISMATCH` | `SCHEMA_MISMATCH` |
| NOT NULL差 | `SCHEMA_MISMATCH` | `SCHEMA_MISMATCH` |
| PK差 | `SCHEMA_MISMATCH` | `SCHEMA_MISMATCH` |
| UNIQUE 不足 | `SCHEMA_MISMATCH` | `SCHEMA_MISMATCH` |
| 非STRICT (`require_strict=false`、既定) | ロガー警告のみ、続行 | 同左 |
| 非STRICT (`require_strict=true`) | `SCHEMA_MISMATCH` | 同左 |

### INT32とINT64の扱い

SQLite側はいずれも`INTEGER`に正規化されるため、スキーマ突合では同一とみなす。スキーマで`INT32`を宣言した列にINT64の値が格納されていた場合、読み出し時に`int32_t`範囲外なら`SQL_RDB_WARN_TRUNCATED`が返る（値は切り詰め）。

---

## 4. 自動マイグレーション

末尾カラム追加に限り、`auto_migrate=true`設定で自動補完できる。

### 設定方法

```c
SqlRDBHandle *h = SqlRDBInit("app.db");

SqlRDBConfig cfg = {
    .threadsafe       = true,
    .stmt_cache_size  = 64,
    .auto_migrate     = true,    /* ←有効化 */
    .multirow_default = 0,
    .require_strict   = false,
};
SqlRDBSetConfig(h, &cfg);

SqlRDBRegisterStruct(h, "persons", PERSON_COLS, PERSON_COL_COUNT);
```

### 内部動作

```
1. CREATE TABLE IF NOT EXISTS (既存テーブル無修正)
2. PRAGMA table_info で db_cols 取得
3. 共通プレフィックス比較 — 一致を確認
4. 不足分について、各カラムに対して
       ALTER TABLE "<name>" ADD COLUMN "<col>" <TYPE> [NOT NULL] [UNIQUE]
   を発行
5. PRAGMA table_info を再取得し、最終一致を再確認
```

### 自動マイグレーションが失敗するケース

- 追加カラムが`NOT NULL`で既存行が存在する場合、SQLiteは「NOT NULLカラムをデフォルト値なしで追加不可」とエラーを返す（`SQL_RDB_ERR_DRIVER`が返る）。回避策はカラムを`SQL_COL_FLAG_NULLABLE`で追加するか、手動マイグレーションを行う
- 追加カラムが`PRIMARY KEY`の場合、SQLiteは既存テーブルへのPK追加を許さない。PK追加は手動マイグレーションで`CREATE TABLE`→`INSERT INTO ... SELECT`→`DROP`→`RENAME`の手順を踏む

---

## 5. 手動マイグレーションが必要なケース

`auto_migrate`では修復できない変更は手動で行う。libc2sqlを起動する前に、外部ツール（`sqlite3` CLI や マイグレーションSQLスクリプト）で実施する。

### 5.1 カラム順序の変更

```sql
BEGIN;
CREATE TABLE "persons_new" (
    "id"    INTEGER NOT NULL PRIMARY KEY,
    "score" REAL,
    "name"  TEXT NOT NULL
) STRICT;
INSERT INTO "persons_new" (id, score, name)
    SELECT id, score, name FROM "persons";
DROP TABLE "persons";
ALTER TABLE "persons_new" RENAME TO "persons";
COMMIT;
```

### 5.2 カラム削除

```sql
BEGIN;
CREATE TABLE "persons_new" (
    "id"   INTEGER NOT NULL PRIMARY KEY,
    "name" TEXT    NOT NULL
) STRICT;
INSERT INTO "persons_new" (id, name) SELECT id, name FROM "persons";
DROP TABLE "persons";
ALTER TABLE "persons_new" RENAME TO "persons";
COMMIT;
```

### 5.3 型変更

例: `id` を `INTEGER` → `TEXT` にする場合は5.1と同じ手順で新テーブル作成→`CAST`してコピー。

### 5.4 NOT NULL の追加

```sql
BEGIN;
UPDATE "persons" SET "name" = '' WHERE "name" IS NULL;   -- 既存NULLを埋める
CREATE TABLE "persons_new" (
    "id"   INTEGER NOT NULL PRIMARY KEY,
    "name" TEXT    NOT NULL
) STRICT;
INSERT INTO "persons_new" SELECT * FROM "persons";
DROP TABLE "persons";
ALTER TABLE "persons_new" RENAME TO "persons";
COMMIT;
```

### 5.5 PK追加

PKを後から追加する場合も新テーブル作成方式で対応する。

### 5.6 確認

手動マイグレーション後は、libc2sqlを起動して`RegisterStruct`が`SQL_RDB_OK`を返すことで整合を確認する。

---

## 6. BLOB列の運用

### 構造体一括CRUD

```c
typedef struct {
    int32_t  id;
    uint8_t  payload[16];  /* 固定長 */
} Doc;
```

- `SqlRDBColumnDef.size`で指定したバイト数を厳密に読み書き
- 値が`size`を超えていた場合、`SqlRDBRead`は先頭`size`バイトのみコピーする
- 値が`size`未満の場合、余剰領域は変更されない

### 可変長BLOB

```c
SqlRDBWriteBlobField(h, "docs", key, "payload", bytes, len);
SqlRDBReadBlobField (h, "docs", key, "payload", &buf, &len);
SqlRDBFreeResult(buf);
```

- スキーマの`size`を超えるサイズも書き込み可能（DB側は実サイズで格納）
- 同じ列を構造体一括CRUDと可変長APIで併用すると、固定長読み出し時に切り詰められる点に注意

### CLIでの確認

```sh
sqlite> SELECT id, length(payload) FROM docs;
id  length(payload)
--  ---------------
1                64
2                 8
```

`length()`は実バイト数を返すため、`size`列の運用と乖離していないかチェックできる。

---

## 7. バックアップ・リストア

libc2sqlのDBは標準SQLiteファイルなので、SQLite標準の手法を全て使える。

### ファイルコピー（アプリ停止中）

```sh
cp app.db backups/app-$(date +%Y%m%d).db
```

### オンラインバックアップ（アプリ稼働中）

`sqlite3` CLIに付属の`.backup`コマンドが安全:

```sh
sqlite3 app.db ".backup backups/app-online.db"
```

または、libc2sqlのプロセスが落ちないよう WAL モードに切り替えた上で `VACUUM INTO` を使う:

```c
SqlRDBHandle *h = SqlRDBInit("app.db");
/* libc2sql は実行用SQLを公開していないので、現状はバックアップ用の
   sqlite3_exec を別接続から発行するのが推奨 */
```

WAL有効化（CLI側）:

```sh
sqlite3 app.db "PRAGMA journal_mode=WAL;"
```

### リストア

ファイルベース: 停止状態でファイルを置換するだけ。

スキーマ移行を伴う場合:

1. 旧DBから`sqlite3 old.db .dump > dump.sql`でSQLダンプを取得
2. 新スキーマで`CREATE TABLE`し、`INSERT`部分を流し込む
3. libc2sqlで`SqlRDBRegisterStruct`を呼んで突合確認

---

## 8. 診断ログの収集

libc2sqlは`SqlRDBSetLogger`で登録したコールバックに対し、生成SQL・結果コード・補足メッセージを送出する。

```c
static void file_logger(void *user, SqlRDBResult code, const char *sql, const char *msg) {
    FILE *fp = user;
    fprintf(fp, "[%d] sql=%s msg=%s\n", code, sql ? sql : "", msg ? msg : "");
    fflush(fp);
}
SqlRDBSetLogger(h, file_logger, stderr);
```

ログには以下が含まれうる:

- マイグレーションで実行された`CREATE TABLE` / `ALTER TABLE`
- スキーマ不一致時の不一致内訳（カラム番号と種類: name/type/notnull/pk/unique）
- 各CRUD呼出のSQLおよびエラー詳細

機密性の高いデータでは、SQLに含まれる値はプレースホルダ`?`のままだが、テーブル名・カラム名・条件構造が漏れる点に注意（バインド値はログに出ない）。

---

## 9. パフォーマンスとトラブルシュート

### 性能ベースライン（in-memory SQLite）

| 操作 | 目安 |
|--|--|
| 10,000件 WriteMany | ≤ 100ms |
| PKルックアップ（Read） | ≤ 0.1ms |

ファイルベースDBではI/Oに左右される。WALモード + 適切な`PRAGMA synchronous`設定で大幅に改善する。

### よくあるトラブル

| 症状 | 原因 | 対処 |
|--|--|--|
| `SqlRDBRegisterStruct` が `SCHEMA_MISMATCH` | DBが旧スキーマと不整合 | `PRAGMA table_info`で確認し、5章の手動マイグレーション、または`auto_migrate=true`設定 |
| 大量書き込みが遅い | autocommitで毎回fsync | 明示的に`SqlRDBBeginTx` ... `SqlRDBCommitTx`、または`SqlRDBWriteMany`使用 |
| 複数プロセスからアクセスして`SQLITE_BUSY`が頻発 | デフォルトロールバックジャーナル | CLIで`PRAGMA journal_mode=WAL;`を設定 |
| メモリ使用が増え続ける | ステートメントキャッシュサイズ不足、または`SqlRDBStmtFree`し忘れ | `stmt_cache_size`の調整、コードレビュー |
| `SqlRDBWriteBlobField`が `MULTIPLE_ROWS` | 条件が複数行マッチ | PK等で1行を特定する条件に変更 |
| 起動直後にハンドルが取れない | DBファイルが他プロセスで排他 | プロセス重複起動の確認、WALモード使用 |

### 整合性検証

```sh
sqlite3 app.db "PRAGMA integrity_check;"
sqlite3 app.db "PRAGMA quick_check;"
```

破損疑いがある場合は`.dump` → 新ファイルへロード で修復を試みる。

### サイズ最適化

```sh
sqlite3 app.db "VACUUM;"
```

長期運用で削除が多いDBでは定期VACUUMが効く。

---

## 10. 互換性とアップグレード

### SQLite3バージョン

| 機能 | 必要バージョン |
|--|--|
| UPSERT (`ON CONFLICT ... DO UPDATE`) | 3.35.0以上 |
| STRICT テーブル | 3.37.0以上 |

libc2sqlのCMakeは3.35.0以上を必須としているが、STRICT付きCREATEを発行するため**実用上は3.37.0以上を推奨**する。旧バージョンを使うDBは`require_strict=false`を維持し、ログに非STRICT警告が出る。

### libc2sql本体のアップグレード

- 構造体メンバ追加は末尾のみ（順序差は自動修復されない）
- 公開APIの破壊的変更はメジャー番号で示す（v0.x系では予告なく変更されうる）
- 内部ヘッダ（`src/*.h`）に直接依存している場合は要再確認

### 別バックエンドへの移行（将来）

libc2sqlの内部はドライバ抽象化されているため、PostgreSQL等への対応は別スペックで追加可能。現状の利用者APIは変わらない方針。
