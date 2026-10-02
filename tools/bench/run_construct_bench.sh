#!/bin/bash
# [性能測定] 構文別ベンチマークスイートの実行ドライバ
# (documents/performance-measurement.md「構文別ベンチマークスイート」節)。
#
# src/c/bench_subprimitive.c の %%BENCH-C-* と src/lisp/bench_aot.lisp の
# %%bench-aot-* を、同じ命令数計測基盤(TCGプラグイン)で1ケースずつ別々に
# QEMU起動して測り、傾き法で「仕事1単位あたりの命令数」を求めて
# 比(AOT版 ÷ C版)を出す。
#
# **このスクリプトは AOT 経路と C 実装しか測らない。**
# defun した関数(型特化・インライン・declaim が効く経路)は JIT 経路で、
# tools/bench/run_jit_bench.sh が測る。経路を混同すると issue #114 と同じ
# 事故になる(名前にしか書かれていない事実は、記録されていない)。
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
#
# [性能測定] 集計と**記録値からのずれの検出**は tools/bench/
# check_bench_baseline.py が行う(JIT 側のドライバと共通)。
# 記録値は tools/bench/bench_baseline.tsv。帯を超えたら上下どちらでも落ちる。
# BENCH_NO_CHECK=1 で照合を省ける(基準値を取り直すときだけ)。

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

# [性能測定] **ビルド直後の初回実行は命令数が約118M(≒10%)多い。**
# 同一条件の6回測定が1,117.2M〜1,120.4M(ばらつき0.28%)に収まる一方、
# make build直後の1回だけが1,239Mになる(イメージ再生成やFATキャッシュの差と
# 思われる)。これを捨てないと、最初に測るカテゴリの傾きだけが系統的に狂う。
# 一度この人工物を「保護追加のコスト+10.6%」と誤読した実例がある。
echo "--- ウォームアップ(捨てる) ---"
run_case "" > /dev/null

echo "=== 構文別ベンチマーク(傾き法): N(C版)=$N_C/${N_C}x3  N(AOT版)=$N_AOT/${N_AOT}x3 ==="

: > "$RESULT_TSV"
for c in $CASES; do
    n_aot_c=$(aot_n_for "$c")
    echo "--- $c ---"
    c_lo=$(run_case "(%%bench-c-$c $N_C)")
    c_hi=$(run_case "(%%bench-c-$c $((N_C * 3)))")
    a_lo=$(run_case "(%%bench-aot-$c $n_aot_c)")
    a_hi=$(run_case "(%%bench-aot-$c $((n_aot_c * 3)))")
    # [性能測定] 1 行 1 測定の統一形式(経路 カテゴリ 繰り返し N lo hi)で書く。
    # 集計と記録値との照合は tools/bench/check_bench_baseline.py が JIT 側の
    # ドライバ(run_jit_bench.sh)と共通で行う。**数字に経路を持たせること**が
    # issue #114 の再発防止の要点なので、形式を分けない
    printf 'c\t%s\t1\t%s\t%s\t%s\n' "$c" "$N_C" "$c_lo" "$c_hi" >> "$RESULT_TSV"
    printf 'aot\t%s\t1\t%s\t%s\t%s\n' "$c" "$n_aot_c" "$a_lo" "$a_hi" >> "$RESULT_TSV"
    echo "$c: C $c_lo -> $c_hi / AOT $a_lo -> $a_hi"
done

echo
if [ "${BENCH_NO_CHECK:-0}" = "1" ]; then
    python3 tools/bench/check_bench_baseline.py --results "$RESULT_TSV" --no-check
else
    python3 tools/bench/check_bench_baseline.py --results "$RESULT_TSV" \
        --baseline "${BENCH_BASELINE:-tools/bench/bench_baseline.tsv}"
fi
