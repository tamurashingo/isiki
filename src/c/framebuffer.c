#include "framebuffer.h"
#include "font8x16.h"


/** 仮想バッファ本体(VBUF_COUNT個)。initialize_virtual_buffersで初期化される */
static frame_buffer frame_buffers[VBUF_COUNT];

/** 現在アクティブ(=物理画面に表示中)な仮想バッファのindex */
static UINT32 active_index = 0;

static void render_frame_buffer(frame_buffer *self);


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
    return self->cursor_position.x * VBUF_GLYPH_WIDTH;
}

/**
 * カーソル位置のY座標をpixel単位で返す
 * @param self frame buffer
 * @return カーソルのY座標(pixel)
 */
static UINT32 cursor_y(frame_buffer *self) {
    return self->cursor_position.y * VBUF_GLYPH_HEIGHT;
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
    UINT8 code = (UINT8)c;
    if (code > 127) {
        code = '?';  /* font8x16はASCIIの128文字ぶんしか持っていない */
    }
    const UINT8 *glyph = font8x16[code];
    for (UINT32 row = 0; row < VBUF_GLYPH_HEIGHT; row++) {
        UINT8 bits = glyph[row];
        for (UINT32 col = 0; col < VBUF_GLYPH_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                buffer[(y + row) * pixels_per_scanline + (x + col)] = color;
            }
        }
    }
}

/**
 * カーソルを表示する。さかのぼって表示している最中はカーソルの実位置が
 * 窓の外にあるため描かない
 * @param self frame buffer
 */
static void draw_cursor(frame_buffer *self) {
    if (self->view_offset != 0) {
        return;
    }
    UINT32 x = cursor_x(self);
    UINT32 y = cursor_y(self);
    for (UINT32 row = 0; row < VBUF_GLYPH_HEIGHT; row++) {
        for (UINT32 col = 0; col < VBUF_GLYPH_WIDTH; col++) {
            self->buffer[(y + row) * self->pixels_per_scanline + (x + col)] = 0x00FFFFFF;
        }
    }
}

/**
 * カーソルを消す
 * @param self frame buffer
 */
static void erase_cursor(frame_buffer *self) {
    if (self->view_offset != 0) {
        return;
    }
    UINT32 x = cursor_x(self);
    UINT32 y = cursor_y(self);
    for (UINT32 row = 0; row < VBUF_GLYPH_HEIGHT; row++) {
        for (UINT32 col = 0; col < VBUF_GLYPH_WIDTH; col++) {
            self->buffer[(y + row) * self->pixels_per_scanline + (x + col)] = 0x00000000;
        }
    }
}

/**
 * 物理画面を1行分上にスクロールする(アクティブなバッファのみ)
 * @param self frame buffer
 */
static void scroll_up(frame_buffer *self) {
    UINT32 y;
    for (y = 0; y + VBUF_GLYPH_HEIGHT < self->height; y++) {
        for (UINT32 x = 0; x < self->width; x++) {
            self->buffer[y * self->pixels_per_scanline + x] =
                self->buffer[(y + VBUF_GLYPH_HEIGHT) * self->pixels_per_scanline + x];
        }
    }
    for (; y < self->height; y++) {
        for (UINT32 x = 0; x < self->width; x++) {
            self->buffer[y * self->pixels_per_scanline + x] = 0x00000000;
        }
    }
}

/* ============================== 履歴リング ==============================
 * 論理行 i (0 <= i < count) の実体は content[((base + i) % hist_rows) * cols] にある。
 * 画面に表示されるのは最後の rows 行(view_offset ぶんだけ手前へずらしたもの)で、
 * cursor_position.y はその窓の中の行番号(0〜rows-1)である。
 *
 * count は初期値 rows から始まり、行が増えるたびに hist_rows まで伸びる。伸び切った
 * 後は base が進んで最も古い行が捨てられる。いずれの場合も「捨てる・詰める」ための
 * コピーは発生しない(以前の実装は1行スクロールごとにグリッド全体をシフトしていた)。
 */

/** 論理行 index の先頭へのポインタ。indexは 0 <= index < count を満たすこと */
static UINT8 *logical_row(const frame_buffer *self, UINT32 index) {
    UINT32 ring = (self->base + index) % self->hist_rows;
    return self->content + (UINT64)ring * self->cols;
}

/** 論理行 index を空白(0)で埋める */
static void clear_logical_row(frame_buffer *self, UINT32 index) {
    UINT8 *row = logical_row(self, index);
    for (UINT32 col = 0; col < self->cols; col++) {
        row[col] = 0;
    }
}

UINT32 os_vbuf_max_view_offset(const frame_buffer *self) {
    return self->count - self->rows;
}

const UINT8 *os_vbuf_visible_row(const frame_buffer *self, UINT32 screen_row) {
    if (screen_row >= self->rows) {
        return 0;
    }
    /* 窓の先頭 = 最後の rows 行の先頭から、さらに view_offset ぶん手前 */
    UINT32 top = self->count - self->rows - self->view_offset;
    return logical_row(self, top + screen_row);
}

