#ifndef _VIRTIO9P_H_
#define _VIRTIO9P_H_

#include "types.h"
#include "framebuffer.h"

/**
 * VirtIO-9pセッション(PCI検出→virtio初期化→Tversion→Tattach(fid=0))を、
 * まだ確立していなければ1度だけ行う。確立済みなら即成功を返す(何度呼んでもよい)。
 * @param err_msg 失敗時にエラー内容(NUL終端文字列)を格納する先
 * @param err_msg_cap err_msgの容量
 * @return 成功時1、失敗時0
 */
int os_virtio9p_ensure_session(char *err_msg, UINT32 err_msg_cap);

/**
 * 9Pエクスポートルートから見た相対パスのファイルをwalk+openし、fidを得る。
 * 内部でos_virtio9p_ensure_sessionを呼ぶ。fidは常に1つだけ使用するため、
 * 使い終わったら必ずos_virtio9p_closeで解放してから次のopenを呼ぶこと。
 * @param path 9Pエクスポートルートから見た相対パス(例: "src/lisp/init.lisp")
 * @param out_fid 得られたfidを格納する先
 * @param err_msg 失敗時にエラー内容(NUL終端文字列)を格納する先
 * @param err_msg_cap err_msgの容量
 * @return 成功時1、失敗時0
 */
int os_virtio9p_open(const char *path, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap);

/**
 * fidに対し、offsetからwantバイトを1回のTreadで読み込む。
 * out_dataは内部の受信バッファを指す(コピーしない)ため、次にこの関数や
 * os_virtio9p_closeを呼ぶ前に読み終えること。
 * @param fid os_virtio9p_openで得たfid
 * @param offset 読み込み開始オフセット
 * @param want 読み込み要求バイト数
 * @param out_data 実データ先頭へのポインタを格納する先
 * @param out_count 実際に読み込んだバイト数を格納する先(0はEOF)
 * @param err_msg 失敗時にエラー内容(NUL終端文字列)を格納する先
 * @param err_msg_cap err_msgの容量
 * @return 成功時1、失敗時0
 */
int os_virtio9p_read_chunk(UINT32 fid, UINT64 offset, UINT32 want,
                            const UINT8 **out_data, UINT32 *out_count,
                            char *err_msg, UINT32 err_msg_cap);

/**
 * os_virtio9p_openで得たfidをTclunkで解放する。
 * @param fid 解放するfid
 * @param err_msg 失敗時にエラー内容(NUL終端文字列)を格納する先
 * @param err_msg_cap err_msgの容量
 * @return 成功時1、失敗時0
 */
int os_virtio9p_close(UINT32 fid, char *err_msg, UINT32 err_msg_cap);

/**
 * VirtIO-9p経由でホスト共有ディレクトリ配下の1ファイルを丸ごと読み込む。
 * 内部でopen→read_chunkループ→closeを行う(os_virtio9p_open/read_chunk/closeの薄い合成)。
 * @param path 9Pエクスポートルートから見た相対パス(例: "src/lisp/init.lisp")
 * @param result_buf 読み込んだ内容を格納する先
 * @param result_cap result_bufの容量
 * @param out_len 実際に読み込んだバイト数を格納する先
 * @param err_msg 失敗時にエラー内容(NUL終端文字列)を格納する先
 * @param err_msg_cap err_msgの容量
 * @return 成功時1、失敗時0
 */
int os_virtio9p_load_file(const char *path, UINT8 *result_buf, UINT32 result_cap,
                           UINT32 *out_len, char *err_msg, UINT32 err_msg_cap);

/**
 * VirtIO-9pマイルストンの動作確認用エントリポイント。
 * "src/lisp/init.lisp" を読み込み、結果をframe bufferに表示する。
 * 失敗時はエラー内容を表示して停止する(kernel_mainから1回だけ呼ばれる想定)。
 * @param fb 表示先のframe buffer
 */
void os_virtio9p_test_run(frame_buffer *fb);

#endif /* _VIRTIO9P_H_ */
