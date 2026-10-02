#!/bin/bash
# [性能測定] 構文別ベンチマークスイート(C 実装 vs AOT)の実行ドライバ
# (documents/performance-measurement.md「構文別ベンチマークスイート」節)。
#
# src/c/bench_subprimitive.c の %%BENCH-C-* と src/lisp/bench_aot.lisp の
# %%bench-aot-* を、同じ命令数計測基盤(TCG プラグイン)で 1 ケースずつ別々に
# QEMU 起動して測り、傾き法で「仕事 1 単位あたりの命令数」を求める。
#
# **このスクリプトは AOT 経路と C 実装しか測らない。**
# defun した関数(型特化・インライン・declaim が効く経路)は JIT 経路で、
# tools/bench/run_jit_bench.sh が測る。経路を混同すると issue #114 と同じ
# 事故になる(名前にしか書かれていない事実は、記録されていない)。
#
# 1 回の QEMU 起動で得られるのは total_insns(ブート全体の合計)だけなので、
# ケースごとに起動を分ける。
#
# [性能測定] Phase4 第0部: boot-only との差分ではなく**傾き法**を使う。
# 同じベンチマークを N と 3N の 2 点で測り、差を 2N で割って 1 単位あたりの
# 命令数を得る。boot のコストは両方に等しく含まれるので引き算で完全に相殺され、
# boot 時のゆらぎ(実測で±数百万命令)が結果に混入しない。単一 N と boot-only の
# 差分で測っていた間は、N が小さいカテゴリで信号がゆらぎに埋もれ、同一実装で
# 17% 違う値が出たり「cons が半減した」という誤った結論を出したりしていた。
# この方法なら「適正 N をカテゴリごとに決める」問題自体が消える(固定コストは
# 切片に入り傾きには乗らない)。
#
# [性能測定] **os_eval 区間の命令数も同時に数える**(ゲート2)。
# N に対する傾きが 0 でなければ、そのカテゴリは反復ごとにインタプリタへ
# 落ちていて、数字は「AOT の性能」でも「C の性能」でもない。
# documents/performance-measurement.md「インタプリタ落ちの検出器」節。
#
# [性能測定] 集計と**記録値からのずれの検出**は tools/bench/
# check_bench_baseline.py が行う(JIT 側のドライバと共通)。
# 記録値は tools/bench/bench_baseline.tsv。帯を超えたら上下どちらでも落ちる。
# BENCH_NO_CHECK=1 で照合を省ける(基準値を取り直すときだけ)。

set -eu

N_C="${BENCH_N_C:-10000000}"
N_AOT="${BENCH_N_AOT:-1000000}"
CASES="${BENCH_CASES:-loop arith tailrec nontailrec branch let cons for vector funcall}"
REPEAT="${BENCH_REPEAT:-3}"
ESTIMATOR="${BENCH_ESTIMATOR:-min3}"
EVAL_TOL="${BENCH_EVAL_TOL:-0.5}"
BASELINE="${BENCH_BASELINE:-tools/bench/bench_baseline.tsv}"

# [性能測定] Phase1 以前は、let と for が 1 反復ごとに Immobilized Space
# (4MB 固定・GC 非対象)を消費し使い切ると OS が停止したため、これらの
# カテゴリだけ N を 10,000〜100,000 に抑えていた。Phase1(za_fn_meta_t の
# fnptr 単位一意化)で実行回数比例の消費が 0 になったため、この回避策は
# 不要になった。むしろ小さい N は boot 時のゆらぎ(実測で約 3,700 万命令)に
# 信号が埋もれ、測定値が信用できなくなる(let を N=10,000 で測っていた間、
# 同一実装で 643 と 531 という 17% も違う値が出ていた)。
# 全カテゴリで同じ N を使う。
aot_n_for() {
    echo "$N_AOT"
}

MILESTONE="tmp/bench_construct_milestone.lisp"
RESULT_TSV="tmp/bench_construct_results.tsv"
NM_FILE="tmp/nm_pass2.txt"

mkdir -p tmp

# アンカー取得・os_eval 区間の決定・再試行・[FLAKE] 記録は
# run_jit_bench.sh と共通(2 箇所に書けば片方だけ直される)
BENCH_FLAKE_LOG="tmp/bench_construct_flakes.txt"
. tools/bench/bench_common.sh

