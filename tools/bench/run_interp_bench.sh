#!/bin/bash
# [性能測定] **解釈実行(インタプリタ)経路**の構文別ベンチマークの実行ドライバ
# (documents/performance-measurement.md「解釈実行の経路の基準値」節)。
#
# これまでの run_construct_bench.sh(c / aot)と run_jit_bench.sh(aot / jit)は
# **解釈実行の経路を 1 行も測っていなかった。** したがって解釈実行に費用を
# 足しても、記録された基準値は 1 つも動かない。このスクリプトがその穴を埋める。
# M8-T4/T5 でヒープ枯渇の検査を**評価の入口**に置くと、その費用はここにしか
# 現れない。
#
# 測るもの:
#   path=interp  src/lisp/bench_interp.lisp を実行時に load して得られる
#                (defglobal X (lambda ...)) を funcall する。
#                **lambda は src/c/eval.c の eval_lambda が
#                make_interpreted_function を直接呼ぶだけなので必ず
#                インタプリタになる**(カーネルに JIT を切るつまみは無い。
#                新しいつまみを足すと、そのつまみ自体が測定対象を変える)
#
# **他経路の数字と直接比べてはいけない。**
# 解釈実行は AOT/JIT より 1〜2 桁遅いので、同じ N では測定時間が爆発する。
# したがって N を 1 桁以上小さく取る(既定 100,000 対 1,000,000)。
# **N が違う傾きは比較できない**(check_bench_baseline.py が照合する)。
# この行の用途は「解釈実行の経路の中での前後比較」だけである。
#
# **ゲートは JIT 側の逆向きである。**どちらも落ちたら測定値を 1 つも出さない。
#
#   ゲート1  %%za-compiled-p が **NIL** であること
#            (test/lisp/bench_interp_guard.lisp。JIT 側は T を要求する)
#   ゲート2  **os_eval の実行命令数が N に対して増えること**
#            (check_bench_baseline.py。JIT/AOT 側は 0 を要求する)
#
# ゲート2 を逆向きに読む理由:
#   解釈実行なら、仕事はすべて os_eval の中で起きる。傾きが 0 なら
#   **それは解釈実行ではない**(lambda がコンパイルされた / 本体が走っていない)。
#   同じ計器を逆向きに読むだけで「本当にインタプリタを測っている」ことの
#   証拠になるので、新しい検査を足さない。
#
# 傾き法(N と 3N の差を 2N で割る)を使う理由は JIT 側と同じ:
#   - ブートの費用(約 11 億命令)が引き算で相殺される
#   - load と defglobal の費用は N に依存しないので切片に入る
#   - 「適正 N をカテゴリごとに決める」問題が消える
#
# 環境変数:
#   BENCH_N        1 点目の反復回数(既定 100000。2 点目は自動で 3 倍)
#   BENCH_CASES    測るカテゴリ(既定 "loop arith let")
#   BENCH_REPEAT   同じ測定を何回繰り返すか(既定 3)
#   BENCH_ESTIMATOR  記録に使う推定量。**既定 median**(校正点で決めた。
#                  min 系は校正点を外す。documents/performance-measurement.md)
#   BENCH_EVAL_TOL os_eval の傾きの閾値(既定 0.5 命令/反復)。
#                  **interp ではこれを「超えていること」が条件になる**
#   BENCH_NO_CHECK 1 にすると記録値との照合(検出器)を省く
#   BENCH_NO_EVAL_GATE 1 にすると os_eval のゲートを省く(**陽性対照専用**)
#
# 陽性対照(規則 8。解釈実行に費用を足すと値が動くこと):
#   BENCH_CASES="loop loopcost" BENCH_REPEAT=1 BENCH_NO_CHECK=1 \
#     make test-qemu-interp-bench
#   loopcost は loop に %%bench-aot-let の内側の let* を 1 つ足しただけで
#   戻り値は同じ(test/lisp/bench_interp_control.lisp)。
#   差は interp/let - interp/arith と一致するはずである(別の形の証拠)。

set -eu

N="${BENCH_N:-100000}"
CASES="${BENCH_CASES:-loop arith let}"
REPEAT="${BENCH_REPEAT:-3}"
ESTIMATOR="${BENCH_ESTIMATOR:-median}"
EVAL_TOL="${BENCH_EVAL_TOL:-0.5}"
BASELINE="${BENCH_BASELINE:-tools/bench/bench_baseline.tsv}"

MILESTONE="tmp/bench_interp_milestone.lisp"
RESULT_TSV="tmp/bench_interp_results.tsv"
NM_FILE="tmp/nm_pass2.txt"
INTERP_SRC="src/lisp/bench_interp.lisp"

mkdir -p tmp

# アンカー取得・os_eval 区間の決定・再試行・[FLAKE] 記録は
# run_jit_bench.sh / run_construct_bench.sh と共通
# (2 箇所に書けば片方だけ直される)
BENCH_FLAKE_LOG="tmp/bench_interp_flakes.txt"
. tools/bench/bench_common.sh

