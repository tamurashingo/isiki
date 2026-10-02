#!/bin/bash
# [性能測定] JIT 経路の構文別ベンチマークの実行ドライバ
# (documents/performance-measurement.md「JIT 経路の基準値」節・
#  「インタプリタ落ちの検出器」節)。
#
# これまでの %%bench-aot-* / %%bench-c-* は **AOT 経路しか測っていなかった。**
# defun した関数(型特化・インライン・declaim が効く経路)は、記録された基準値に
# 一度も現れていない。このスクリプトはその穴を埋める。
#
# 測るもの:
#   path=jit  src/lisp/bench_jit.lisp を実行時に load -> defun ->
#             za_try_compile_defun で JIT 化された関数
#   path=aot  カーネルへ埋め込み済みの %%bench-aot-*(同じ S 式)
#
# **測定の前に 2 つのゲートを通す。**どちらも落ちたら測定値を 1 つも出さない。
#
#   ゲート1 %%za-compiled-p が T であること(test/lisp/bench_jit_guard.lisp)
#   ゲート2 **os_eval の実行命令数が N に対して増えないこと**
#
# ゲート2 が要る理由:
#   **%%za-compiled-p が T でも足りなかった。** あれは「この defun が JIT
#   コンパイルされたか」に答えるだけで、「本体が全部ネイティブで走るか」には
#   答えない。反復ごとに os_eval へ落ちていれば、その数字は「JIT の性能」でも
#   「AOT の性能」でもない。
#   TCG プラグインの start=/end= で os_eval 区間の命令数(range_insns)を数え、
#   **N に対する傾きが 0 であること**を条件にする。1 回数えるだけでは
#   固定費か反復ごとかが分からない(ドライバもゲートも os_eval を通るので
#   0 にはならない)。傾きなら迷いが残らない。
#   **カーネルには一切手を入れない。**os_eval に数え上げを足すと、
#   その変更自体が性能を変える。
#
# 傾き法(N と 3N の差を 2N で割る)を使う理由:
#   - ブートの費用(約 11 億命令)が引き算で相殺される
#   - **JIT 版は defun が測定と同じブート内で起きる。**その JIT コンパイルの
#     費用は N に依存しない固定費なので、同じく切片に入って傾きには乗らない
#   - 「適正 N をカテゴリごとに決める」問題が消える
#
# 環境変数:
#   BENCH_N        1 点目の反復回数(既定 1000000。2 点目は自動で 3 倍)
#   BENCH_CASES    測るカテゴリ(既定 6 カテゴリ)
#   BENCH_PATHS    測る経路(既定 "aot jit")
#   BENCH_REPEAT   同じ測定を何回繰り返すか(既定 3)
#   BENCH_ESTIMATOR  記録に使う推定量(min3 / median3 / minend3。既定 min3)
#                  **どれを使っても 3 つとも出力に出る。**推定量を変えたら
#                  記録値は全部無効になる(documents/performance-measurement.md)
#   BENCH_EVAL_TOL os_eval の傾きの許容値(既定 0.5 命令/反復)
#   BENCH_NO_CHECK 1 にすると記録値との照合(検出器)を省く
#   BENCH_NO_EVAL_GATE 1 にすると os_eval のゲートを省く(**陽性対照専用**)

set -eu

N="${BENCH_N:-1000000}"
CASES="${BENCH_CASES:-loop arith tailrec let for funcall}"
PATHS="${BENCH_PATHS:-aot jit}"
REPEAT="${BENCH_REPEAT:-3}"
ESTIMATOR="${BENCH_ESTIMATOR:-min3}"
EVAL_TOL="${BENCH_EVAL_TOL:-0.5}"
BASELINE="${BENCH_BASELINE:-tools/bench/bench_baseline.tsv}"

MILESTONE="tmp/bench_jit_milestone.lisp"
RESULT_TSV="tmp/bench_jit_results.tsv"
NM_FILE="tmp/nm_pass2.txt"

mkdir -p tmp

# アンカー取得・os_eval 区間の決定・再試行・[FLAKE] 記録は
# run_construct_bench.sh と共通(2 箇所に書けば片方だけ直される)
BENCH_FLAKE_LOG="tmp/bench_jit_flakes.txt"
. tools/bench/bench_common.sh

