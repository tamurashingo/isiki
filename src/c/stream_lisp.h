#ifndef _STREAM_LISP_H_
#define _STREAM_LISP_H_

#include "types.h"
#include "stream.h"

/**
 * os_stream_t(生のCポインタ)をMAGIC_STREAMのTAG_INSTANCEでラップする。
 * @param raw ラップ対象のストリーム(Lispヒープ上に確保済みであること)
 * @return タグ付けされたINSTANCE(STREAM)
 */
lisp_val_t os_make_stream(os_stream_t *raw);

/**
 * 組み込み関数OPEN-INPUT-STREAM。第一引数のパスの9Pファイルをopenしたstreamを返す。
 * @param args (path) pathはSTRING(9Pエクスポートルートから見た相対パス)
 * @param env 呼び出し時の環境(未使用)
 * @return 成功時STREAM、失敗時g_sym_eval_error
 */
lisp_val_t cc_open_input_stream(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数OPEN-OUTPUT-STREAM。現在実行中プロセスの画面(stdout_buffer)への
 * 出力専用streamを返す(9P書き込み・シリアル出力は未サポートのため引数は無視する)。
 * @param args 未使用
 * @param env 呼び出し時の環境(未使用)
 * @return 画面出力用のSTREAM
 */
lisp_val_t cc_open_output_stream(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数CLOSE。第一引数のstreamを閉じる。
 * @param args (stream)
 * @param env 呼び出し時の環境(未使用)
 * @return nil
 */
lisp_val_t cc_close(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数READ-CHAR。第一引数のstreamから1文字読み込む。
 * @param args (stream)
 * @param env 呼び出し時の環境(未使用)
 * @return 読み込んだCHAR。EOF・エラー・close後はnil
 */
lisp_val_t cc_read_char(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数WRITE-CHAR。第一引数のCHARを第二引数のstreamへ書き込む。
 * @param args (char stream)
 * @param env 呼び出し時の環境(未使用)
 * @return 第一引数のCHAR自身(書き込みの成否に関わらず)
 */
lisp_val_t cc_write_char(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数READ。第一引数のstreamから1つのS式を読み込む。
 * @param args (stream)
 * @param env 呼び出し時の環境(未使用)
 * @return 読み込んだS式。読めるものが無ければnil、構文エラーならg_sym_read_error
 */
lisp_val_t cc_read(lisp_val_t args, lisp_val_t env);

/** OPEN-INPUT-STREAM/OPEN-OUTPUT-STREAM/CLOSE/READ-CHAR/WRITE-CHAR/READをglobal_environmentに登録する */
void os_register_streams(void);

#endif /* _STREAM_LISP_H_ */
