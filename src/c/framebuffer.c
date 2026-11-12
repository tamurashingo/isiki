#include "framebuffer.h"
#include "font8x16.h"


/** 仮想バッファ本体(VBUF_COUNT個)。initialize_virtual_buffersで初期化される */
static frame_buffer frame_buffers[VBUF_COUNT];

/** 現在アクティブ(=物理画面に表示中)な仮想バッファのindex */
static UINT32 active_index = 0;

static void render_frame_buffer(frame_buffer *self);
/* カーソルの描画が格子を引けるように前方宣言する(定義は履歴リングの節にある) */
static UINT8 *content_screen_row(const frame_buffer *self, UINT32 row);


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

/* ============================== カーソル(#227) ==============================
 * **カーソルはセルの中身を壊してはならない。**
 *
 * 直す前は draw_cursor が 8x16 を真っ白で塗るだけ、erase_cursor が真っ黒で塗る
 * だけだった。draw_char(:50-64)は**立っているビットだけを塗り、背景には触らない**
 * ので、この 2 つはこう壊れていた(利用者が実機のエディタで踏んだ。#227):
 *   - カーソルの下の文字が白い矩形に潰れて見えない
 *   - カーソルが通り過ぎたマスは黒いまま残り、文字が画面から消える
 *     (格子は壊れていないので、その行に何か入力すると描き直されて復活する)
 *
 * だから**どちらも格子から文字を引いて描き直す**。カーソルの位置は反転表示
 * (白地に黒)にする —— 端末ソフトの標準的な見た目であり、1 マス 1 バイト・
 * 色なしの格子のままできる。
 */

/** セル1つ(8x16)を単色で塗る */
static void fill_cell(frame_buffer *self, UINT32 x, UINT32 y, UINT32 color) {
    for (UINT32 row = 0; row < VBUF_GLYPH_HEIGHT; row++) {
        for (UINT32 col = 0; col < VBUF_GLYPH_WIDTH; col++) {
            self->buffer[(y + row) * self->pixels_per_scanline + (x + col)] = color;
        }
    }
}

/**
 * カーソル位置の格子にある文字を返す。何も書かれていなければ 0。
 *
 * **格子の初期値は 0 であり、空白文字 ' '(0x20)とは別の値である**
 * (clear_logical_row / os_vbuf_clear / initialize_virtual_buffers がいずれも 0 で
 * 埋める。アプリが空白で塗ると 0x20 が入るので、両方が現れる)。
 * **0 のときは字形を引かない。** render_frame_buffer が格子を描くときの判定
 * (`if (c != 0)`、:437)と同じにして、「格子の 0 は描かない」を 1 つの約束に揃える。
 *
 * なお font8x16[0x00] と font8x16[0x20] は**どちらも 16 バイトすべて 0** である
 * ことを確かめた(src/c/font8x16.h。表は tools/gen_font.py が spleen から生成する)。
 * つまり今のフォントでは描いても画素は変わらない。それでも 0 を弾いておくのは、
 * フォント表が差し替わったときに**未書き込みのマスに 0x00 の字形が出る**ことを
 * 防ぐためである。
 * @param self frame buffer
 * @return カーソル位置の文字(空なら0)
 */
static UINT8 cursor_cell_char(const frame_buffer *self) {
    if (self->cursor_position.y >= self->rows || self->cursor_position.x >= self->cols) {
        return 0;
    }
    return content_screen_row(self, self->cursor_position.y)[self->cursor_position.x];
}

/**
 * カーソルを**反転表示**で描く。さかのぼって表示している最中はカーソルの実位置が
 * 窓の外にあるため描かない
 * @param self frame buffer
 */
static void draw_cursor(frame_buffer *self) {
    if (self->view_offset != 0) {
        return;
    }
    UINT32 x = cursor_x(self);
    UINT32 y = cursor_y(self);
    fill_cell(self, x, y, VBUF_COLOR_FG);
    UINT8 c = cursor_cell_char(self);
    if (c != 0) {
        /* 白で塗ったあと**同じ文字を黒で**描く。これで下の文字が見える */
        draw_char(self->buffer, self->pixels_per_scanline, x, y, (char)c, VBUF_COLOR_BG);
    }
}

/**
 * カーソルを消し、**そのマスの文字を描き直す**
 * @param self frame buffer
 */
