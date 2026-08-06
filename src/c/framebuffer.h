#ifndef _FRAMEBUFFER_H_
#define _FRAMEBUFFER_H_

#include "types.h"


/** 仮想バッファの数(F1〜F4に割り当てる) */
#define VBUF_COUNT 4
/** 仮想バッファが保持できる文字グリッドの最大列数・行数 */
#define VBUF_MAX_COLS 256
#define VBUF_MAX_ROWS 128


typedef struct _frame_buffer {
    volatile UINT32 *buffer;
    UINT32 width;
    UINT32 height;
    UINT32 pixels_per_scanline;

    /** カーソルの位置 */
    struct _cursor_position {
        UINT32 x;
        UINT32 y;
    } cursor_position;

    /**
     * この仮想バッファに表示されている文字の内容。
     * 非アクティブな間もここに保持され、アクティブになった際に画面へ再描画される。
     */
    UINT8 content[VBUF_MAX_ROWS][VBUF_MAX_COLS];


    /**
     * frame buffer を黒色で塗る
     * @param self frame buffer
     */
    void (*clear_screen)(struct _frame_buffer *self);

    /**
     * カーソルを表示する
     * @param self frame buffer
     */
    void (*draw_cursor)(struct _frame_buffer *self);
    /**
     * カーソルを消す
     * @param self frame buffer
     */
    void (*erase_cursor)(struct _frame_buffer *self);
    /**
     * 現在のカーソル位置に1文字出力する。
     * 文字出力後はカーソルを1字分動かす。
     * 文字が改行の場合はカーソル位置を次の行にする。
     * @param self frame buffer
     * @param c 出力する文字
     */
    void (*write_char)(struct _frame_buffer *self, UINT8 c);
    /**
     * 現在のカーソル位置から文字列を出力する。
     * 文字出力後はカーソルを文字数分動かす。
     * 文字列の中に改行があればカーソルを次の行に移動させる。
     * @param self frame buffer
     * @param s 出力する文字列
     */
    void (*write_string)(struct _frame_buffer *self, const char* s);


    /**
     * frame buffer上のカーソルのX座標を返す
     * @param self frame buffer
     * @return カーソルのX座標(pixel)
     */
    UINT32 (*cursor_x)(struct _frame_buffer *self);
    /**
     * frame buffer上のカーソルのY座標を返す
     * @param self frame buffer
     * @return カーソルのY座標(pixel)
     */
    UINT32 (*cursor_y)(struct _frame_buffer *self);

} frame_buffer;


/**
 * VBUF_COUNT個の仮想frame bufferを初期化する
 * @param base 書き込み先のframe bufferのアドレス(全バッファ共通の物理アドレス)
 * @param width 横幅
 * @param height 高さ
 * @param pixels_per_scanline 1行のピクセル数
 * @return 初期状態でアクティブなバッファ(index 0)のアドレス
 */
frame_buffer* initialize_virtual_buffers(UINT64 base, UINT32 width, UINT32 height, UINT32 pixels_per_scanline);

/**
 * 現在アクティブな仮想frame bufferを返す
 * @return 現在アクティブな仮想frame buffer
 */
frame_buffer* get_active_frame_buffer(void);

/**
 * アクティブな仮想frame bufferを切り替え、画面を再描画する
 * @param index 切り替え先のバッファ番号(0〜VBUF_COUNT-1)
 */
void switch_active_frame_buffer(UINT32 index);


#endif /* _FRAMEBUFFER_H_ */

