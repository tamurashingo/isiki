#ifndef _READER_H_
#define _READER_H_

#include "types.h"
#include "process.h"
#include "stream.h"

/**
 * proc の標準入力バッファ(stdin_buf)から1つの完全なS式を読み取り、読取カーソル(read_pos)を進める。
 * 読み取れるものが無い場合は nil を返し、バッファをクリアして次の行の入力を待つ。
 * 構文エラー(閉じカッコ不足・文字列リテラル未終端・先頭の余分な ')' など)は g_sym_read_error を返す。
 * @param proc 読み取り対象のプロセス
 * @return 読み取ったS式。読めるものが無ければnil、構文エラーならg_sym_read_error
 */
lisp_val_t os_read(process_t *proc);

/**
 * streamから1つの完全なS式を読み取る。os_readと同じ規約に従う:
 * 読み取れるものが無い(クリーンEOF)場合はnil、構文エラーの場合はg_sym_read_errorを返す。
 * I/Oエラー(stream->error)はクリーンEOFと区別されないため、呼び出し側がnil受け取り後に
 * stream->errorを確認すること。
 * @param stream 読み取り対象のストリーム
 * @return 読み取ったS式。読めるものが無ければnil、構文エラーならg_sym_read_error
 */
lisp_val_t os_read_stream(os_stream_t *stream);

/**
 * strを1つの数値トークンとして読み取る(read_exprと同じ数値字句を受け付ける: 10進整数/浮動小数点数、
 * #b/#o/#x の基数付き整数)。文字列全体を消費しかつ結果が数値(fixnum/bignum/float)であれば
 * その値を返す。そうでなければ<parse-error>をsignalする(:string に元の文字列、
 * :expected-class に(%find-class '<number>))。
 * @param str 解析対象のSTRING
 * @param env 呼び出し時の環境(<parse-error>のsignal-conditionに使う)
 * @return 解析された数値。解析失敗時はos_signal_conditionの戻り値
 *         (通常はハンドラ経由でトップレベルへabortするため到達しない。init.lisp未ロード時はg_sym_eval_error)
 */
lisp_val_t os_parse_number(lisp_val_t str, lisp_val_t env);

/**
 * proc の入力バッファが尽きた際に、次の行の入力(Enterによるready確定)を待つ。
 * カーネル実行時は割り込み経由でバッファが進み ready が立つまでブロックする(interrupt.cで定義)。
 * ユニットテストでは実際の割り込みが発生しないため、テストファイル側でこの関数を差し替える。
 * @param proc 入力待ちするプロセス
 */
void os_wait_for_more_input(process_t *proc);

#endif /* _READER_H_ */
