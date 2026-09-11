#ifndef _BENCH_SUBPRIMITIVE_H_
#define _BENCH_SUBPRIMITIVE_H_

#include "types.h"

/**
 * [性能測定] 構文別ベンチマークスイート(documents/performance-measurement.md
 * 「構文別ベンチマークスイート」節)のC側参照実装。
 *
 * src/lisp/bench_aot.lisp の %%bench-aot-* 各関数と1対1で対応する「素のC実装」を、
 * Lisp呼び出し規約(評価済み引数のconsリスト+env)を経由せず素のC関数として持ち、
 * それぞれを %%BENCH-C-* という組み込み関数として登録する。両者を同じ命令数計測
 * 基盤(TCGプラグイン+boot-only差分)で測り、比(AOT版 ÷ C版)を構文ごとに比較する
 * ことで、AOTトランスパイラのコード生成がどの構文で特に重くなるかを特定する。
 *
 * 各関数は引数を1つ(FIXNUM N、実行する仕事の単位数)だけ取り、結果をFIXNUMで返す。
 * 戻り値をLisp側へ返すことで、計算結果が使われずに最適化で消えることを防ぐ。
 */
void os_register_bench_subprimitives(void);

#endif /* _BENCH_SUBPRIMITIVE_H_ */
