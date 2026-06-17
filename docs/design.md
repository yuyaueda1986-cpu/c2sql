# libc2sql 設計書

## 目的とスコープ

libc2sqlは「CアプリケーションがSQL文を直接書かずに、構造体ポインタでRDBの永続化を行える」ことを目的とする静的ライブラリである。利用者は4種類のCRUD API（Write / Read / Delete / WriteMany）と検索条件ビルダーのみで永続化を実装できる。

**初期版で提供する機能**:

- 接続管理（複数ハンドル並行可）
- 構造体スキーマ登録（最大64カラム）
- DDL自動生成（CREATE TABLE / ALTER TABLE ADD COLUMN）
- DML自動生成（INSERT / UPSERT / SELECT / COUNT / DELETE / 単一フィールドUPDATE）
- 型安全な検索条件AST（等価・大小比較・AND/OR・全件）
- 明示的トランザクション（SAVEPOINTによる16段ネスト）
- 可変長BLOB補助API
- スレッドセーフモード、ステートメントキャッシュ、診断ログ、エラーコンテキスト
- 既存テーブルとのスキーマ突合と、`auto_migrate`によるカラム追加
- **スキーマ仕様書（JSON）からの定義コード生成**（外部ツール `tools/c2sql-gen`。詳細は後述および[改善提案.md](改善提案.md)）

**初期版の非ゴール**:

- JOIN・サブクエリ・集計・ORDER BY / LIMIT
- SQLite3以外の同梱ドライバ（インターフェースのみ提供）
- 動的ライブラリ配布、`dlopen`によるドライバ動的ロード

> **方針変更（2026-06）**: 旧版では「構造体定義からのコード生成」を非ゴールとしていたが、
> 「管理者がバッチ作業で利用者へライブラリを払い出す」運用モデル（[ユーザーストーリー.md](ユーザーストーリー.md)）
> を採用し、コード生成をゴールに格上げした。ただし**ライブラリ本体（`src/`・`include/c2sql.h`）は
> 生成に非依存**であり、生成は本体の外側にある外部ツールに閉じる（後述「コード生成ツール」節）。
> これにより本体のABI・テスト・サニタイザ保証は不変のまま、管理者作業のバッチ化を実現する。

## アーキテクチャ

採用パターン: **Layered + Plugin Driver**。

```
+-----------------------------+
| Application                 |
+--------------+--------------+
               |
+--------------v--------------+   公開ヘッダ
| Public API (c2sql.h)        |   利用者唯一のエントリポイント
+--------------+--------------+
               |
+--------------v--------------------------------+
| Core: Handle Manager / Schema Registry        |
|       Query Builder / Condition AST           |
|       Statement Cache / Transaction Manager   |
|       Error Context / Logger / Mutex          |
|       Type Mapping / Migration                |
+--------------+--------------------------------+
               |
+--------------v--------------+   関数ポインタテーブル
| Driver Interface (vtable)   |   (open/exec/prepare/bind/step/column/tx)
+--------------+--------------+
               |
+--------------v--------------+
| SQLite3 Driver              |
+-----------------------------+
```

### 公開／内部の境界

| 区分 | 場所 | 例 |
|--|--|--|
| 公開ヘッダ | `include/c2sql.h` のみ | `SqlRDBHandle`, `SqlRDBInit`, `SqlRDBWrite` |
| 内部ヘッダ | `src/*.h` | `handle_internal.h`, `db_driver.h`, `migration.h` |
| 公開シンボル | `SqlRDB*` / `SQL_TYPE_*` / `SQL_RDB_*` / `SQL_OP_*` / `SQL_COL_FLAG_*` / `LIBC2SQL_VERSION_*` | — |
| 内部シンボル | `c2sql_internal_*` または `static` | `c2sql_internal_qb_build` |

## コンポーネントとファイル対応