# nontailrec は JIT 経路では測れない。深さ 100 の非末尾再帰が C スタックを
# 溢れさせ、ガードを踏んでプロセスが止まる(報告は -serial stdio にしか出ない
# ので、外からは QEMU の無反応と区別がつかない)。
# documents/jit-unsupported-syntax.md と test/lisp/bench_jit_test.lisp 2b 節を参照
for c in $CASES; do
    if [ "$c" = "nontailrec" ]; then
        echo "ERROR: nontailrec は JIT 経路では測れません(深さ 34 で STACK OVERFLOW)。" >&2
        echo "       documents/jit-unsupported-syntax.md を参照。" >&2
        exit 1
    fi
done

# カテゴリが呼ぶ関数の一覧(JIT 化を確認する対象)。
# **カテゴリごとに、そのカテゴリが実際に呼ぶ関数すべてを並べる。**
guard_fns_for() {
    case "$1" in
        tailrec)  echo "tailrec tailrec-step" ;;
        funcall)  echo "funcall callee" ;;
        # 陽性対照。**呼び先(%%bench-eval-callee)はわざと JIT に乗らない**ので
        # ゲート1 では見ない。ゲート1 を通してゲート2 で落とすのが対照の趣旨
        evalfall) echo "evalfall" ;;
        *)        echo "$1" ;;
    esac
}

# os_eval ゲートの陽性対照は JIT 経路にしか無い(AOT 側に %%bench-aot-evalfall は
# 無い)。間違って aot で呼ぶと「関数が無い」で落ちるだけで対照にならないので、
# ここで止めて理由を出す
for c in $CASES; do
    if [ "$c" = "evalfall" ]; then
        for p in $PATHS; do
            if [ "$p" != "jit" ]; then
                echo "ERROR: evalfall(os_eval ゲートの陽性対照)は JIT 経路専用です。" >&2
                echo "       BENCH_PATHS=jit を付けてください。" >&2
                exit 1
            fi
        done
    fi
done

# $1 の呼び出し式を実行する milestone を書く。path に応じてゲートを挟む。
# **どのブートでも %%DIAG-IMAGE-ANCHOR-PUB を出す。**プラグインへ渡した
# start=/end= がそのブートでも有効であることを、ドライバ側で突き合わせるため
# (UEFI のロード先はディスク構成で変わる。isiki_instcount.c の注意書き)
write_milestone() {
    local p="$1" c="$2" n="$3"
    {
        echo '(load "src/lisp/init.lisp")'
        echo '(load "test/lisp/test_framework.lisp")'
        if [ "$p" = "jit" ]; then
            echo '(load "src/lisp/bench_jit.lisp")'
            echo '(load "test/lisp/bench_jit_guard.lisp")'
            if [ "$c" = "evalfall" ]; then
                echo '(load "test/lisp/bench_jit_eval_control.lisp")'
            fi
            for fn in $(guard_fns_for "$c"); do
                echo "(bench-jit-guard \"%%bench-jit-$fn\" (function %%bench-jit-$fn))"
            done
        fi
        echo '(format *isiki-test-stream* "#anchor ~D~%" (%%diag-image-anchor-pub))'
        echo '(finish-output *isiki-test-stream*)'
        if [ "$p" = "jit" ]; then
            # **ゲート1 を通らなければ測定しない。** 数字を1つも出さない
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
}

# --- os_eval の範囲を決める(ウォームアップも兼ねる) -------------------------
# [性能測定] **ビルド直後の初回実行は命令数が約 118M(≒10%)多い。**
# 捨てないと、最初に測るカテゴリの傾きだけが系統的に狂う
# (0.87 命令/反復という物理的にありえない値を一度記録している)
first_case=$(echo $CASES | awk '{print $1}')
first_path=$(echo $PATHS | awk '{print $1}')
write_milestone "$first_path" "$first_case" "$N"
bench_init_eval_range "$NM_FILE" "$MILESTONE"

echo "=== JIT/AOT 構文別ベンチマーク(傾き法) N=$N / ${N}x3  繰り返し=$REPEAT ==="

: > "$RESULT_TSV"
r=1
while [ "$r" -le "$REPEAT" ]; do
    for c in $CASES; do
        for p in $PATHS; do
            write_milestone "$p" "$c" "$N"
            set -- $(bench_run "$MILESTONE" "path=$p case=$c n=$N");       lo=$1; lo_ev=$2
            write_milestone "$p" "$c" $((N * 3))
            set -- $(bench_run "$MILESTONE" "path=$p case=$c n=$((N * 3))"); hi=$1; hi_ev=$2
            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                   "$p" "$c" "$r" "$N" "$lo" "$hi" "$lo_ev" "$hi_ev" >> "$RESULT_TSV"
            echo "[$r] $c/$p: $lo -> $hi   os_eval $lo_ev -> $hi_ev"
        done
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
