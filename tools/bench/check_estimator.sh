#!/bin/bash
# [性能測定] **推定量の検査。**QEMU を起動しないので毎回 make test で回す。
# documents/performance-measurement.md「推定量を校正点で決めた」節、規則 8。
#
# 固定資料 test/bench/estimator_fixture.tsv は、真の値が分かっている行
# (c/for = 4.000。逆アセンブルで確定)を N=10,000,000 で 5 回測った生データで、
# **1 ブートだけ約 24M 命令低い回**を含む。
#
# 見るのは 2 つ:
#   陰性対照  既定の推定量(median)が真値の許容範囲に入ること
#   **陽性対照  min 系の推定量が落ちること**
#
# 陽性対照が要る理由: 「校正点を置いた」と書くだけでは、
# **校正点が何かを捕まえられることを示していない。**
# PR #90 と PR #94 で検出器自体が壊れていた例が 2 件ある。
set -eu

FIXTURE=test/bench/estimator_fixture.tsv
CHECK="python3 tools/bench/check_bench_baseline.py --results $FIXTURE --no-check"

echo "--- 陰性対照: median は校正点を通る ---"
if ! $CHECK --estimator median > /dev/null 2>&1; then
    echo "ERROR: 既定の推定量 median が校正点(c/for = 4.000)を外しました。" >&2
    $CHECK --estimator median || true
    exit 1
fi
echo "OK"

echo "--- 陽性対照: min 系は校正点を外して落ちる ---"
for est in min-slope slope-of-mins; do
    if $CHECK --estimator "$est" > /dev/null 2>&1; then
        echo "ERROR: 推定量 $est が校正点を通ってしまいました。" >&2
        echo "       **校正点の検査が壊れています。**この固定資料では" >&2
        echo "       $est は 2.78(真値 4.000)になるはずです。" >&2
        $CHECK --estimator "$est" || true
        exit 1
    fi
    echo "OK ($est は落ちた)"
done

echo "--- 参考: 固定資料に対する 5 推定量の値 ---"
$CHECK --estimator median 2>&1 | sed -n '/校正点/,/^$/p'
