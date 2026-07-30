#ifndef _FRAMEBUFFER_H_
#define _FRAMEBUFFER_H_

#include "types.h"


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
 * frame bufferを初期化する
 * @param base 書き込み先のframe bufferのアドレス
 * @param width 横幅
 * @param height 高さ
 * @param pixels_per_scanline 1行のピクセル数
 * @return 初期化したframe bufferのアドレス。現在は g_frame_buffer のアドレス
 */
frame_buffer* initialize_frame_buffer(UINT64 base, UINT32 width, UINT32 height, UINT32 pixels_per_scanlien);


#endif /* _FRAMEBUFFER_H_ */

