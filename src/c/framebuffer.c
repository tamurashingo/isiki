#include "framebuffer.h"
#include "font8x16.h"


/** 仮想バッファ本体(VBUF_COUNT個)。initialize_virtual_buffersで初期化される */
static frame_buffer frame_buffers[VBUF_COUNT];

/** 現在アクティブ(=物理画面に表示中)な仮想バッファのindex */
static UINT32 active_index = 0;


/**
 * self が現在アクティブな(物理画面に表示中の)frame bufferかどうかを返す
 * @param self frame buffer
 * @return アクティブなら非0、そうでなければ0
 */
static int is_active(frame_buffer *self) {
    return self == &frame_buffers[active_index];
}

/**
 * カーソル位置のX座標をpixel単位で返す
 * @param self frame buffer
 * @return カーソルのX座標(pixel)
 */
static UINT32 cursor_x(frame_buffer *self) {
    return self->cursor_position.x * 8;
}

/**
 * カーソル位置のY座標をpixel単位で返す
 * @param self frame buffer
 * @return カーソルのY座標(pixel)
 */
static UINT32 cursor_y(frame_buffer *self) {
    return self->cursor_position.y * 16;
}

/**
 * フォントビットマップ(font8x16)を使って、bufferの(x,y)に1文字分のグリフを描画する
 * @param buffer 描画先のframe buffer本体
 * @param pixels_per_scanline 1行のピクセル数
 * @param x 描画先のX座標(pixel)
 * @param y 描画先のY座標(pixel)
 * @param c 描画する文字
 * @param color 描画色
 */
static void draw_char(volatile UINT32 *buffer, UINT32 pixels_per_scanline, UINT32 x, UINT32 y, char c, UINT32 color) {
    unsigned char code = (unsigned char)c;
    if (code > 127) {
        code = '?';
    }
    const unsigned char *glyph = font8x16[code];

    for (int row = 0; row < 16; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                buffer[(y + row) * pixels_per_scanline + (x + col)] = color;
            }
        }
    }
}

/**
 * カーソル位置に白い矩形を描画してカーソルを表示する
 * @param self frame buffer
 */
static void draw_cursor(frame_buffer *self) {
    for (UINT32 row = 0; row < 16; row++) {
        for (UINT32 col = 0; col < 8; col++) {
            self->buffer[(cursor_y(self) + row) * self->pixels_per_scanline + (cursor_x(self) + col)] = 0x00FFFFFF;
        }
    }
}

/**
 * カーソル位置を黒で塗ってカーソルを消す
 * @param self frame buffer
 */
static void erase_cursor(frame_buffer *self) {
    for (UINT32 row = 0; row < 16; row++) {
        for (UINT32 col = 0; col < 8; col++) {
            self->buffer[(cursor_y(self) + row) * self->pixels_per_scanline + (cursor_x(self) + col)] = 0x00000000;
        }
    }
}

/**
 * 物理画面を1行(16px)分上にスクロールする。content グリッド自体は変更しない
 * @param self frame buffer
 */
static void scroll_up(frame_buffer *self) {
    UINT32 y;
    for (y = 0; y + 16 < self->height; y++) {
        for (UINT32 x = 0; x < self->width; x++) {
            self->buffer[y * self->pixels_per_scanline + x] = self->buffer[(y + 16) * self->pixels_per_scanline + x];
        }
    }
    for (; y < self->height; y++) {
        for (UINT32 x = 0; x < self->width; x++) {
            self->buffer[y * self->pixels_per_scanline + x] = 0x00000000;
        }
    }
}

/**
 * content グリッドを1行分上にシフトし、最終行を空にする。
 * アクティブ・非アクティブに関わらず、仮想バッファの状態として常に呼ぶ。
 * @param self frame buffer
 */
static void content_scroll_up(frame_buffer *self) {
    UINT32 row;
    for (row = 0; row + 1 < VBUF_MAX_ROWS; row++) {
        for (UINT32 col = 0; col < VBUF_MAX_COLS; col++) {
            self->content[row][col] = self->content[row + 1][col];
        }
    }
    for (UINT32 col = 0; col < VBUF_MAX_COLS; col++) {
        self->content[VBUF_MAX_ROWS - 1][col] = 0;
    }
}

/**
 * content グリッドの(col, row)に1文字書き込む。範囲外は無視する
 * @param self frame buffer
 * @param col 書き込む列
 * @param row 書き込む行
 * @param c 書き込む文字
 */
static void content_put(frame_buffer *self, UINT32 col, UINT32 row, UINT8 c) {
    if (row >= VBUF_MAX_ROWS || col >= VBUF_MAX_COLS) {
        return;
    }
    self->content[row][col] = c;
}

/**
 * カーソルを次の行の先頭に移動する。画面の最終行を越えたらスクロールする
 * @param self frame buffer
 */