| 設計コンポーネント | 主責任 | 実装ファイル |
|--|--|--|
| Public API | 公開API定義と引数検証 | `include/c2sql.h`, `src/c2sql.c` |
| Handle Manager | ハンドルライフサイクル、生存レジストリ、API入口の検証・mutex取得 | `src/c2sql.c`, `src/handle_internal.h` |
| Schema Registry | 構造体スキーマの登録・検索・解放 | `src/schema_registry.{c,h}` |
| Query Builder | DDL/DMLのSQL文字列生成 | `src/query_builder.{c,h}` |
| Condition AST | 検索条件のツリー構築・解放 | `src/condition_ast.{c,h}` |
| Statement Cache | プリペアドステートメントLRUキャッシュ | `src/stmt_cache.{c,h}` |
| Transaction Manager | depthカウンタとSAVEPOINTスタック管理 | `src/txn_manager.h`, `src/c2sql.c` |
| Error Context | ハンドル単位の直近エラーコード+メッセージ | `src/error_ctx.{c,h}` |
| Logger | コールバック登録と呼び出し | `src/logger.{c,h}` |
| Mutex Wrapper | POSIX/Windowsの抽象 | `src/mutex.{c,h}` |
| Type Mapping | C型↔SQL型変換、NULLビットマップ処理 | `src/type_mapping.{c,h}` |
| Migration | CREATE/PRAGMA/ALTERでDB側と整合 | `src/migration.{c,h}` |
| Driver Interface | 関数ポインタテーブル | `src/db_driver.h` |
| SQLite3 Driver | SQLite3用具体実装 | `src/sqlite_driver.{c,h}` |

## 主要フロー

### 構造体登録〜書き込み

1. `SqlRDBInit(dsn)` → ドライバオープン、ハンドル割当、生存レジストリ登録
2. `SqlRDBRegisterStruct(h, name, cols, n)` → 識別子検証 → スキーマレジストリへディープコピー → Migration呼び出し
3. Migration: `CREATE TABLE IF NOT EXISTS ... STRICT` → `PRAGMA table_info`/`index_list`で既存テーブル突合 → 末尾カラム不足かつ`auto_migrate`時のみ`ALTER TABLE ADD COLUMN`
4. `SqlRDBWrite(h, name, &row, NULL)` → スキーマ参照 → PKありUPSERT / PKなしINSERT のSQLを Query Builder で生成 → ステートメントキャッシュから取得 → Type Mappingでバインド → step

### 読み出し（単一行・複数行）

- `SqlRDBRead`: SELECT実行 → 1行目を内部バッファへ読み出し → 2行目存在チェック → 単一なら出力バッファへコピー、複数なら`MULTIPLE_ROWS`（出力バッファ不変）
- `SqlRDBReadMany`: 未キャッシュのプリペアドを返却 → `SqlRDBStmtNext`で逐次取得 → `SqlRDBStmtFree`で`finalize`

### トランザクション制御

```
depth 0  --Begin-->  depth 1 (BEGIN)         --Commit-->   depth 0
                  ↓ Begin
                  depth 2..16 (SAVEPOINT sp_N)
                  ↑ Commit (RELEASE sp_N)  / Rollback (ROLLBACK TO sp_N → RELEASE sp_N)
```

- depth==0で`Commit`/`Rollback` → `SQL_RDB_ERR_NO_ACTIVE_TX`
- depth==16で`Begin` → `SQL_RDB_ERR_NESTED_TX`
- `SqlRDBClose`時にdepth>0なら暗黙のrollback

## データモデル

### 公開構造体

```c
typedef struct SqlRDBColumnDef {
    const char  *name;   /* カラム名 (利用者所有) */
    SqlRDBType   type;   /* SQL_TYPE_INT32/INT64/REAL/TEXT/BLOB */
    size_t       offset; /* offsetof で得る構造体内オフセット */
    size_t       size;   /* sizeof で得るバイト数 */
    unsigned     flags;  /* SQL_COL_FLAG_NONE/PRIMARY_KEY/NULLABLE/UNIQUE のビット和 */
} SqlRDBColumnDef;
```

