#ifndef _STREAM_H_
#define _STREAM_H_

#include "types.h"
#include "framebuffer.h"
#include "mount.h"

/** OPEN-INPUT-STREAM等に渡せるパスの最大長(NUL終端込み)。stream_lisp.c/load.c/
    mount.cで共有する規約 */
#define STREAM_PATH_MAX 256

/** ストリームの種別 */
typedef enum {
    STREAM_9P_FILE_READ,
    STREAM_9P_FILE_WRITE,
    STREAM_9P_FILE_IO,
    STREAM_OUTPUT_SCREEN,
    STREAM_STRING_INPUT,
    STREAM_STRING_OUTPUT,
    STREAM_FAT_FILE_WRITE,
    STREAM_FAT_FILE_IO
} stream_kind_t;

/** STREAM_FAT_FILE_WRITE/IOの書き込みバッファ容量。os_alloc_rawはreallocできない
    ため固定容量で確保する(STREAM_STRING_OUTPUT_CAPと同じ制約)。この容量を超える
    ファイルはマウント経由では書き込めない既知の制限とする */
#define STREAM_FAT_FILE_CAP 65536

/**
 * 9Pのバイト列をバッファリングしながら1文字ずつ切り出す、文字列バッファ、または
 * 画面へ1文字ずつ書き出すためのストリーム。buf_data/buf_count/buf_posは
 * os_virtio9p_read_chunkが返す受信バッファ(呼び出しごとに上書きされる共有バッファ)
 * からstream自身にコピーした内容を保持する。複数の9pストリームを跨いで読み込みが
 * 交互に発生しても(例: あるファイルのload中に別ファイルをloadする)、他方のTreadで
 * 共有バッファが上書きされて読み込み内容が壊れないようにするため。
 */
typedef struct {
    stream_kind_t kind;

    /* --- STREAM_9P_FILE_READ/WRITE/IO 共通: 現在の読み書きカーソル --- */
    UINT32 fid;
    UINT64 next_offset;

    /* --- STREAM_9P_FILE_READ/IOの読み込み用バッファ(stream自身にコピーを保持) --- */
    UINT8 buf_data[1024];
    UINT32 buf_count;
    UINT32 buf_pos;

    /* --- STREAM_9P_FILE_WRITE/IOの書き込み用バッファ。満杯またはfinish-output/closeでflush --- */
    UINT8 write_buf[512];
    UINT32 write_buf_len;

    int eof;
    int error;

    /** STREAM_OUTPUT_SCREENの書き込み先 */
    frame_buffer *out_fb;

    /** STREAM_STRING_INPUT/OUTPUTのバッファ本体(os_alloc_rawで確保)。
        INPUTはstr_pos、OUTPUTはstr_lenがそれぞれ読み取り/書き込み済みの位置を表す */
    UINT8 *str_buf;
    UINT32 str_cap;
    UINT32 str_len;
    UINT32 str_pos;

    /** preview-char用の1文字先読みキャッシュ */
    int has_lookahead;
    char lookahead;

    /** formatの~T/format-fresh-line用の現在の出力桁位置 */
    UINT32 column;

    /** os_stream_closeを呼んだ後は1になり、以後の読み書きを禁止する */
    int closed;

    /** STREAM_FAT_FILE_WRITE/IO専用: os_stream_closeで(fat32|fat16)-write-file等を
        呼ぶために必要な情報(mount_kind_t. MOUNT_KIND_FAT32/FAT16のいずれか) */
    mount_kind_t mount_fs_kind;
    lisp_val_t mount_device;
    char mount_relative_path[STREAM_PATH_MAX];
} os_stream_t;

/** STREAM_STRING_OUTPUTの固定バッファ容量(realloc不可のため) */
#define STREAM_STRING_OUTPUT_CAP 1024

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
 * 9Pファイルを書き込み用にopenする。既存ファイルが有ればopen+truncate、
 * 無ければ新規作成する(create_if_missingが真の場合)。
 * @param stream 初期化先
 * @param path 9Pエクスポートルートから見た相対パス
 * @param create_if_missing 既存ファイルのopenに失敗した場合、新規作成にフォールバックするか
 * @param err_msg 失敗時にエラー内容(NUL終端文字列)を格納する先
 * @param err_msg_cap err_msgの容量
 * @return 成功時1、失敗時0
 */
int os_stream_open_9p_file_write(os_stream_t *stream, const char *path, int create_if_missing, char *err_msg, UINT32 err_msg_cap);

