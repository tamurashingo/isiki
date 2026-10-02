#!/bin/bash
# [性能測定] JIT 経路の構文別ベンチマークの実行ドライバ
# (documents/performance-measurement.md「JIT 経路の基準値」節)。
#
# これまでの %%bench-aot-* / %%bench-c-* は **AOT 経路しか測っていなかった。**
# defun した関数(型特化・インライン・declaim が効く経路)は、記録された基準値に
# 一度も現れていない。このスクリプトはその穴を埋める。
#
# 測るもの:
#   path=jit  src/lisp/bench_jit.lisp を実行時に load -> defun ->
#             za_try_compile_defun で JIT 化された関数
#   path=aot  カーネルへ埋め込み済みの %%bench-aot-*(同じ S 式)
# 両者を同じ N・同じ傾き法で測り、差を出す。
#
# **測定の前に %%za-compiled-p を assert する。**T でなければ、そのカテゴリは
# 測定せずにこのスクリプトを落とす(test/lisp/bench_jit_guard.lisp)。
# 警告して続行にはしない: 数字が出てしまえば誰かが使う。
#
# 傾き法(N と 3N の差を 2N で割る)を使う理由:
#   - ブートの費用(約 11 億命令)が引き算で完全に相殺される
#   - **JIT 版は defun が測定と同じブート内で起きる。**その JIT コンパイルの
#     費用は N に依存しない固定費なので、同じく切片に入って傾きには乗らない。
#     boot-only との差分で測ると、この固定費が丸ごと信号に混ざる
#   - 「適正 N をカテゴリごとに決める」問題が消える
#
# 環境変数:
#   BENCH_N       1 点目の反復回数(既定 1000000。2 点目は自動で 3 倍)
#   BENCH_CASES   測るカテゴリ(既定 6 カテゴリ)
#   BENCH_PATHS   測る経路(既定 "aot jit")
#   BENCH_REPEAT  同じ測定を何回繰り返すか(既定 1)。**1 回あたりの分解能を
#                 出すときは 3 以上にする**(ばらつきの実測が分解能になる)
#   BENCH_NO_CHECK 1 にすると記録値との照合(検出器)を省く

set -eu

N="${BENCH_N:-1000000}"
CASES="${BENCH_CASES:-loop arith tailrec let for funcall}"
PATHS="${BENCH_PATHS:-aot jit}"
REPEAT="${BENCH_REPEAT:-1}"
BASELINE="${BENCH_BASELINE:-tools/bench/bench_baseline.tsv}"

MILESTONE="tmp/bench_jit_milestone.lisp"
RESULT_TSV="tmp/bench_jit_results.tsv"

mkdir -p tmp

# nontailrec は JIT 経路では測れない。深さ 100 の非末尾再帰が C スタックを
# 溢れさせ、ガードを踏んでプロセスが止まる(報告は -serial stdio にしか出ない
# ので、外からは QEMU の無反応と区別がつかない)。
# test/lisp/bench_jit_test.lisp の「2b」節に実測がある
for c in $CASES; do
    if [ "$c" = "nontailrec" ]; then
        echo "ERROR: nontailrec は JIT 経路では測れません(深さ 34 で STACK OVERFLOW)。" >&2
        echo "       test/lisp/bench_jit_test.lisp の 2b 節を参照。" >&2
        exit 1
    fi
done

# カテゴリが呼ぶ関数の一覧(JIT 化を確認する対象)。
# **カテゴリごとに、そのカテゴリが実際に呼ぶ関数すべてを並べる。**
# 1 つだけ確認して全部を代表させてはいけない
guard_fns_for() {
    case "$1" in
        tailrec) echo "tailrec tailrec-step" ;;
        funcall) echo "funcall callee" ;;
        *)       echo "$1" ;;
    esac
}

# $1: 経路(aot|jit)  $2: カテゴリ  $3: N  -> total_insns を標準出力へ
run_case() {
    local p="$1" c="$2" n="$3"
    {
        echo '(load "src/lisp/init.lisp")'
        echo '(load "test/lisp/test_framework.lisp")'
        if [ "$p" = "jit" ]; then
            echo '(load "src/lisp/bench_jit.lisp")'
            echo '(load "test/lisp/bench_jit_guard.lisp")'
            for fn in $(guard_fns_for "$c"); do
                echo "(bench-jit-guard \"%%bench-jit-$fn\" (function %%bench-jit-$fn))"
            done
            # **ゲートを通らなければ測定しない。** 数字を1つも出さない
            echo '(if (bench-jit-guard-gate)'
            echo "    (progn (defglobal bench-result (%%bench-jit-$c $n))"
            echo "           (format *isiki-test-stream* \"[bench] path=jit case=$c n=$n result=~A~%\" bench-result))"
            echo '  (format *isiki-test-stream* "[bench] 測定しなかった(JIT に乗っていない)~%"))'
        else
            echo "(defglobal bench-result (%%bench-aot-$c $n))"
            echo "(format *isiki-test-stream* \"[bench] path=aot case=$c n=$n result=~A~%\" bench-result)"
        fi
        echo '(isiki-test-report)'
        echo '(close *isiki-test-stream*)'
    } > "$MILESTONE"

    local out insns
    # make 側が " 0 failed" を grep するので、guard が落ちればここで失敗する
    if ! out=$(make test-qemu-instcount MILESTONE="$MILESTONE" 2>&1); then
        echo "ERROR: 測定が失敗しました (path=$p case=$c n=$n)" >&2
        echo "$out" | tail -25 >&2
        exit 1
    fi
    insns=$(echo "$out" | sed -n 's/.*\[isiki_instcount\] total_insns=\([0-9]*\).*/\1/p' | tail -1)
    if [ -z "$insns" ]; then
        echo "ERROR: total_insns を取得できませんでした (path=$p case=$c n=$n)" >&2
        echo "$out" | tail -25 >&2
        exit 1
    fi
    echo "$insns"
}

# [性能測定] **ビルド直後の初回実行は命令数が約 118M(≒10%)多い。**
# 捨てないと、最初に測るカテゴリの傾きだけが系統的に狂う
# (0.87 命令/反復という物理的にありえない値を一度記録している)
echo "--- ウォームアップ(捨てる) ---"
run_case aot loop "$N" > /dev/null

echo "=== JIT/AOT 構文別ベンチマーク(傾き法) N=$N / ${N}x3  繰り返し=$REPEAT ==="

: > "$RESULT_TSV"
r=1
while [ "$r" -le "$REPEAT" ]; do
    for c in $CASES; do
        for p in $PATHS; do
            lo=$(run_case "$p" "$c" "$N")
            hi=$(run_case "$p" "$c" $((N * 3)))
            printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$p" "$c" "$r" "$N" "$lo" "$hi" >> "$RESULT_TSV"
            echo "[$r] $c/$p: $lo -> $hi"
        done
    done
    r=$((r + 1))
done

echo
if [ "${BENCH_NO_CHECK:-0}" = "1" ]; then
    python3 tools/bench/check_bench_baseline.py --results "$RESULT_TSV" --no-check
else
    python3 tools/bench/check_bench_baseline.py --results "$RESULT_TSV" --baseline "$BASELINE"
fi