/**
 * 履歴を1行進める(最下部に空行を1つ足す)。
 * リングがまだ埋まっていなければ count を伸ばし、埋まっていれば base を進めて
 * 最も古い行を捨てる。どちらの場合も新しい行は同じ式で求まる。
 * @param self frame buffer
 */
static void content_scroll_up(frame_buffer *self) {
    if (self->count < self->hist_rows) {
        self->count++;
    } else {
        self->base = (self->base + 1) % self->hist_rows;
    }
    clear_logical_row(self, self->count - 1);
}

/**
 * 画面内の(col, row)に1文字書き込む。範囲外は無視する
 * @param self frame buffer
 * @param col 書き込む列(画面内)
 * @param row 書き込む行(画面内)
 * @param c 書き込む文字
 */
static void content_put(frame_buffer *self, UINT32 col, UINT32 row, UINT8 c) {
    if (row >= self->rows || col >= self->cols) {
        return;
    }
    /* 画面行 row に対応する論理行(view_offsetは書き込みには効かせない。
       書き込みは常に最下部の1画面に対して行われる) */
    logical_row(self, self->count - self->rows + row)[col] = c;
}

/* ============================== スクロールバック ============================== */

/** スクロール中であることを示す行の桁数(インジケータを描く一時バッファの上限) */
#define VBUF_INDICATOR_MAX 64