/**
 * 9Pファイルを読み書き両用にopenする。既存ファイルが有ればopen+truncate、
 * 無ければ新規作成する(create_if_missingが真の場合)。
 * @param stream 初期化先
 * @param path 9Pエクスポートルートから見た相対パス
 * @param create_if_missing 既存ファイルのopenに失敗した場合、新規作成にフォールバックするか
 * @param err_msg 失敗時にエラー内容(NUL終端文字列)を格納する先
 * @param err_msg_cap err_msgの容量
 * @return 成功時1、失敗時0
 */
int os_stream_open_9p_file_io(os_stream_t *stream, const char *path, int create_if_missing, char *err_msg, UINT32 err_msg_cap);

/**
 * 画面(fb)への出力専用ストリームとして初期化する。
 * @param stream 初期化先
 * @param fb 出力先のframe buffer
 */
void os_stream_open_screen_output(os_stream_t *stream, frame_buffer *fb);

/**
 * 文字列を読み込み専用ストリームとして初期化する。dataの内容をコピーして保持する。
 * @param stream 初期化先
 * @param data 読み込み元の文字列データ
 * @param len dataのバイト数
 */
void os_stream_open_string_input(os_stream_t *stream, const char *data, UINT32 len);

/**
 * 固定容量(STREAM_STRING_OUTPUT_CAP)の文字列出力ストリームとして初期化する。
 * @param stream 初期化先
 */
void os_stream_open_string_output(os_stream_t *stream);

/**
 * マウントされたFAT32/FAT16デバイスへの書き込み専用ストリームとして初期化する。
 * 書き込み内容は固定容量(STREAM_FAT_FILE_CAP)のバッファに溜め、os_stream_close時に
 * まとめて(fat32|fat16)-write-file(無ければ...-create-file)へ渡す。
 * @param stream 初期化先
 * @param kind MOUNT_KIND_FAT32またはMOUNT_KIND_FAT16
 * @param device deviceシンボル
 * @param relative_path close時にFATドライバへ渡す相対パス(NUL終端)
 */
void os_stream_open_fat_file_write(os_stream_t *stream, mount_kind_t kind, lisp_val_t device, const char *relative_path);

/**
 * マウントされたFAT32/FAT16デバイスへの読み書き両用ストリームとして初期化する。
 * open時点では空のバッファから始まる(9PのSTREAM_9P_FILE_IOと同様、open=truncate
 * 相当)。書き込んだ内容はos_stream_close時にos_stream_open_fat_file_writeと
 * 同様にflushされる。
 * @param stream 初期化先
 * @param kind MOUNT_KIND_FAT32またはMOUNT_KIND_FAT16
 * @param device deviceシンボル
 * @param relative_path close時にFATドライバへ渡す相対パス(NUL終端)
 */
void os_stream_open_fat_file_io(os_stream_t *stream, mount_kind_t kind, lisp_val_t device, const char *relative_path);

/**
 * streamから1文字読み込む。内部バッファが尽きていれば9PのTreadで再充填する。
 * has_lookaheadが立っていればそちらを優先して消費する。
 * @param stream 読み込み元
 * @param out_ch 読み込んだ文字を格納する先
 * @return 読み込めた場合1、EOFまたはI/Oエラーまたはclose後の場合0(区別はstream->eof/error/closedで行う)
 */
int os_stream_read_char(os_stream_t *stream, char *out_ch);

/**
 * streamから1文字先読みする(カーソルは進めない)。2回連続で呼んでも同じ文字が返る。
 * @param stream 読み込み元
 * @param out_ch 読み込んだ文字を格納する先
 * @return 読み込めた場合1、EOFまたはI/Oエラーまたはclose後の場合0
 */
int os_stream_preview_char(os_stream_t *stream, char *out_ch);

/**
 * streamに1文字書き込む。読み込み専用のkindや、close後は何もしない。
 * 9Pファイル書き込み系はwrite_bufに積むだけで、満杯になるまで実際のTwriteは発生しない。
 * @param stream 書き込み先
 * @param ch 書き込む文字
 * @return 書き込めた場合1、そうでなければ0
 */
int os_stream_write_char(os_stream_t *stream, char ch);

/**
 * 9Pファイル書き込み系のwrite_bufに溜まっている内容を強制的にTwriteで送出する。
 * 他のkindでは何もしない。
 * @param stream flush対象
 */
void os_stream_finish_output(os_stream_t *stream);

/**
 * streamを閉じる。9Pファイル系の場合はまずfinish-output相当のflushを行い、fidを解放する。
 * 以後streamはclosed==1になる。
 * @param stream 解放対象
 */
void os_stream_close(os_stream_t *stream);

#endif /* _STREAM_H_ */