### 型マッピング

| `SqlRDBType` | C型 | SQLite型 |
|--|--|--|
| `SQL_TYPE_INT32` | `int32_t` | `INTEGER` |
| `SQL_TYPE_INT64` | `int64_t` | `INTEGER` |
| `SQL_TYPE_REAL` | `double` | `REAL` |
| `SQL_TYPE_TEXT` | `char[N]`（NUL終端） | `TEXT` |
| `SQL_TYPE_BLOB` | 固定長`uint8_t[N]` または可変長API | `BLOB` |

### NULLビットマップ

- 長さ: `SQL_RDB_NULL_BITMAP_BYTES(col_count)` = `ceil(col_count/8)` バイト
- `bit N` がカラムNに対応（LSB側から）
- 書き込み: bit=1なら`NULL`バインド（`NULLABLE`未指定列に対するbit=1は`NOT_NULL_VIOLATION`）
- 読み出し: SQL NULLのカラムは`out_null_map`の対応bitをセット、構造体メンバは未変更

### ハンドル状態と生存レジストリ

`SqlRDBHandle`はopaque型。内部に以下を持つ:

| フィールド | 用途 |
|--|--|
| `magic` | 高速な健全性確認（`SQL_RDB_HANDLE_MAGIC` / `SQL_RDB_HANDLE_DEAD`） |
| `config` | スレッドセーフ・cache_size・auto_migrate・require_strict 等 |
| `driver` / `driver_ctx` | vtable と バックエンド固有コンテキスト |
| `registry` / `cache` / `txn` | スキーマ・ステートメントキャッシュ・TX状態 |
| `error` / `logger` / `mutex` | 直近エラー・ロガー・mutex |

`SqlRDBClose`は`free(h)`した後の二度目のCloseでUAFを起こさないよう、グローバルな**生存ハンドルレジストリ**（pthread mutexで保護された配列）でアクティブハンドルを追跡する。Close時にレジストリから外し、二重Closeは「レジストリに無い」ことで`INVALID_HANDLE`を返す。

## エラー戦略

全ての公開APIは`SqlRDBResult`で結果を返却。エラーカテゴリ:

| 種別 | コード例 | 復旧 |
|--|--|--|
| 入力 | `INVALID_ARG`, `INVALID_HANDLE`, `INVALID_NAME`, `UNKNOWN_STRUCT`, `UNKNOWN_COLUMN`, `DUPLICATE_*`, `TOO_MANY_COLUMNS` | 呼出側で引数修正 |
| 状態 | `NO_ACTIVE_TX`, `NESTED_TX`, `SCHEMA_MISMATCH`, `NOT_FOUND`, `MULTIPLE_ROWS` | 呼出順序やデータの調整 |
| 整合性 | `NOT_NULL_VIOLATION` | 入力データ修正 |
| システム | `NO_MEMORY`, `DB_OPEN`, `DRIVER`, `INTERNAL` | リトライ／呼出側で対応 |
| 警告 | `WARN_TRUNCATED` | 成功扱い、利用者通知用 |

`SqlRDBLastError(h, &code)`でハンドル直近の詳細メッセージ（最大256バイト）を取得できる。

## 同期戦略

- `config.threadsafe=true`（既定）: 全公開API入口で `mutex_lock` → 終了時 `mutex_unlock`
- 生存ハンドルレジストリは別の `pthread_mutex_t` で保護
- SQLite3ドライバは`SQLITE_OPEN_NOMUTEX`で開き、上位のハンドルmutexで排他

## マイグレーション戦略

`RegisterStruct`時の流れ:

```
CREATE TABLE IF NOT EXISTS ... STRICT
   ↓
PRAGMA table_info / index_list / index_info で既存DB列構成を取得
sqlite_schema.sql から STRICT 属性を判定
   ↓
スキーマと突合 (name / 順序 / 型クラス / NOT NULL / PK / UNIQUE の6項目)
   ↓
一致 → OK
不一致 → SCHEMA_MISMATCH
末尾カラム不足 → auto_migrate なら ALTER TABLE ADD COLUMN 後に再突合
カラム順序差 → auto_migrate でも自動修復しない（SCHEMA_MISMATCH）
非STRICT → require_strict=true なら SCHEMA_MISMATCH、false ならロガー警告
```

