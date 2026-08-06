#ifndef _VIRTIO9P_H_
#define _VIRTIO9P_H_

#include "types.h"
#include "framebuffer.h"

/**
 * VirtIO-9p経由でホスト共有ディレクトリ配下の1ファイルを読み込む。
 * PCI検出→virtio初期化→Tversion→Tattach→Twalk→Topen→Treadを順に行う。
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