# **呼べないカテゴリは生成物の印から導出する**(規則 13: 件数は記録せず導出)。
# bench_interp.lisp は defglobal なので、本体から別のベンチ関数を名前で呼ぶ
# ものは <undefined-function> になる。生成器(tools/bench/gen_bench_jit.py)が
# その印を書き、ここで grep する。**一覧をここに書き写すと腐る。**
if [ ! -f "$INTERP_SRC" ]; then
    echo "ERROR: $INTERP_SRC が無い。python3 tools/bench/gen_bench_jit.py を実行すること" >&2
    exit 1
fi
CONTROL_SRC="test/lisp/bench_interp_control.lisp"
for c in $CASES; do
    if grep -q "^;;; \[呼べない\] %%bench-interp-$c " "$INTERP_SRC"; then
        echo "ERROR: $c は解釈実行の経路では測れません(本体が別のベンチ関数を" >&2
        echo "       名前で呼ぶので、defglobal では関数位置が解決されません)。" >&2
        echo "       印は $INTERP_SRC にあり、生成器が付けています:" >&2
        grep "^;;; \[呼べない\]" "$INTERP_SRC" | sed 's/^/         /' >&2
        exit 1
    fi
    # **名前の打ち間違いをここで止める。** 通してしまうと「未定義変数で
    # ゲストが落ちた」という、測定の失敗と区別しにくい形で現れる
    if ! grep -q "(defglobal %%bench-interp-$c " "$INTERP_SRC" \
       && ! grep -q "(defglobal %%bench-interp-$c$" "$CONTROL_SRC"; then
        echo "ERROR: %%bench-interp-$c が定義されていません。測れるカテゴリ:" >&2
        grep -ho "(defglobal %%bench-interp-[^ )]*" "$INTERP_SRC" "$CONTROL_SRC" \
            | sed 's/.*%%bench-interp-/         /' >&2
        exit 1
    fi
done

# $1 のカテゴリを $2 回実行する milestone を書く。
# **どのブートでも %%DIAG-IMAGE-ANCHOR-PUB を出す。** プラグインへ渡した
# start=/end= がそのブートでも有効であることを突き合わせるため
# (UEFI のロード先はディスク構成で変わる。isiki_instcount.c の注意書き)
write_milestone() {
    local c="$1" n="$2"
    {
        echo '(load "src/lisp/init.lisp")'
        echo '(load "test/lisp/test_framework.lisp")'
        echo '(load "src/lisp/bench_interp.lisp")'
        echo '(load "test/lisp/bench_interp_guard.lisp")'
        # 陽性対照だけは生成物に無い(わざと真実源と違う形なので)
        if [ "$c" = "loopcost" ]; then
            echo '(load "test/lisp/bench_interp_control.lisp")'
        fi
        # **ゲート1: JIT に乗っていないこと。** JIT 側の guard の逆向き
        echo "(bench-interp-guard \"%%bench-interp-$c\" %%bench-interp-$c)"
        echo '(format *isiki-test-stream* "#anchor ~D~%" (%%diag-image-anchor-pub))'
        echo '(finish-output *isiki-test-stream*)'
        # **ゲート1 を通らなければ測定しない。** 数字を 1 つも出さない
        echo '(if (bench-interp-guard-gate)'
        echo "    (progn (defglobal bench-result (funcall %%bench-interp-$c $n))"
        echo "           (format *isiki-test-stream* \"[bench] path=interp case=$c n=$n result=~A~%\" bench-result))"
        echo '  (format *isiki-test-stream* "[bench] 測定しなかった(インタプリタで走っていない)~%"))'
        echo '(isiki-test-report)'
        echo '(close *isiki-test-stream*)'
    } > "$MILESTONE"
}

# --- os_eval の範囲を決める(ウォームアップも兼ねる) -------------------------
# [性能測定] **ビルド直後の初回実行は命令数が約 118M(≒10%)多い。**
# 捨てないと、最初に測るカテゴリの傾きだけが系統的に狂う
# (0.87 命令/反復という物理的にありえない値を一度記録している)
first_case=$(echo $CASES | awk '{print $1}')
write_milestone "$first_case" "$N"
bench_init_eval_range "$NM_FILE" "$MILESTONE"

echo "=== 解釈実行の構文別ベンチマーク(傾き法) N=$N / ${N}x3  繰り返し=$REPEAT  推定量=$ESTIMATOR ==="
echo "    **この値は c / aot / jit の行と直接比べられない(N が 1 桁違う)。**"

: > "$RESULT_TSV"
r=1
while [ "$r" -le "$REPEAT" ]; do
    for c in $CASES; do
        write_milestone "$c" "$N"
        set -- $(bench_run "$MILESTONE" "path=interp case=$c n=$N");         lo=$1; lo_ev=$2
        write_milestone "$c" $((N * 3))
        set -- $(bench_run "$MILESTONE" "path=interp case=$c n=$((N * 3))"); hi=$1; hi_ev=$2
        printf 'interp\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
               "$c" "$r" "$N" "$lo" "$hi" "$lo_ev" "$hi_ev" >> "$RESULT_TSV"
        echo "[$r] $c/interp: $lo -> $hi   os_eval $lo_ev -> $hi_ev"
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