static void newline(frame_buffer *self) {
    self->cursor_position.x = 0;
    self->cursor_position.y += 1;
    if (cursor_y(self) >= self->height) {
        content_scroll_up(self);
        if (is_active(self)) {
            scroll_up(self);
        }
        self->cursor_position.y -= 1;
    }
}

/**
 * 現在のカーソル位置に1文字出力する。content グリッドは常に更新し、
 * アクティブな場合のみ物理画面へも描画する
 * @param self frame buffer
 * @param c 出力する文字
 */
static void write_char(frame_buffer *self, UINT8 c) {
    if (is_active(self)) {
        erase_cursor(self);
    }
    if (c == '\n') {
        newline(self);
        if (is_active(self)) {
            draw_cursor(self);
        }
        return;
    }

    content_put(self, self->cursor_position.x, self->cursor_position.y, c);
    if (is_active(self)) {
        draw_char(self->buffer, self->pixels_per_scanline, cursor_x(self), cursor_y(self), (char)c, 0x00FFFFFF);
    }
    self->cursor_position.x += 1;
    if (cursor_x(self) > self->width) {
        newline(self);
    }
    if (is_active(self)) {
        draw_cursor(self);
    }
}

/**
 * 文字列を先頭から1文字ずつwrite_charで出力する
 * @param self frame buffer
 * @param s 出力する文字列
 */
static void write_string(frame_buffer *self, const char *s) {
    while (*s) {
        write_char(self, (UINT8)*s);
        s++;
    }
}

/**
 * frame buffer を黒色で塗る
 * @param self frame buffer
 */
static void clear_screen(frame_buffer *self) {
    for (UINT32 y = 0; y < self->height; y++) {
        for (UINT32 x = 0; x < self->width; x++) {
            self->buffer[y * self->pixels_per_scanline + x] = 0x00000000;
        }
    }
}

/**
 * self の content グリッドの内容を物理画面へ再描画する。
 * バッファ切り替え時に呼ぶ。
 * @param self frame buffer
 */
static void render_frame_buffer(frame_buffer *self) {
    clear_screen(self);

    UINT32 rows = self->height / 16;
    UINT32 cols = self->width / 8;
    if (rows > VBUF_MAX_ROWS) {
        rows = VBUF_MAX_ROWS;
    }
    if (cols > VBUF_MAX_COLS) {
        cols = VBUF_MAX_COLS;
    }

    for (UINT32 y = 0; y < rows; y++) {
        for (UINT32 x = 0; x < cols; x++) {
            UINT8 c = self->content[y][x];
            if (c != 0) {
                draw_char(self->buffer, self->pixels_per_scanline, x * 8, y * 16, (char)c, 0x00FFFFFF);
            }
        }
    }

    draw_cursor(self);
}

/**
 * 現在アクティブな仮想frame bufferを返す
 * @return 現在アクティブな仮想frame buffer
 */
frame_buffer* get_active_frame_buffer(void) {
    return &frame_buffers[active_index];
}

/**
 * アクティブな仮想バッファをindexへ切り替え、切替え先の内容を物理画面に再描画する
 * @param index 切り替え先のバッファ番号(0〜VBUF_COUNT-1)
 */
void switch_active_frame_buffer(UINT32 index) {
    if (index >= VBUF_COUNT || index == active_index) {
        return;
    }
    active_index = index;
    render_frame_buffer(&frame_buffers[index]);
}

/**
 * VBUF_COUNT個の仮想frame bufferを初期化し、すべて同じ物理バッファ領域(base)を指すようにする
 * @param base 書き込み先のframe bufferのアドレス(全バッファ共通の物理アドレス)
 * @param width 横幅
 * @param height 高さ
 * @param pixels_per_scanline 1行のピクセル数
 * @return 初期状態でアクティブなバッファ(index 0)のアドレス
 */
frame_buffer* initialize_virtual_buffers(UINT64 base, UINT32 width, UINT32 height, UINT32 pixels_per_scanline) {

    for (UINT32 i = 0; i < VBUF_COUNT; i++) {
        frame_buffer *fb = &frame_buffers[i];

        fb->buffer = (volatile UINT32 *)base;
        fb->width = width;
        fb->height = height;
        fb->pixels_per_scanline = pixels_per_scanline;
        fb->cursor_position.x = 0;
        fb->cursor_position.y = 0;

        for (UINT32 row = 0; row < VBUF_MAX_ROWS; row++) {
            for (UINT32 col = 0; col < VBUF_MAX_COLS; col++) {
                fb->content[row][col] = 0;
            }
        }

        fb->clear_screen = clear_screen;
        fb->draw_cursor = draw_cursor;
        fb->erase_cursor = erase_cursor;
        fb->write_char = write_char;
        fb->write_string = write_string;
        fb->cursor_x = cursor_x;
        fb->cursor_y = cursor_y;
    }

    active_index = 0;

    return &frame_buffers[0];
}