static void erase_cursor(frame_buffer *self) {
    if (self->view_offset != 0) {
        return;
    }
    UINT32 x = cursor_x(self);
    UINT32 y = cursor_y(self);
    fill_cell(self, x, y, VBUF_COLOR_BG);
    UINT8 c = cursor_cell_char(self);
    if (c != 0) {
        /* 黒で塗っただけで戻ると、通り過ぎたマスの文字が消える(#227 の症状1) */
        draw_char(self->buffer, self->pixels_per_scanline, x, y, (char)c, VBUF_COLOR_FG);
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
            self->buffer[y * self->pixels_per_scanline + x] = VBUF_COLOR_BG;
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
static UINT8 *content_screen_row(const frame_buffer *self, UINT32 row) {
    /* 画面行 row に対応する論理行(view_offsetは書き込みには効かせない。
       書き込みは常に最下部の1画面に対して行われる) */
    return logical_row(self, self->count - self->rows + row);
}

static void content_put(frame_buffer *self, UINT32 col, UINT32 row, UINT8 c) {
    if (row >= self->rows || col >= self->cols) {
        return;
    }
    content_screen_row(self, row)[col] = c;
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
            self->buffer[(y + row) * self->pixels_per_scanline + x] = VBUF_COLOR_BG;
        }
    }
    for (UINT32 i = 0; i < pos && i < self->cols; i++) {
        draw_char(self->buffer, self->pixels_per_scanline,
                  i * VBUF_GLYPH_WIDTH, y, text[i], VBUF_COLOR_FG);
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
        draw_char(self->buffer, self->pixels_per_scanline, cursor_x(self), cursor_y(self), (char)c, VBUF_COLOR_FG);
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
            self->buffer[y * self->pixels_per_scanline + x] = VBUF_COLOR_BG;
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
                          x * VBUF_GLYPH_WIDTH, y * VBUF_GLYPH_HEIGHT, (char)c, VBUF_COLOR_FG);
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

/* ============================== 位置指定・読み出し・消去(#216) ============================== */

/*
 * [#216] 全画面を使うアプリの土台。**この 3 つがこの層に無かった。**
 * 調査(logic §9-7/9-8/9-9)は「カーソルの位置を外から指定できない / 消去の手段が
 * 外に無い / 格子の内容を読み返す手段が無い」と報告している。
 *
 * **3 つを同時に入れたのは、読み出しが無いと位置指定が効いていることを
 * 機械で確かめられないからである**(規則 8 の陽性対照が置けない)。
 * 範囲外の扱いは「黙って捨てる」ではなく**0 を返して呼び出し側に見せる**
 * (content_put は黙って捨てる。framebuffer.c:180-182。同じ作りにすると
 * Lisp 側の口が「効いたのか効かなかったのか」を言えなくなる)。
 */

int os_vbuf_set_cursor(frame_buffer *self, UINT32 row, UINT32 col) {
    if (row >= self->rows || col >= self->cols) {
        return 0;
    }
    /* さかのぼり中なら最下部へ戻す。カーソルは**表示窓の中の位置**であり
       (framebuffer.h:29)、書き込みは常に最下部の 1 画面に対して行われるので、
       さかのぼったまま位置を指定すると「指した場所」と「書かれる場所」が
       食い違う。write_char が入口で同じことをしている(framebuffer.c:308-313) */
    if (self->view_offset != 0) {
        self->view_offset = 0;
        if (is_active(self)) {
            render_frame_buffer(self);
        }
    }
    if (is_active(self)) {
        erase_cursor(self);
    }
    self->cursor_position.x = col;
    self->cursor_position.y = row;
    if (is_active(self)) {
        draw_cursor(self);
    }
    return 1;
}

int os_vbuf_cell(const frame_buffer *self, UINT32 row, UINT32 col, UINT8 *out_ch) {
    if (row >= self->rows || col >= self->cols) {
        return 0;
    }
    /* **書き込みと同じ行の対応を使う**(content_screen_row)。
       os_vbuf_visible_row は view_offset を見る「いま見えている行」で、
       さかのぼり中に読むと書いた場所と違う行が返る。
       この口は「アプリが書いた格子を読み返す」ためのものなので、
       書き込み側と同じ対応に揃えた */
    *out_ch = content_screen_row(self, row)[col];
    return 1;
}

void os_vbuf_clear(frame_buffer *self) {
    /* **格子と物理画面の両方を空にする。** 片方だけでは戻ってくる:
       clear_screen は物理ピクセルだけを塗るので、バッファを切り替えて戻ると
       render_frame_buffer が格子から描き直して文字が復活する
       (kernel.c:403-405 のコメントが同じ食い違いの危険を書いている)。
       逆に格子だけ消すと画面には残る。

       **履歴は消さない。** 消すのは「いま見えている 1 画面」であり、
       さかのぼれる過去を捨てる口ではない */
    self->view_offset = 0;
    for (UINT32 row = 0; row < self->rows; row++) {
        UINT8 *line = content_screen_row(self, row);
        for (UINT32 col = 0; col < self->cols; col++) {
            line[col] = 0;
        }
    }
    self->cursor_position.x = 0;
    self->cursor_position.y = 0;
    if (is_active(self)) {
        clear_screen(self);
        draw_cursor(self);
    }
}

/*
 * [#227] カーソルの手前の 1 文字を消す。**格子と画素の両方**を消す。
 *
 * これを framebuffer.c に移したのには 2 つ理由がある。
 *
 * (1) 直す前は interrupt.c の backspace() が erase_cursor を
 *     「セルを黒く塗る道具」として使っていた(interrupt.c:232-238)。
 *     erase_cursor が文字を描き直すようになると、その使い方では
 *     **消したはずの文字がその場で描き直されてしまう。**
 * (2) そもそも直す前には別の食い違いもあった: **格子を更新していなかった**ので、
 *     消した文字は content に残り、面を切り替えて戻る/さかのぼって戻ると
 *     render_frame_buffer が描き直して**復活していた**(os_vbuf_clear の
 *     コメントが書いているのと同じ食い違いである)。
 *
 * 消す責任をこの層に持たせると、母艦の単体試験から呼べるようにもなる
 * (interrupt.c は GDT/IDT と手書きアセンブラを抱えていてリンクできない。#216 と同じ分担)。
 */
int os_vbuf_backspace(frame_buffer *self) {
    if (self->cursor_position.x == 0) {
        /* 行頭では消さない。プロンプト自体を食ってしまうため(従来どおり) */
        return 0;
    }
    if (is_active(self)) {
        erase_cursor(self);   /* いまのカーソルの跡を消す(下に文字があれば描き直される) */
    }
    self->cursor_position.x -= 1;
    content_put(self, self->cursor_position.x, self->cursor_position.y, 0);
    if (is_active(self)) {
        /* 格子を空にしたので、ここの draw_cursor は白い矩形だけを描く */
        draw_cursor(self);
    }
    return 1;
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
