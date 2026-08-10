#ifndef _VIRTIO9P_H_
#define _VIRTIO9P_H_

#include "types.h"

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
 * 内部でos_virtio9p_ensure_sessionを呼ぶ。fidは呼ぶたびに新規に割り当てられる
 * (使い終わったらos_virtio9p_closeで解放すること。同時に複数openしてよい)。
 * @param path 9Pエクスポートルートから見た相対パス(例: "src/lisp/init.lisp")
 * @param mode P9_OREAD/P9_OWRITE/P9_ORDWR。P9_OTRUNCを重ねてよい
 * @param out_fid 得られたfidを格納する先
 * @param err_msg 失敗時にエラー内容(NUL終端文字列)を格納する先
 * @param err_msg_cap err_msgの容量
 * @return 成功時1、失敗時0
 */
int os_virtio9p_open(const char *path, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap);

/**
 * 9Pエクスポートルートから見た相対パスに新規ファイルを作成し、openした状態のfidを得る。
 * パスの末尾要素をファイル名、それ以前を親ディレクトリのパスとして扱う。
 * 内部でos_virtio9p_ensure_sessionを呼ぶ。
 * @param path 9Pエクスポートルートから見た相対パス(例: "tmp/new.txt")
 * @param perm 作成する際のUnixパーミッション(例: 0644)
 * @param mode P9_OWRITE/P9_ORDWR
 * @param out_fid 得られたfidを格納する先
 * @param err_msg 失敗時にエラー内容(NUL終端文字列)を格納する先
 * @param err_msg_cap err_msgの容量
 * @return 成功時1、失敗時0
 */
int os_virtio9p_create(const char *path, UINT32 perm, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap);

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
 * fidに対し、offsetからdata[0..count)を1回のTwriteで書き込む。
 * @param fid os_virtio9p_open/createで得たfid
 * @param offset 書き込み開始オフセット
 * @param data 書き込むバイト列
 * @param count dataのバイト数
 * @param out_written 実際に書き込めたバイト数を格納する先
 * @param err_msg 失敗時にエラー内容(NUL終端文字列)を格納する先
 * @param err_msg_cap err_msgの容量
 * @return 成功時1、失敗時0
 */
int os_virtio9p_write_chunk(UINT32 fid, UINT64 offset, const UINT8 *data, UINT32 count,
                             UINT32 *out_written, char *err_msg, UINT32 err_msg_cap);

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

#endif /* _VIRTIO9P_H_ */
