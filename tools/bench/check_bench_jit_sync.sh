#!/bin/bash
# [性能測定] src/lisp/bench_jit.lisp が src/lisp/bench_aot.lisp から生成した
# ものと一致していることを固定する(documents/performance-measurement.md
# 「JIT 経路の基準値」節)。
#
# ベンチ本体の S 式が AOT 版と JIT 版で食い違うと、2 つの経路の命令数を
# 比較した数字に意味が無くなる。片方だけ直される事故を防ぐため、生成を
# やり直して diff を取る。差があれば gen_bench_jit.py を実行してコミットする。
set -eu

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

python3 tools/bench/gen_bench_jit.py "$tmp" > /dev/null

if ! diff -u src/lisp/bench_jit.lisp "$tmp"; then
    cat >&2 <<'MSG'

ERROR: src/lisp/bench_jit.lisp が src/lisp/bench_aot.lisp と同期していません。
       直し方: python3 tools/bench/gen_bench_jit.py && git add src/lisp/bench_jit.lisp
       (bench_jit.lisp を手で編集してはいけません。真実源は bench_aot.lisp です)
MSG
    exit 1
fi
echo "bench_jit.lisp は bench_aot.lisp と同期しています(本体の S 式が同一)"
