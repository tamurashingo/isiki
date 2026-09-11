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
# ケースごとに起動を分け、必ずboot-onlyとの差分を取る(本書で確立した標準手順)。
# C版はAOT版より桁違いに軽く、差分がboot時のゆらぎ(±100万命令程度)に
# 埋もれるため、C版だけNを10倍にして計測し、最後に1単位あたりへ正規化する。
#
# 使い方: make test-qemu-construct-bench [BENCH_N_C=...] [BENCH_N_AOT=...]

set -eu

N_C="${BENCH_N_C:-10000000}"
N_AOT="${BENCH_N_AOT:-1000000}"
CASES="${BENCH_CASES:-loop arith tailrec nontailrec branch let cons for vector funcall}"

# カテゴリごとのN(AOT版)の上書き。構文によって1反復あたりのコストが3桁近く違うため、
# 全カテゴリを同じNで測ると重いものが現実的な時間で終わらない(実測: letをN=1,000,000で
# 測ったところ、他カテゴリが1起動2分程度なのに対し33分経っても完走しなかった)。
# 原因は、letが1回評価されるごとにos_make_lifted_closureがza_fn_meta_tを
# Immobilized Space(GC非対象・解放されないバンプ確保)へリークさせること
# (documents/performance-measurement.md「letのImmobilized Spaceリーク」節)。
# 1単位あたりへ正規化してから比を出すので、Nが違っても比較は成立する。
#
# さらに重要な制約: letとforは1反復ごとにImmobilized Space(4MB固定、GC非対象)を
# 消費し、使い切るとos_imm_page_allocがOSを無限ループで停止させる。実測で
# letは160 byte/反復・forは28.7 byte/反復を消費するため、空き約3.89MBに対し
# letは約24,300反復・forは約135,000反復でハングする。そのためNはこの上限より
# 十分小さい値に抑える必要がある(この制約自体が本ベンチマークの最大の発見)。
aot_n_for() {
    case "$1" in
        let)  echo 10000 ;;
        for)  echo 50000 ;;
        cons) echo 100000 ;;
        *)    echo "$N_AOT" ;;
    esac
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

echo "=== 構文別ベンチマーク: N(C版)=$N_C N(AOT版)=$N_AOT ==="
echo "--- boot-only ベースラインを計測中 ---"
BOOT=$(run_case "")
echo "boot-only total_insns=$BOOT"

: > "$RESULT_TSV"
for c in $CASES; do
    n_aot_c=$(aot_n_for "$c")
    echo "--- $c (C版, N=$N_C) ---"
    raw_c=$(run_case "(%%bench-c-$c $N_C)")
    echo "--- $c (AOT版, N=$n_aot_c) ---"
    raw_aot=$(run_case "(%%bench-aot-$c $n_aot_c)")
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$c" "$raw_c" "$raw_aot" "$BOOT" "$N_C" "$n_aot_c" >> "$RESULT_TSV"
    echo "$c: C raw=$raw_c AOT raw=$raw_aot"
done

python3 - "$RESULT_TSV" << 'PYEOF'
import sys
rows = []
for line in open(sys.argv[1]):
    name, raw_c, raw_aot, boot, n_c, n_aot = line.split()
    per_c = (int(raw_c) - int(boot)) / int(n_c)
    per_aot = (int(raw_aot) - int(boot)) / int(n_aot)
    rows.append((name, per_c, per_aot, per_aot / per_c if per_c > 0 else float('inf')))
rows.sort(key=lambda r: -r[3])
print()
print("| カテゴリ | C版(命令/単位) | AOT版(命令/単位) | 比(AOT/C) |")
print("|---|---|---|---|")
for name, pc, pa, ratio in rows:
    print(f"| {name} | {pc:.2f} | {pa:.2f} | {ratio:.1f}x |")
PYEOF