/** valueを10進でbufへ書き、書いた文字数を返す(libcが無いので自前) */
static UINT32 indicator_put_uint(char *buf, UINT32 pos, UINT32 value) {
    char tmp[12];
    UINT32 n = 0;
    if (value == 0) {
        tmp[n++] = '0';
    }
    while (value > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (n > 0 && pos < VBUF_INDICATOR_MAX - 1) {
        buf[pos++] = tmp[--n];
    }
    return pos;
}

/**
 * さかのぼり中であることを画面最終行へ直接描く。
 *
 * contentへは**書かない**。履歴が汚れると、戻ったときにインジケータが本文として
 * 残ってしまう。物理フレームバッファだけを塗るので、render_frame_bufferが
 * contentから描き直せば自然に消える。
 * @param self frame buffer
 */
static void draw_scroll_indicator(frame_buffer *self) {
    char text[VBUF_INDICATOR_MAX];
    UINT32 pos = 0;
    const char *prefix = "-- SCROLL ";
    while (*prefix && pos < VBUF_INDICATOR_MAX - 1) {
        text[pos++] = *prefix++;
    }
    pos = indicator_put_uint(text, pos, self->view_offset);
    if (pos < VBUF_INDICATOR_MAX - 1) {
        text[pos++] = '/';
    }
    pos = indicator_put_uint(text, pos, os_vbuf_max_view_offset(self));
    const char *suffix = " --";
    while (*suffix && pos < VBUF_INDICATOR_MAX - 1) {
        text[pos++] = *suffix++;
    }
    text[pos] = 0;

    UINT32 y = (self->rows - 1) * VBUF_GLYPH_HEIGHT;
    /* 最終行を一度黒で塗ってから描く(下の本文と重ならないように) */
    for (UINT32 row = 0; row < VBUF_GLYPH_HEIGHT; row++) {
        for (UINT32 x = 0; x < self->width; x++) {
            self->buffer[(y + row) * self->pixels_per_scanline + x] = 0x00000000;
        }
    }
    for (UINT32 i = 0; i < pos && i < self->cols; i++) {
        draw_char(self->buffer, self->pixels_per_scanline,
                  i * VBUF_GLYPH_WIDTH, y, text[i], 0x00FFFFFF);
    }
}

void os_vbuf_scroll(INT32 delta_rows) {
    frame_buffer *self = &frame_buffers[active_index];
    UINT32 max_offset = os_vbuf_max_view_offset(self);
    /* view_offsetは「さかのぼった行数」なので、過去方向(delta<0)で増える */
    INT64 next = (INT64)self->view_offset - (INT64)delta_rows;
    if (next < 0) {
        next = 0;
    }
    if (next > (INT64)max_offset) {
        next = (INT64)max_offset;
    }
    if ((UINT32)next == self->view_offset) {
        return;  /* 端に張り付いている。再描画も不要 */
    }
    self->view_offset = (UINT32)next;
    render_frame_buffer(self);
}

int os_vbuf_scroll_to_bottom(void) {
    frame_buffer *self = &frame_buffers[active_index];
    if (self->view_offset == 0) {
        return 0;
    }
    self->view_offset = 0;
    render_frame_buffer(self);
    return 1;
}

UINT32 os_vbuf_screen_rows(void) {
    return frame_buffers[active_index].rows;
}

/* ============================== 出力 ============================== */

/**
 * カーソルを次の行の先頭に移動する。画面の最終行を越えたらスクロールする
 * @param self frame buffer
 */
static void newline(frame_buffer *self) {
    self->cursor_position.x = 0;
    self->cursor_position.y += 1;
    if (self->cursor_position.y >= self->rows) {
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
    /* さかのぼり中に出力が来たら最下部へスナップしてから書く。
       触るのは**このバッファのview_offsetだけ**で、他のバッファには影響しない */
    if (self->view_offset != 0) {
        self->view_offset = 0;
        if (is_active(self)) {
            render_frame_buffer(self);
        }
    }

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
    /* 桁数ちょうどで折り返す。以前はピクセル数で `> width` と比較しており、
       1行に cols+1 文字目が入ってしまっていた(content上には残るが
       render_frame_bufferはcols桁で切るため、バッファを切り替えて戻ると消えた) */
    if (self->cursor_position.x >= self->cols) {
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
 * self の content のうち、いま表示すべき窓(view_offsetを考慮した rows 行)を
 * 物理画面へ再描画する。バッファ切り替え時とスクロール時に呼ぶ。
 * @param self frame buffer
 */
static void render_frame_buffer(frame_buffer *self) {
    clear_screen(self);

    for (UINT32 y = 0; y < self->rows; y++) {
        const UINT8 *row = os_vbuf_visible_row(self, y);
        if (row == 0) {
            continue;
        }
        for (UINT32 x = 0; x < self->cols; x++) {
            UINT8 c = row[x];
            if (c != 0) {
                draw_char(self->buffer, self->pixels_per_scanline,
                          x * VBUF_GLYPH_WIDTH, y * VBUF_GLYPH_HEIGHT, (char)c, 0x00FFFFFF);
            }
        }
    }

    if (self->view_offset != 0) {
        draw_scroll_indicator(self);
    } else {
        draw_cursor(self);
    }
}

/**
 * 現在アクティブな仮想frame bufferを返す
 * @return 現在アクティブな仮想frame buffer
 */
frame_buffer* get_active_frame_buffer(void) {
    return &frame_buffers[active_index];
}

/**
 * アクティブな仮想バッファをindexへ切り替え、切替え先の内容を物理画面に再描画する。
 * view_offsetはバッファごとに保持しているので、戻ってきたときは同じ位置が表示される
 * @param index 切り替え先のバッファ番号(0〜VBUF_COUNT-1)
 */
void switch_active_frame_buffer(UINT32 index) {
    if (index >= VBUF_COUNT || index == active_index) {
        return;
    }
    active_index = index;
    render_frame_buffer(&frame_buffers[index]);
}

UINT32 os_vbuf_effective_hist_rows(UINT32 height, UINT32 hist_rows) {
    UINT32 rows = height / VBUF_GLYPH_HEIGHT;
    return hist_rows < rows ? rows : hist_rows;
}

UINT64 os_vbuf_content_bytes(UINT32 width, UINT32 height, UINT32 hist_rows) {
    UINT64 cols = width / VBUF_GLYPH_WIDTH;
    return cols * (UINT64)os_vbuf_effective_hist_rows(height, hist_rows) * VBUF_COUNT;
}

/**
 * VBUF_COUNT個の仮想frame bufferを初期化し、すべて同じ物理バッファ領域(base)を指すようにする
 * @param base 書き込み先のframe bufferのアドレス(全バッファ共通の物理アドレス)
 * @param width 横幅
 * @param height 高さ
 * @param pixels_per_scanline 1行のピクセル数
 * @param content_area os_vbuf_content_bytesぶんの領域
 * @param hist_rows 1バッファあたりの履歴行数
 * @return 初期状態でアクティブなバッファ(index 0)のアドレス
 */
frame_buffer* initialize_virtual_buffers(UINT64 base, UINT32 width, UINT32 height,
                                         UINT32 pixels_per_scanline,
                                         UINT8 *content_area, UINT32 hist_rows) {
    UINT32 cols = width / VBUF_GLYPH_WIDTH;
    UINT32 rows = height / VBUF_GLYPH_HEIGHT;
    /* 切り上げは os_vbuf_content_bytes と同じ関数を通す。ここだけで切り上げると、
       呼び出し側が確保したバイト数を超えて書き込むことになる */
    hist_rows = os_vbuf_effective_hist_rows(height, hist_rows);

    for (UINT32 i = 0; i < VBUF_COUNT; i++) {
        frame_buffer *fb = &frame_buffers[i];

        fb->buffer = (volatile UINT32 *)base;
        fb->width = width;
        fb->height = height;
        fb->pixels_per_scanline = pixels_per_scanline;
        fb->cursor_position.x = 0;
        fb->cursor_position.y = 0;

        fb->content = content_area + (UINT64)i * cols * hist_rows;
        fb->cols = cols;
        fb->rows = rows;
        fb->hist_rows = hist_rows;
        fb->base = 0;
        /* 空行で埋まった1画面ぶんから始める。こうしておくと
           「画面行 y の実体は論理行 count - rows + y」が最初から成立する */
        fb->count = rows;
        fb->view_offset = 0;

        for (UINT64 n = 0; n < (UINT64)cols * hist_rows; n++) {
            fb->content[n] = 0;
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
