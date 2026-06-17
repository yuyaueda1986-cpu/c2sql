# tools/ — 管理者バッチ（c2sql-gen）

利用者から受け取った**スキーマ仕様書（JSON）**を、手書き `SqlRDBColumnDef[]`
を排した定義コードへ変換する外部ツール群。運用モデルの全体像は
[docs/ユーザーストーリー.md](../docs/ユーザーストーリー.md)、設計判断は
[docs/改善提案.md](../docs/改善提案.md) を参照。

## 構成

| ファイル | 役割 |
|--|--|
| `spec.schema.json` | 仕様書の JSON Schema（draft-07） |
| `c2sql_spec.py` | 仕様の読み込み・検証・名前導出（共有モジュール） |
| `c2sql_lint.py` | 仕様検証 CLI |
| `c2sql_gen.py` | 仕様 → `*_schema.h` / `*_schema.c` / `*.cmake` 生成 CLI |
| `build_batch.sh` | lint → gen → build → ctest の一括実行 |

依存は Python 3 標準ライブラリのみ（外部パッケージ不要）。

## 使い方

```sh
# 1) 仕様を検証
python3 tools/c2sql_lint.py specs/persons.json

# 2) 定義コードを生成（generated/ へ）
python3 tools/c2sql_gen.py specs/persons.json -o generated

# 3) すべてをまとめて（lint→gen→build→test）
tools/build_batch.sh
```

## 仕様書フォーマット

最小例（`specs/persons.json`）:

```json
{
  "schema_version": "1.0",
  "struct_name": "persons",
  "c_struct": "Person",
  "backend": "sqlite3",
  "max_records": 100000,
  "fields": [
    { "name": "id",    "type": "int32", "primary_key": true },
    { "name": "name",  "type": "text",  "size": 32 },
    { "name": "score", "type": "real",  "nullable": true }
  ]
}
```

- `offset` と数値型の `size` は書かない（生成器が `offsetof` / `sizeof` で補完）。
- `text` / `blob` は `size`（バッファ長）が必須。
- `primary_key` は最大1列。`c_struct` 省略時は `struct_name` の PascalCase。
- `max_records` は Phase 1 ではメタデータ（`*_MAX_RECORDS` マクロ）。

## 命名規約（生成される識別子）

`struct_name="persons"`, `c_struct="Person"` の場合:

| 生成物 | 名前 |
|--|--|
| C構造体型 | `Person` |
| 列定義配列 | `PERSONS_COLS` |
| 列数マクロ | `PERSONS_COL_COUNT` |
| 最大件数マクロ | `PERSONS_MAX_RECORDS` |
| 登録ヘルパ | `RegisterPersons()` |
| CMakeターゲット | `persons_schema` |

生成物は `DO NOT EDIT` ヘッダ付き。スキーマを変えるときは仕様書を編集して
再生成する（生成物を直接編集しない）。
