#ifndef _TERMINAL_H_
#define _TERMINAL_H_

#include "runtime.h"

/* [#216] 全画面を使うアプリのための、Lisp から呼べる下位の口。
 *
 * ## なぜ口を足さなければならないのか
 *
 * 生のメモリを読み書きする口(%%PEEK/%%POKE)はあるが、**表示面の在りかを
 * Lisp へ渡す手段がどこにも無い**(frame_buffers は static、記号名から番地を
 * 引く仕組みも無い。調査 refs §7)。だから「0 個では不可能」である。
 *
 * ## どの層に登録するか — ここが着手前に確かめるべき点だった
 *
 * **os_bootstrap の中に足すと、小さいヒープで走らせる試験が固まる。**
 * 通る窓が 5KB 未満しかなく、症状は NG ではなくハングで、
 * SMALL_HEAP_SIZE の掃引し直しが要る(test/c/runtime_test.c:2265-2274)。
 *
 * 小さいヒープの試験(setup_small_heap)が呼ぶのは os_bootstrap() **だけ**で、
 * os_register_* はどれも kernel.c からしか呼ばれない。だから
 * **os_bootstrap の外に登録すれば SMALL_HEAP_SIZE に触らずに済む**(調査 §8-6 の ★)。
 * この推定は #216 の着手時に実物で確かめた: この 5 個を足して
 * `make test` が通る(固まらない)ことが確認である。
 *
 * ## 5 個の内訳(4 つの口)
 *
 *   位置を指定して書く / 画面を消す … %%TERM-SET-CURSOR + %%TERM-CLEAR(2 個)
 *   キーを 1 つ待つ                 … %%TERM-RAW-INPUT(待つのは read-char。下記)
 *   格子の 1 マスを読む             … %%TERM-READ-CELL
 *   画面の行数・桁数を読む          … %%TERM-SIZE
 *
 * **「キーを 1 つ待つ」は新しい関数にしていない。** むき出しの関数にすると
 * *standard-input* の差し替えを通らず、アプリを試験できなくなる
 * (ゲストへキーを送る自動化は無い。調査 §9-15)。素の入力に入れたうえで
 * **既存の read-char で 1 キー取れる**ようにしてある(reader.c の
 * os_process_stdin_read_char に分岐を置いた)。
 *
 * ## 選択子つき 1 個にしなかった理由
 *
 * 調査は「選択子つき 1 個で足りる」としつつ「読みにくく試験しにくい」と
 * 書いている。小ヒープに触らないと確かめられたので、読みやすい側を採った。
 */

/** %%TERM-SIZE … (rows cols) を返す */
lisp_val_t cc_term_size(lisp_val_t args, lisp_val_t env);
/** %%TERM-SET-CURSOR … (row col) へカーソルを置く。範囲外は nil */
lisp_val_t cc_term_set_cursor(lisp_val_t args, lisp_val_t env);
/** %%TERM-CLEAR … いま見えている1画面を格子と物理画面の両方で消す */
lisp_val_t cc_term_clear(lisp_val_t args, lisp_val_t env);
/** %%TERM-READ-CELL … (row col) の格子の1マスを文字で返す。範囲外は nil */
lisp_val_t cc_term_read_cell(lisp_val_t args, lisp_val_t env);
/** %%TERM-RAW-INPUT … 素の入力の入切。切り替える前の状態を返す */
lisp_val_t cc_term_raw_input(lisp_val_t args, lisp_val_t env);

/**
 * 上の 5 個を global_environment へ登録する。
 * **os_bootstrap の外から呼ぶこと**(上の注記の理由)。
 */
void os_register_terminal_primitives(void);

#endif /* _TERMINAL_H_ */
