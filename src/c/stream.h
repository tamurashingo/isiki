#ifndef _STREAM_H_
#define _STREAM_H_

#include "types.h"

/** ストリームの種別(現在は9Pファイルのみ) */
typedef enum {
    STREAM_9P_FILE
} stream_kind_t;

/**
 * 9Pのバイト列をバッファリングしながら1文字ずつ切り出すためのストリーム。
 * buf_data/buf_count/buf_posはos_virtio9p_read_chunkが返す受信バッファ
 * (呼び出しごとに上書きされる)への参照であり、コピーは保持しない。
 */
typedef struct {
    stream_kind_t kind;
    UINT32 fid;
    UINT64 next_offset;
    const UINT8 *buf_data;
    UINT32 buf_count;
    UINT32 buf_pos;
    int eof;
    int error;
} os_stream_t;

/**
 * 9Pファイルをopenし、streamを読み込み可能な状態に初期化する。
 * @param stream 初期化先
 * @param path 9Pエクスポートルートから見た相対パス
 * @param err_msg 失敗時にエラー内容(NUL終端文字列)を格納する先
 * @param err_msg_cap err_msgの容量
 * @return 成功時1、失敗時0
 */
int os_stream_open_9p_file(os_stream_t *stream, const char *path, char *err_msg, UINT32 err_msg_cap);

/**
 * streamから1文字読み込む。内部バッファが尽きていれば9PのTreadで再充填する。
 * @param stream 読み込み元
 * @param out_ch 読み込んだ文字を格納する先
 * @return 読み込めた場合1、EOFまたはI/Oエラーの場合0(区別はstream->eof/errorで行う)
 */
int os_stream_read_char(os_stream_t *stream, char *out_ch);

/**
 * streamが保持しているfidを解放する。
 * @param stream 解放対象
 */
void os_stream_close(os_stream_t *stream);

#endif /* _STREAM_H_ */
