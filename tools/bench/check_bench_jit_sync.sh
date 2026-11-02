#!/bin/bash
# [性能測定] src/lisp/bench_jit.lisp と src/lisp/bench_interp.lisp が
# src/lisp/bench_aot.lisp から生成したものと一致していることを固定する
# (documents/performance-measurement.md「JIT 経路の基準値」節・
#  「解釈実行の経路の基準値」節)。
#
# ベンチ本体の S 式が経路ごとに食い違うと、経路の命令数を比較した数字に
# 意味が無くなる。片方だけ直される事故を防ぐため、生成をやり直して diff を取る。
# 差があれば gen_bench_jit.py を実行してコミットする。
#
# 3 つ見る:
#   1. bench_jit.lisp    が真実源から生成したものと一致
#   2. bench_interp.lisp が真実源から生成したものと一致
#   3. **陽性対照の内側の let* が真実源からの逐語の写しであること。**
#      test/lisp/bench_interp_control.lisp の %%bench-interp-loopcost は
#      **手書き**で、「%%bench-interp-loop の本体 + %%bench-aot-let の内側の
#      let* を 1 つ」という形に意味がある(足した費用が
#      interp/let - interp/arith と一致することを別の形の証拠にする)。
#      手書きなので真実源が動いたときに静かにずれる。1 行の grep で固定する。
set -eu

tmp_jit=$(mktemp)
tmp_interp=$(mktemp)
trap 'rm -f "$tmp_jit" "$tmp_interp"' EXIT

python3 tools/bench/gen_bench_jit.py --target jit    "$tmp_jit"    > /dev/null
python3 tools/bench/gen_bench_jit.py --target interp "$tmp_interp" > /dev/null

rc=0

if ! diff -u src/lisp/bench_jit.lisp "$tmp_jit"; then
    cat >&2 <<'MSG'

ERROR: src/lisp/bench_jit.lisp が src/lisp/bench_aot.lisp と同期していません。
       直し方: python3 tools/bench/gen_bench_jit.py && git add src/lisp/bench_jit.lisp
       (bench_jit.lisp を手で編集してはいけません。真実源は bench_aot.lisp です)
MSG
    rc=1
fi

if ! diff -u src/lisp/bench_interp.lisp "$tmp_interp"; then
    cat >&2 <<'MSG'

ERROR: src/lisp/bench_interp.lisp が src/lisp/bench_aot.lisp と同期していません。
       直し方: python3 tools/bench/gen_bench_jit.py && git add src/lisp/bench_interp.lisp
       (bench_interp.lisp を手で編集してはいけません。真実源は bench_aot.lisp です)
MSG
    rc=1
fi

# 真実源の %%bench-aot-let の内側の let*(1 行で書かれている)。
# 陽性対照はこの 1 行を逐語で持っていなければならない
CONTROL_FORM='(let* ((a i) (b (+ a 1)) (c (+ b 1)) (d (+ c 1)) (e (+ d 1)))'
CONTROL_FILE='test/lisp/bench_interp_control.lisp'

if ! grep -qF -- "$CONTROL_FORM" src/lisp/bench_aot.lisp; then
    echo "ERROR: 真実源 src/lisp/bench_aot.lisp に陽性対照が写した let* が見つかりません:" >&2
    echo "       $CONTROL_FORM" >&2
    echo "       %%bench-aot-let の内側の let* を書き換えたなら、" >&2
    echo "       $CONTROL_FILE とこのスクリプトの CONTROL_FORM も直すこと。" >&2
    rc=1
elif ! grep -qF -- "$CONTROL_FORM" "$CONTROL_FILE"; then
    echo "ERROR: $CONTROL_FILE の陽性対照が真実源の let* と一致しません:" >&2
    echo "       $CONTROL_FORM" >&2
    echo "       陽性対照が足す費用は「%%bench-aot-let の内側の let* 1 つ分」で" >&2
    echo "       なければならない(interp/let - interp/arith と突き合わせるため)。" >&2
    rc=1
fi

if [ "$rc" -ne 0 ]; then
    exit 1
fi
echo "bench_jit.lisp / bench_interp.lisp は bench_aot.lisp と同期しています(本体の S 式が同一)"
echo "陽性対照 (%%bench-interp-loopcost) の内側の let* も真実源と一致しています"
