#ifndef _READER_H_
#define _READER_H_

#include "types.h"
#include "process.h"

/**
 * proc の標準入力バッファ(stdin_buf)から1つの完全なS式を読み取り、読取カーソル(read_pos)を進める。
 * 読み取れるものが無い場合は nil を返し、バッファをクリアして次の行の入力を待つ。
 * 構文エラー(閉じカッコ不足・文字列リテラル未終端・先頭の余分な ')' など)は g_sym_read_error を返す。
 * @param proc 読み取り対象のプロセス
 * @return 読み取ったS式。読めるものが無ければnil、構文エラーならg_sym_read_error
 */
lisp_val_t os_read(process_t *proc);

/**
 * proc の入力バッファが尽きた際に、次の行の入力(Enterによるready確定)を待つ。
 * カーネル実行時は割り込み経由でバッファが進み ready が立つまでブロックする(interrupt.cで定義)。
 * ユニットテストでは実際の割り込みが発生しないため、テストファイル側でこの関数を差し替える。
 * @param proc 入力待ちするプロセス
 */
void os_wait_for_more_input(process_t *proc);

#endif /* _READER_H_ */
