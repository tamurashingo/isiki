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

/**
 * 現在のプロセスの標準入力バッファから1文字読む(キーボード入力ストリーム
 * STREAM_INPUT_KEYBOARD用)。バッファを使い切っていれば次の行の入力を待つ。
 * @param out_ch 読んだ文字の格納先
 * @return 読めれば1、入力終端(ユニットテスト等でos_wait_for_more_inputが供給しない場合)は0
 */
int os_process_stdin_read_char(char *out_ch);

/**
 * os_read_streamと同じだが、「S式が無いまま入力が終端した」ことをout_eofで区別できる。
 * os_read_streamの戻り値nilは空リスト()を読んだ場合とも一致するため、ISLisp仕様のreadの
 * eos-error-p/eos-value処理にはこちらを使う。
 * readerが先読みして未消費のまま残した1文字は、stream自身には書き戻さない(読み取り中の
 * 割り当てでGCが走るとos_stream_tは別アドレスへ再配置されているため)。代わりに
 * out_has_pending/out_pendingで呼び出し側へ返すので、呼び出し側がMAGIC_STREAMハンドルから
 * 取り直したos_stream_tのpreview-char用先読みスロット(has_lookahead/lookahead)へ戻すこと。
 * @param stream 読み取り対象のストリーム
 * @param out_eof 終端でS式を読めなかった場合に1、それ以外は0を書き込む
 * @param out_has_pending 未消費の先読み文字があれば1、無ければ0を書き込む
 * @param out_pending 未消費の先読み文字の格納先
 * @return 読み取ったS式(終端時はnil)。構文エラーの場合はg_sym_read_error
 */
lisp_val_t os_read_stream_ex(os_stream_t *stream, int *out_eof, int *out_has_pending, char *out_pending);

#endif /* _READER_H_ */
