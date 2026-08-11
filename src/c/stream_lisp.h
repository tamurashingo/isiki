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
 * MAGIC_STREAMのTAG_INSTANCEから元のos_stream_t*を取り出す(os_make_streamの逆)。
 * formatモジュール等、stream_lisp.c外からstreamの生ポインタが必要な場合に使う。
 * @param stream タグ付けされたINSTANCE(STREAM)
 * @return 元のos_stream_t*
 */
os_stream_t *os_stream_from_lisp(lisp_val_t stream);

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

/**
 * 組み込み関数OPEN-STREAM-P。第一引数がまだcloseされていないstreamかどうかを判定する。
 * @param args (obj)
 * @param env 呼び出し時の環境(未使用)
 * @return open状態のstreamならg_sym_t、そうでなければnil
 */
lisp_val_t cc_open_stream_p(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数INPUT-STREAM-P。第一引数が入力可能なkindのstreamかどうかを判定する。
 * @param args (obj)
 * @param env 呼び出し時の環境(未使用)
 * @return 入力可能なstreamならg_sym_t、そうでなければnil
 */
lisp_val_t cc_input_stream_p(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数OUTPUT-STREAM-P。第一引数が出力可能なkindのstreamかどうかを判定する。
 * @param args (obj)
 * @param env 呼び出し時の環境(未使用)
 * @return 出力可能なstreamならg_sym_t、そうでなければnil
 */
lisp_val_t cc_output_stream_p(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数OPEN-OUTPUT-FILE。第一引数のパスの9Pファイルを書き込み用にopenする
 * (既存ファイルが有ればopen+truncate、無ければ新規作成する)。
 * @param args (filename)
 * @param env 呼び出し時の環境(未使用)
 * @return 成功時STREAM、失敗時g_sym_eval_error
 */
lisp_val_t cc_open_output_file(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数OPEN-IO-FILE。第一引数のパスの9Pファイルを読み書き両用にopenする
 * (既存ファイルが有ればopen+truncate、無ければ新規作成する)。
 * @param args (filename)
 * @param env 呼び出し時の環境(未使用)
 * @return 成功時STREAM、失敗時g_sym_eval_error
 */
lisp_val_t cc_open_io_file(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数FINISH-OUTPUT。第一引数のstreamの書き込みバッファ(9Pファイル系のみ)を
 * 強制的にflushする。
 * @param args (stream)
 * @param env 呼び出し時の環境(未使用)
 * @return nil
 */
lisp_val_t cc_finish_output(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数CREATE-STRING-INPUT-STREAM。第一引数のSTRINGの内容を読み込み元とする
 * streamを作る。
 * @param args (string)
 * @param env 呼び出し時の環境(未使用)
 * @return 作成したSTREAM
 */
lisp_val_t cc_create_string_input_stream(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数CREATE-STRING-OUTPUT-STREAM。書き込み内容を内部バッファに蓄積するstreamを作る。
 * @param args 未使用
 * @param env 呼び出し時の環境(未使用)
 * @return 作成したSTREAM
 */
lisp_val_t cc_create_string_output_stream(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数GET-OUTPUT-STREAM-STRING。第一引数のstreamに、前回この関数を呼んだ以降
 * (初回ならstream作成以降)に書き込まれた内容をSTRINGとして返す。呼ぶたびに内部バッファを
 * リセットする(仕様通り)。
 * @param args (stream)
 * @param env 呼び出し時の環境(未使用)
 * @return 蓄積されていた内容のSTRING
 */
lisp_val_t cc_get_output_stream_string(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数PREVIEW-CHAR。第一引数のstreamから1文字先読みする(カーソルは進めない)。
 * eos-error-p/eos-valueオプション引数は未対応(READ-CHARと同様、EOFでは常にnilを返す)。
 * @param args (stream)
 * @param env 呼び出し時の環境(未使用)
 * @return 先読みしたCHAR。EOF・エラー・close後はnil
 */
lisp_val_t cc_preview_char(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数READ-LINE。第一引数のstreamから改行(を含まない)までの1行を読み込む。
 * eos-error-p/eos-valueオプション引数は未対応。
 * @param args (stream)
 * @param env 呼び出し時の環境(未使用)
 * @return 読み込んだ行のSTRING。最初の読み込みで即EOFの場合はnil
 */
lisp_val_t cc_read_line(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数STREAM-READY-P。非同期I/Oが無く読み込みは常に同期的にブロックするため、
 * 常にg_sym_tを返すスタブ。
 * @param args (input-stream) 未使用
 * @param env 呼び出し時の環境(未使用)
 * @return 常にg_sym_t
 */
lisp_val_t cc_stream_ready_p(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数READ-BYTE。第一引数のstreamから1バイト読み込む(READ-CHARと同じ実装を
 * UINT8↔fixnumでラップしたもの。このOSにバイナリ/テキストの区別は無い)。
 * eos-error-p/eos-valueオプション引数は未対応。
 * @param args (input-stream)
 * @param env 呼び出し時の環境(未使用)
 * @return 読み込んだバイト値(FIXNUM)。EOF・エラー・close後はnil
 */
lisp_val_t cc_read_byte(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数WRITE-BYTE。第一引数のバイト値(FIXNUM)を第二引数のstreamへ書き込む
 * (WRITE-CHARと同じ実装をUINT8↔fixnumでラップしたもの)。
 * @param args (z output-stream)
 * @param env 呼び出し時の環境(未使用)
 * @return 第一引数のz自身(書き込みの成否に関わらず)
 */
lisp_val_t cc_write_byte(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数PROBE-FILE。第一引数のパスの9Pファイルをopenできるかどうかを判定する
 * (9PにTstat/Tgetattrが無いため、「openできるか」で存在確認を近似する)。
 * @param args (filename)
 * @param env 呼び出し時の環境(未使用)
 * @return openできればg_sym_t、そうでなければnil
 */
lisp_val_t cc_probe_file(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数FILE-POSITION。第一引数のstreamの現在位置を返す
 * (9Pファイル系はnext_offset、文字列入力はstr_pos、文字列出力はstr_len)。
 * @param args (stream)
 * @param env 呼び出し時の環境(未使用)
 * @return 現在位置(FIXNUM)
 */
lisp_val_t cc_file_position(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数SET-FILE-POSITION。第一引数のstreamの現在位置を第二引数の値に設定する。
 * 9Pファイル系は読み込みバッファを空にして次回読み込み時に新しい位置からTreadし直す。
 * @param args (stream z)
 * @param env 呼び出し時の環境(未使用)
 * @return 設定した位置(第二引数z自身)
 */
lisp_val_t cc_set_file_position(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数FILE-LENGTH。第一引数のパスの9Pファイルを開き、先頭から末尾まで読み切った
 * バイト数を返す(9PにTstat/Tgetattrが無いため、読み切った長さでファイルサイズを近似する)。
 * @param args (filename)
 * @param env 呼び出し時の環境(未使用)
 * @return 読み切ったバイト数(FIXNUM)。openに失敗した場合はg_sym_eval_error
 */
lisp_val_t cc_file_length(lisp_val_t args, lisp_val_t env);

/**
 * OPEN-INPUT-STREAM/OPEN-OUTPUT-STREAM/CLOSE/READ-CHAR/WRITE-CHAR/READ/OPEN-STREAM-P/
 * INPUT-STREAM-P/OUTPUT-STREAM-P/OPEN-OUTPUT-FILE/OPEN-IO-FILE/FINISH-OUTPUT/
 * CREATE-STRING-INPUT-STREAM/CREATE-STRING-OUTPUT-STREAM/GET-OUTPUT-STREAM-STRING/
 * PREVIEW-CHAR/READ-LINE/STREAM-READY-P/READ-BYTE/WRITE-BYTE/PROBE-FILE/FILE-POSITION/
 * SET-FILE-POSITION/FILE-LENGTHをglobal_environmentに登録する
 */
void os_register_streams(void);

#endif /* _STREAM_LISP_H_ */
