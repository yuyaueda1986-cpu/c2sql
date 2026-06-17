#!/usr/bin/env bash
#
# build_batch.sh — 管理者バッチ: lint -> gen -> build -> test を一括実行。
#
# 使い方:
#   tools/build_batch.sh                 # specs/*.json を処理
#   tools/build_batch.sh specs/foo.json  # 個別指定
#
# 1件でも lint に失敗したら払い出しをブロックする。生成物は generated/ に出力。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

OUT="generated"
BUILD="build"

# 引数があればそれを、無ければ specs/*.json を対象にする。
if [ "$#" -gt 0 ]; then
    SPECS=("$@")
else
    shopt -s nullglob
    SPECS=(specs/*.json)
    shopt -u nullglob
fi

if [ "${#SPECS[@]}" -eq 0 ]; then
    echo "no spec files found (specs/*.json)" >&2
    exit 1
fi

mkdir -p "$OUT"

for spec in "${SPECS[@]}"; do
    echo "== lint  $spec"
    python3 tools/c2sql_lint.py "$spec"
    echo "== gen   $spec"
    python3 tools/c2sql_gen.py "$spec" -o "$OUT"
done

echo "== build"
cmake -S . -B "$BUILD" >/dev/null
cmake --build "$BUILD"

echo "== test"
ctest --test-dir "$BUILD" --output-on-failure

echo "== OK (${#SPECS[@]} spec(s) processed)"
