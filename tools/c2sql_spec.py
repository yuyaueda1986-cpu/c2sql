"""c2sql_spec — スキーマ仕様書の読み込み・検証・名前導出。

tools/c2sql_lint.py と tools/c2sql_gen.py が共有する。Python標準ライブラリ
のみに依存（外部パッケージ不要）。仕様フォーマットは tools/spec.schema.json。
"""
import json
import re

IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
MAX_IDENT_LEN = 128
MAX_COLUMNS = 64  # libc2sql 本体の上限（SQL_RDB_ERR_TOO_MANY_COLUMNS）

VALID_TYPES = {"int32", "int64", "real", "text", "blob"}
VAR_SIZED_TYPES = {"text", "blob"}  # size 必須
SCALAR_TYPES = {"int32", "int64", "real"}  # size を取らない
SUPPORTED_BACKENDS = {"sqlite3"}  # postgres は将来

# libc2sql の識別子検証が拒否する SQL 予約語の代表例（lint で早期検出する）。
RESERVED_WORDS = {
    "select", "insert", "update", "delete", "from", "where", "table",
    "index", "key", "primary", "unique", "rows", "row", "order", "group",
    "by", "join", "into", "values", "create", "drop", "alter", "and", "or",
    "not", "null", "default", "check", "foreign", "references", "begin",
    "commit", "rollback", "savepoint", "transaction",
}

TYPE_INFO = {
    # type   -> (c_scalar_type, array_base, sql_type)
    "int32": ("int32_t", None, "SQL_TYPE_INT32"),
    "int64": ("int64_t", None, "SQL_TYPE_INT64"),
    "real":  ("double", None, "SQL_TYPE_REAL"),
    "text":  (None, "char", "SQL_TYPE_TEXT"),
    "blob":  (None, "uint8_t", "SQL_TYPE_BLOB"),
}


class SpecError(Exception):
    """検証エラーの集約。`errors` に人間可読なメッセージのリストを持つ。"""

    def __init__(self, errors):
        self.errors = errors
        super().__init__("; ".join(errors))


def load_spec(path):
    """JSONを読み込んで dict を返す。構文エラーは SpecError。"""
    try:
        with open(path, "r", encoding="utf-8") as fp:
            return json.load(fp)
    except json.JSONDecodeError as exc:
        raise SpecError([f"JSON構文エラー: {exc}"])
    except OSError as exc:
        raise SpecError([f"ファイル読み込みエラー: {exc}"])


def _check_ident(value, label, errors):
    if not isinstance(value, str) or not IDENT_RE.match(value):
        errors.append(f"{label}: 識別子規約違反（英字/_で始まり英数字/_のみ）: {value!r}")
        return
    if len(value) > MAX_IDENT_LEN:
        errors.append(f"{label}: 長すぎます（最大{MAX_IDENT_LEN}）: {value!r}")
    if value.lower() in RESERVED_WORDS:
        errors.append(f"{label}: SQL予約語は使用できません: {value!r}")


def validate(spec):
    """仕様 dict を検証。問題があれば SpecError を送出。"""
    errors = []

    if not isinstance(spec, dict):
        raise SpecError(["仕様のトップレベルはオブジェクトである必要があります"])

    if spec.get("schema_version") != "1.0":
        errors.append("schema_version は \"1.0\" を指定してください")

    _check_ident(spec.get("struct_name"), "struct_name", errors)

    if "c_struct" in spec:
        _check_ident(spec.get("c_struct"), "c_struct", errors)

    backend = spec.get("backend")
    if backend not in SUPPORTED_BACKENDS:
        errors.append(
            f"backend は {sorted(SUPPORTED_BACKENDS)} のいずれか（postgres は将来対応）: {backend!r}")

    max_records = spec.get("max_records")
    if not isinstance(max_records, int) or isinstance(max_records, bool) or max_records < 1:
        errors.append(f"max_records は 1 以上の整数を指定してください: {max_records!r}")

    fields = spec.get("fields")
    if not isinstance(fields, list) or not fields:
        errors.append("fields は1要素以上の配列である必要があります")
        raise SpecError(errors)

    if len(fields) > MAX_COLUMNS:
        errors.append(f"カラム数が上限{MAX_COLUMNS}を超えています: {len(fields)}")

    seen = set()
    pk_count = 0
    for i, f in enumerate(fields):
        loc = f"fields[{i}]"
        if not isinstance(f, dict):
            errors.append(f"{loc}: オブジェクトである必要があります")
            continue

        name = f.get("name")
        _check_ident(name, f"{loc}.name", errors)
        if isinstance(name, str):
            if name in seen:
                errors.append(f"{loc}.name: カラム名が重複しています: {name!r}")
            seen.add(name)

        ftype = f.get("type")
        if ftype not in VALID_TYPES:
            errors.append(f"{loc}.type: {sorted(VALID_TYPES)} のいずれか: {ftype!r}")
        else:
            if ftype in VAR_SIZED_TYPES:
                size = f.get("size")
                if not isinstance(size, int) or isinstance(size, bool) or size < 1:
                    errors.append(f"{loc}: type={ftype} は size>=1 が必須です: {f.get('size')!r}")
            elif ftype in SCALAR_TYPES and "size" in f:
                errors.append(f"{loc}: type={ftype} に size は指定できません")

        if f.get("primary_key"):
            pk_count += 1
            if f.get("nullable"):
                errors.append(f"{loc}: primary_key 列は nullable にできません")

    if pk_count > 1:
        errors.append(f"primary_key は最大1列です（{pk_count}列指定）")

    if errors:
        raise SpecError(errors)


def _pascal(snake):
    return "".join(part.capitalize() for part in snake.split("_") if part)


def derive(spec):
    """検証済み仕様から、生成に使う派生名の dict を返す。"""
    struct_name = spec["struct_name"]
    c_struct = spec.get("c_struct") or _pascal(struct_name)
    macro_prefix = struct_name.upper()

    cols = []
    for f in spec["fields"]:
        c_scalar, arr_base, sql_type = TYPE_INFO[f["type"]]
        flags = []
        if f.get("primary_key"):
            flags.append("SQL_COL_FLAG_PRIMARY_KEY")
        if f.get("nullable"):
            flags.append("SQL_COL_FLAG_NULLABLE")
        if f.get("unique"):
            flags.append("SQL_COL_FLAG_UNIQUE")
        cols.append({
            "name": f["name"],
            "sql_type": sql_type,
            "c_scalar": c_scalar,       # None for array types
            "arr_base": arr_base,       # None for scalar types
            "size": f.get("size"),      # set for text/blob
            "flags": " | ".join(flags) if flags else "SQL_COL_FLAG_NONE",
        })

    return {
        "table": struct_name,
        "c_struct": c_struct,
        "macro_prefix": macro_prefix,
        "register_fn": "Register" + _pascal(struct_name),
        "max_records": spec["max_records"],
        "col_count": len(spec["fields"]),
        "cols": cols,
    }