# $1: 呼び出す Lisp 式(空ならブートだけ)
# **どのブートでも %%DIAG-IMAGE-ANCHOR-PUB を出す。** プラグインへ渡した
# start=/end= がそのブートでも有効であることを突き合わせるため
write_milestone() {
    local call_form="$1"
    {
        echo '(load "src/lisp/init.lisp")'
        echo '(load "test/lisp/test_framework.lisp")'
        echo '(format *isiki-test-stream* "#anchor ~D~%" (%%diag-image-anchor-pub))'
        echo '(finish-output *isiki-test-stream*)'
        if [ -n "$call_form" ]; then
            echo "(defglobal bench-result $call_form)"
            echo '(format *isiki-test-stream* "[bench] result=~A~%" bench-result)'
        fi
        echo '(isiki-test-report)'
        echo '(close *isiki-test-stream*)'
    } > "$MILESTONE"
}

# [性能測定] **ビルド直後の初回実行は命令数が約118M(≒10%)多い。**
# 同一条件の6回測定が1,117.2M〜1,120.4M(ばらつき0.28%)に収まる一方、
# make build直後の1回だけが1,239Mになる(イメージ再生成やFATキャッシュの差と
# 思われる)。これを捨てないと、最初に測るカテゴリの傾きだけが系統的に狂う。
# 一度この人工物を「保護追加のコスト+10.6%」と誤読した実例がある。
write_milestone ""
bench_init_eval_range "$NM_FILE" "$MILESTONE"

echo "=== 構文別ベンチマーク(傾き法): N(C版)=$N_C/${N_C}x3  N(AOT版)=$N_AOT/${N_AOT}x3  繰り返し=$REPEAT ==="

: > "$RESULT_TSV"
r=1
while [ "$r" -le "$REPEAT" ]; do
    for c in $CASES; do
        n_aot_c=$(aot_n_for "$c")
        echo "--- [$r] $c ---"
        # [性能測定] 1 行 1 測定の統一形式
        # (経路 カテゴリ 繰り返し N lo hi lo_eval hi_eval)で書く。
        # 集計と記録値との照合は tools/bench/check_bench_baseline.py が JIT 側の
        # ドライバと共通で行う。**数字に経路を持たせること**が issue #114 の
        # 再発防止の要点なので、形式を分けない
        write_milestone "(%%bench-c-$c $N_C)"
        set -- $(bench_run "$MILESTONE" "path=c case=$c n=$N_C");           c_lo=$1; c_lo_ev=$2
        write_milestone "(%%bench-c-$c $((N_C * 3)))"
        set -- $(bench_run "$MILESTONE" "path=c case=$c n=$((N_C * 3))");   c_hi=$1; c_hi_ev=$2
        write_milestone "(%%bench-aot-$c $n_aot_c)"
        set -- $(bench_run "$MILESTONE" "path=aot case=$c n=$n_aot_c");     a_lo=$1; a_lo_ev=$2
        write_milestone "(%%bench-aot-$c $((n_aot_c * 3)))"
        set -- $(bench_run "$MILESTONE" "path=aot case=$c n=$((n_aot_c * 3))"); a_hi=$1; a_hi_ev=$2
        printf 'c\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n'   "$c" "$r" "$N_C"     "$c_lo" "$c_hi" "$c_lo_ev" "$c_hi_ev" >> "$RESULT_TSV"
        printf 'aot\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$c" "$r" "$n_aot_c" "$a_lo" "$a_hi" "$a_lo_ev" "$a_hi_ev" >> "$RESULT_TSV"
        echo "[$r] $c: C $c_lo -> $c_hi / AOT $a_lo -> $a_hi   os_eval C $c_lo_ev->$c_hi_ev AOT $a_lo_ev->$a_hi_ev"
    done
    r=$((r + 1))
done

echo
bench_report_flakes

gate_opt="--eval-tol $EVAL_TOL"
if [ "${BENCH_NO_EVAL_GATE:-0}" = "1" ]; then
    gate_opt="--no-eval-gate"
fi

if [ "${BENCH_NO_CHECK:-0}" = "1" ]; then
    python3 tools/bench/check_bench_baseline.py --results "$RESULT_TSV" \
        --estimator "$ESTIMATOR" $gate_opt --no-check
else
    python3 tools/bench/check_bench_baseline.py --results "$RESULT_TSV" \
        --estimator "$ESTIMATOR" $gate_opt --baseline "$BASELINE"
fi
