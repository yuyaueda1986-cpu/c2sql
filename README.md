# libc2sql

C構造体ポインタだけでRDB（初期実装: SQLite3）の永続化を行うC11準拠の静的ライブラリ。SQL文を書かずに、CRUD・検索条件・トランザクション・スキーマ自動マイグレーションを扱える。

```c
SqlRDBHandle *h = SqlRDBInit(":memory:");
SqlRDBRegisterStruct(h, "persons", PERSON_COLS, 3);

Person alice = { 1, "Alice", 9.5 };
SqlRDBWrite(h, "persons", &alice, NULL);

Person out;
SqlRDBCondition *cond = SqlRDBCondInt("id", SQL_OP_EQ, 1);
SqlRDBRead(h, "persons", cond, &out, NULL, NULL);
SqlRDBCondFree(cond);
SqlRDBClose(h);
```

## 主な特徴

- **構造体ベースAPI**: 構造体を一度登録すれば、以降は構造体ポインタの受け渡しだけでCRUD操作
- **SQLインジェクション構造的排除**: 全ての値はプリペアドステートメントへバインド
- **型安全な検索条件**: AND/ORで合成可能な`SqlRDBCondition`ツリー
- **UPSERT / トランザクション / SAVEPOINTネスト**: 16段までの明示的TX、`auto_migrate`によるカラム追加
- **可変長BLOB補助API**: 構造体一括CRUDとは別に`SqlRDBWriteBlobField`/`ReadBlobField`を提供
- **スレッドセーフモード**: 同一ハンドル共有時は内部mutexで直列化、複数ハンドル並行も可
- **バックエンド差し替え可能**: 関数ポインタテーブルによるドライバ抽象（初期実装はSQLite3）
- **品質保証**: AddressSanitizer/UndefinedBehaviorSanitizerクリーン、clang-tidy重大度Highゼロ

## ビルド

### 必要要件

- C11準拠コンパイラ（gcc 9+ / clang 10+ / msvc 2019+）
- CMake 3.20+
- SQLite3 3.35.0以上（UPSERT・STRICTテーブル使用のため、実用上3.37+推奨）
- POSIX環境ではpthread

### 標準ビルド

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

成果物は`build/src/libc2sql.a`。公開ヘッダは`include/c2sql.h`のみ。

### PostgreSQL バックエンド（任意）

既定では SQLite3 のみでビルドする。libpq を用いた PostgreSQL ドライバを
有効化するには `libpq-dev` を入れて `-DC2SQL_WITH_POSTGRES=ON` を渡す:

```sh
cmake -S . -B build -DC2SQL_WITH_POSTGRES=ON
cmake --build build
```

利用側は接続文字列のスキームでバックエンドが切り替わる（API は不変）:

```c
SqlRDBHandle *h = SqlRDBInit("postgresql://user:pass@localhost:5432/mydb");
/* "postgres://" も可。それ以外は SQLite パス/URI として扱う */
```

統合テスト（`tests/test_pg.c`）は環境変数 `C2SQL_PG_DSN` が設定されたときのみ
実行され、未設定ならスキップされる。

### サニタイザビルド

ASan + UBSan で全テストを実行する場合:

```sh
cmake -S . -B build-asan -DC2SQL_SANITIZE=ON
cmake --build build-asan
ctest --test-dir build-asan
```

### 静的解析

clang-tidyおよびcppcheck（インストール済みの場合）:

```sh
cmake --build build --target tidy
cmake --build build --target cppcheck
```

## サンプル

`examples/`配下に3つの動作例。`make examples` または `ctest` で自動実行される。

| サンプル | 主題 |
|--|--|
| `examples/basic_crud.c` | Init→Register→Write/Read/Delete の3ステップAPI |
| `examples/transactions.c` | Begin/Commit/Rollback と SAVEPOINT ネスト |
| `examples/multi_row.c` | `ReadMany`イテレータ、可変長BLOB読み書き |

## ドキュメント

- [設計書](docs/design.md) — アーキテクチャ・コンポーネント境界・データモデル
- [API利用者向け取扱説明書](docs/api-guide.md) — 公開APIの使い方・エラーハンドリング・典型パターン
- [DBメンテナ向け取扱説明書](docs/maintenance-guide.md) — テーブル構造・マイグレーション運用・トラブルシュート
- [ユーザーストーリー](docs/ユーザーストーリー.md) — 利用者⇄管理者の運用モデル・受け渡し様式・バッチ化スコープ
- [改善提案](docs/改善提案.md) — 管理者バッチ（コード生成）によるライブラリ払い出しへの拡張ロードマップ

## ステータス

初期版（v0.1.0）。要件1〜15を満たし、全テスト合格・サニタイザ/静的解析クリーン。ABI互換性はメジャー0系のため変更されうる。

## ライセンス

未定（プロジェクト方針確定後に追記）。

## リポジトリ構成

```
include/         公開ヘッダ (c2sql.h のみ)
src/             ライブラリ本体
tests/           単体・統合・並行・性能テスト
examples/        サンプルプログラム
tools/           c2sql-gen（スキーマ仕様書→定義コード生成バッチ）
specs/           受け渡しスキーマ仕様書（JSON）
generated/       c2sql-gen の生成物（再生成可能・DO NOT EDIT）
docs/            設計書および取扱説明書
.kiro/specs/     仕様（要件・設計・タスク）
.kiro/steering/  プロジェクトコンテキスト
```

## コード生成バッチ（c2sql-gen）

利用者から受け取ったスキーマ仕様書（JSON）を、手書き `SqlRDBColumnDef[]` を
排した定義コードへ変換する管理者向けバッチ。詳細は[tools/README.md](tools/README.md)。

```sh
tools/build_batch.sh            # specs/*.json を lint→生成→build→test
```

生成物を使う利用者コード例は `examples/generated_crud.c`。