詳細は[DBメンテナ向け取扱説明書](maintenance-guide.md)を参照。

## コード生成ツール（c2sql-gen）

管理者が利用者からの「スキーマ仕様書（JSON）」を受け取り、手書きの
`SqlRDBColumnDef[]` を排した定義コードをバッチ生成するための外部ツール。
本体ライブラリとは独立したレイヤであり、生成物は**利用者コード**として
`libc2sql.a` にリンクされる（本体の公開ヘッダ・ABIには影響しない）。

```
specs/persons.json ──► tools/c2sql-lint ──► tools/c2sql-gen ──► generated/
   (利用者が記入)         (仕様検証)            (定義生成)        persons_schema.h
                                                                persons_schema.c
                                                                persons.cmake
```

| 構成要素 | 役割 | 実装 |
|--|--|--|
| `tools/spec.schema.json` | スキーマ仕様書のJSON Schema | データ |
| `tools/c2sql_lint.py` | 仕様検証（識別子規約・型・主キー単一・最大件数） | Python（標準ライブラリのみ） |
| `tools/c2sql_gen.py` | 仕様書 → `*_schema.h` / `*_schema.c` / `*.cmake` 生成 | Python（標準ライブラリのみ） |
| `tools/build_batch.sh` | lint → gen → build → ctest の一括実行 | bash |
| `specs/*.json` | 受け渡し仕様書 | データ |
| `generated/*` | 生成物（`DO NOT EDIT` ヘッダ付き） | 生成 |

### 生成物の構造

`offset` と数値型の `size` は生成器が `offsetof` / `sizeof(((T*)0)->member)`
で機械的に埋めるため、利用者・管理者とも手入力しない（オフセット／サイズの
取り違えを構造的に排除）。`max_records` は `*_MAX_RECORDS` マクロとして
生成ヘッダに露出する（Phase 1 ではメタデータ。強制は[改善提案.md](改善提案.md)
の段階導入に従う）。

```c
/* generated/persons_schema.h（抜粋, AUTO-GENERATED） */
typedef struct Person { int32_t id; char name[32]; double score; } Person;
#define PERSONS_COL_COUNT   3u
#define PERSONS_MAX_RECORDS 100000
extern const SqlRDBColumnDef PERSONS_COLS[PERSONS_COL_COUNT];
SqlRDBResult RegisterPersons(SqlRDBHandle *h);
```

利用者は生成ヘッダを include し `RegisterPersons(h)` を呼ぶだけでよい
（`examples/generated_crud.c` が実証）。

## 性能・拡張性

- 主要操作はステートメントキャッシュ経由でO(1)準備、O(列数)バインド
- スキーマレジストリは線形検索（小規模想定、ハッシュ化APIに将来差し替え可）
- 性能スモーク基準: 10,000件INSERT @ in-memory SQLite ≤ 100ms、PKルックアップ ≤ 0.1ms

## セキュリティ

- 全ての利用者値はプリペアドステートメントのパラメータバインドで渡される（SQLインジェクション構造的排除）
- カラム名・テーブル名は識別子バリデーション（英数+アンダースコア、SQL予約語拒否）を通過したもののみSQLに展開
- ロガー出力には生成SQLが含まれるため、機密データを扱う環境では運用判断を要する

## バージョンと互換性

```c
#define LIBC2SQL_VERSION_MAJOR 0
#define LIBC2SQL_VERSION_MINOR 1
#define LIBC2SQL_VERSION_PATCH 0
```

メジャー0系のためABIは変更されうる。公開構造体に新メンバを追加する場合は末尾に追加し、サイズ判定で旧バイナリ互換を維持する方針。
