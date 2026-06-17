#!/usr/bin/env python3
"""c2sql-lint — スキーマ仕様書（JSON）を検証する。

使い方:
    python3 tools/c2sql_lint.py specs/persons.json [more.json ...]

仕様の構文・識別子規約・型・主キー単一・最大件数などを検査し、問題があれば
非ゼロで終了する。管理者バッチ（build_batch.sh）の最初の関門。
"""
import sys

from c2sql_spec import SpecError, load_spec, validate


def lint_one(path):
    try:
        spec = load_spec(path)
        validate(spec)
    except SpecError as exc:
        print(f"NG  {path}", file=sys.stderr)
        for e in exc.errors:
            print(f"    - {e}", file=sys.stderr)
        return False
    print(f"OK  {path}")
    return True


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    ok = True
    for path in argv[1:]:
        ok = lint_one(path) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
