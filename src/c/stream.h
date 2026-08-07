#ifndef _STREAM_H_
#define _STREAM_H_

#include "types.h"
#include "framebuffer.h"

/** ストリームの種別(9Pファイルの読み込み、または画面への書き込み) */
typedef enum {
    STREAM_9P_FILE,
    STREAM_OUTPUT_SCREEN
} stream_kind_t;

/**
 * 9Pのバイト列をバッファリングしながら1文字ずつ切り出す、または画面へ1文字ずつ
 * 書き出すためのストリーム。buf_data/buf_count/buf_posはos_virtio9p_read_chunkが
 * 返す受信バッファ(呼び出しごとに上書きされる)への参照であり、コピーは保持しない。
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
    /** STREAM_OUTPUT_SCREENの書き込み先 */
    frame_buffer *out_fb;
    /** os_stream_closeを呼んだ後は1になり、以後の読み書きを禁止する */
    int closed;
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
 * 画面(fb)への出力専用ストリームとして初期化する。
 * @param stream 初期化先
 * @param fb 出力先のframe buffer
 */
void os_stream_open_screen_output(os_stream_t *stream, frame_buffer *fb);

/**
 * streamから1文字読み込む。内部バッファが尽きていれば9PのTreadで再充填する。
 * @param stream 読み込み元
 * @param out_ch 読み込んだ文字を格納する先
 * @return 読み込めた場合1、EOFまたはI/Oエラーまたはclose後の場合0(区別はstream->eof/error/closedで行う)
 */
int os_stream_read_char(os_stream_t *stream, char *out_ch);

/**
 * streamに1文字書き込む。close後、またはSTREAM_OUTPUT_SCREEN以外のkindでは何もしない。
 * @param stream 書き込み先
 * @param ch 書き込む文字
 * @return 書き込めた場合1、そうでなければ0
 */
int os_stream_write_char(os_stream_t *stream, char ch);

/**
 * streamを閉じる。STREAM_9P_FILEの場合はfidを解放する。以後streamはclosed==1になる。
 * @param stream 解放対象
 */
void os_stream_close(os_stream_t *stream);

#endif /* _STREAM_H_ */
