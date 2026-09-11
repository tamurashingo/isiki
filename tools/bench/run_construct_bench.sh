#!/bin/bash
# [性能測定] 構文別ベンチマークスイートの実行ドライバ
# (documents/performance-measurement.md「構文別ベンチマークスイート」節)。
#
# src/c/bench_subprimitive.c の %%BENCH-C-* と src/lisp/bench_aot.lisp の
# %%bench-aot-* を、同じ命令数計測基盤(TCGプラグイン)で1ケースずつ別々に
# QEMU起動して測り、boot-onlyベースラインとの差分から「仕事1単位あたりの
# 命令数」を求めて比(AOT版 ÷ C版)を出す。
#
# 1回のQEMU起動で得られるのはtotal_insns(ブート全体の合計)だけなので、
# ケースごとに起動を分ける。
#
# [性能測定] Phase4 第0部: boot-onlyとの差分ではなく**傾き法**を使う。
# 同じベンチマークをNと3Nの2点で測り、差を2Nで割って1単位あたりの命令数を得る。
# bootのコストは両方に等しく含まれるので引き算で完全に相殺され、boot時のゆらぎ
# (実測で±数百万命令)が結果に混入しない。単一Nとboot-onlyの差分で測っていた
# 間は、Nが小さいカテゴリで信号がゆらぎに埋もれ、同一実装で17%違う値が出たり
# 「consが半減した」という誤った結論を出したりしていた。
# この方法なら「適正Nをカテゴリごとに決める」問題自体が消える(固定コストは
# 切片に入り傾きには乗らない)。

set -eu

N_C="${BENCH_N_C:-10000000}"
N_AOT="${BENCH_N_AOT:-1000000}"
CASES="${BENCH_CASES:-loop arith tailrec nontailrec branch let cons for vector funcall}"

# [性能測定] Phase1以前は、letとforが1反復ごとにImmobilized Space(4MB固定・
# GC非対象)を消費し使い切るとOSが停止したため、これらのカテゴリだけNを
# 10,000〜100,000に抑えていた。Phase1(za_fn_meta_tのfnptr単位一意化)で
# 実行回数比例の消費が0になったため、この回避策は不要になった。
# むしろ小さいNはboot時のゆらぎ(実測で約3,700万命令)に信号が埋もれ、
# 測定値が信用できなくなる(letをN=10,000で測っていた間、同一実装で
# 643と531という17%も違う値が出ていた)。全カテゴリで同じNを使う。
aot_n_for() {
    echo "$N_AOT"
}
MILESTONE="tmp/bench_construct_milestone.lisp"
RESULT_TSV="tmp/bench_construct_results.tsv"

mkdir -p tmp

# $1: 呼び出すLisp式(空ならboot-onlyベースライン) -> total_insnsを標準出力へ
run_case() {
    local call_form="$1"
    {
        echo '(load "src/lisp/init.lisp")'
        echo '(load "test/lisp/test_framework.lisp")'
        if [ -n "$call_form" ]; then
            echo "(defglobal bench-result $call_form)"
            echo '(format *isiki-test-stream* "[bench] result=~A~%" bench-result)'
        fi
        echo '(isiki-test-report)'
        echo '(close *isiki-test-stream*)'
    } > "$MILESTONE"

    local out
    out=$(make test-qemu-instcount MILESTONE="$MILESTONE" 2>&1)
    local insns
    insns=$(echo "$out" | sed -n 's/.*\[isiki_instcount\] total_insns=\([0-9]*\).*/\1/p' | tail -1)
    if [ -z "$insns" ]; then
        echo "ERROR: total_insns を取得できませんでした (call: $call_form)" >&2
        echo "$out" | tail -20 >&2
        exit 1
    fi
    echo "$insns"
}

echo "=== 構文別ベンチマーク(傾き法): N(C版)=$N_C/${N_C}x3  N(AOT版)=$N_AOT/${N_AOT}x3 ==="

: > "$RESULT_TSV"
for c in $CASES; do
    n_aot_c=$(aot_n_for "$c")
    echo "--- $c ---"
    c_lo=$(run_case "(%%bench-c-$c $N_C)")
    c_hi=$(run_case "(%%bench-c-$c $((N_C * 3)))")
    a_lo=$(run_case "(%%bench-aot-$c $n_aot_c)")
    a_hi=$(run_case "(%%bench-aot-$c $((n_aot_c * 3)))")
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$c" "$c_lo" "$c_hi" "$a_lo" "$a_hi" "$N_C" "$n_aot_c" >> "$RESULT_TSV"
    echo "$c: C $c_lo -> $c_hi / AOT $a_lo -> $a_hi"
done

python3 - "$RESULT_TSV" << 'PYEOF2'
import sys
rows = []
for line in open(sys.argv[1]):
    name, c_lo, c_hi, a_lo, a_hi, n_c, n_aot = line.split()
    per_c = (int(c_hi) - int(c_lo)) / (2 * int(n_c))
    per_aot = (int(a_hi) - int(a_lo)) / (2 * int(n_aot))
    rows.append((name, per_c, per_aot, per_aot / per_c if per_c > 0 else float('inf')))
rows.sort(key=lambda r: -r[2])
print()
print("| カテゴリ | C版(命令/単位) | AOT版(命令/単位) | 比(AOT/C) |")
print("|---|---|---|---|")
for name, pc, pa, ratio in rows:
    print(f"| {name} | {pc:.2f} | {pa:.2f} | {ratio:.1f}x |")
PYEOF2
