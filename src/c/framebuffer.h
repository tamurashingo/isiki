#ifndef _FRAMEBUFFER_H_
#define _FRAMEBUFFER_H_

#include "types.h"


/** 仮想バッファの数(F1〜F4に割り当てる) */
#define VBUF_COUNT 4

/** グリフの大きさ(font8x16)。桁数・行数はこれで画面サイズを割って求める */
#define VBUF_GLYPH_WIDTH 8
#define VBUF_GLYPH_HEIGHT 16

/**
 * 1バッファあたりの履歴行数の既定値。
 * 実測で (disassemble 'dis-caller) が見出し込み約253行、物理画面が50行なので、
 * 逆アセンブル3〜4回分が遡れる。1280x800(160桁)の場合、
 * 160 * 1000 * 4バッファ = 640KB をboot allocatorから取る。
 */
#define VBUF_DEFAULT_HIST_ROWS 1000


typedef struct _frame_buffer {
    volatile UINT32 *buffer;
    UINT32 width;
    UINT32 height;
    UINT32 pixels_per_scanline;

    /** カーソルの位置。yは**画面内**の行(0〜rows-1)で、履歴上の位置ではない */
    struct _cursor_position {
        UINT32 x;
        UINT32 y;
    } cursor_position;

    /**
     * この仮想バッファの文字グリッド。非アクティブな間もここに保持され、
     * アクティブになった際に画面へ再描画される。
     *
     * hist_rows行 x cols桁のリングバッファで、content[row * cols + col] でアクセスする。
     * 2次元配列ではないのは、桁数がUEFI(GOP)から得た解像度で決まる実行時の値であり、
     * コンパイル時に固定できないため(80も160も256も決め打ちにしない)。
     * 実体はboot allocatorから確保する(GCヒープの外、OS生存期間中保持)。
     */
    UINT8 *content;

    /** 桁数(width / VBUF_GLYPH_WIDTH)。contentの行あたりの要素数 */
    UINT32 cols;
    /** 物理画面に収まる行数(height / VBUF_GLYPH_HEIGHT) */
    UINT32 rows;
    /** contentのリングが保持できる行数(>= rows)。rowsと等しければ履歴なし */
    UINT32 hist_rows;

    /**
     * リング上で最も古い有効行の位置。行が溢れるとここが進む。
     * 論理行 i (0 <= i < count) の実体は content[((base + i) % hist_rows) * cols] にある。
     */
    UINT32 base;
    /** 有効な論理行数(<= hist_rows)。初期値はrows(空行で埋まった1画面ぶん) */
    UINT32 count;
    /**
     * 最下部から何行さかのぼって表示しているか。0 = 最下部(通常の表示)。
     * 0 <= view_offset <= count - rows にクランプされる。
     */
    UINT32 view_offset;


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
     * さかのぼって表示している最中に呼ばれた場合は、まず最下部へ復帰する。
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
 * 実際に使われる履歴行数を返す。画面行数(height / VBUF_GLYPH_HEIGHT)を下回る値を
 * 渡された場合は画面行数まで切り上げる(窓の計算 count - rows が成立しなくなるため)。
 *
 * この切り上げを os_vbuf_content_bytes と initialize_virtual_buffers の**両方が**
 * 通ることで、確保したバイト数と実際に使うバイト数が食い違わないことを保証する。
 * 以前は初期化側だけが切り上げており、呼び出し側が小さい値で確保すると
 * そのまま範囲外へ書き込んでいた。
 * @param height 画面の縦ピクセル数
 * @param hist_rows 希望する履歴行数
 * @return 実際に使われる履歴行数
 */
UINT32 os_vbuf_effective_hist_rows(UINT32 height, UINT32 hist_rows);

/**
 * VBUF_COUNT個ぶんのcontentに必要なバイト数を返す。
 * 確保はframebuffer.cではなく呼び出し側(kernel_main)が行う。boot allocatorは
 * runtime.cの資源であり、framebuffer.cをそこへ依存させるとユニットテストから
 * 単体でリンクできなくなるため(initialize_processesが仮想バッファを受け取るのと同じ分担)。
 * @param width 画面の横ピクセル数
 * @param height 画面の縦ピクセル数
 * @param hist_rows 1バッファあたりの希望する履歴行数(切り上げられることがある)
 * @return 必要なバイト数
 */
UINT64 os_vbuf_content_bytes(UINT32 width, UINT32 height, UINT32 hist_rows);

/**
 * VBUF_COUNT個の仮想frame bufferを初期化する
 * @param base 書き込み先のframe bufferのアドレス(全バッファ共通の物理アドレス)
 * @param width 横幅
 * @param height 高さ
 * @param pixels_per_scanline 1行のピクセル数
 * @param content_area os_vbuf_content_bytesぶんの領域(NULL不可)。VBUF_COUNT個で分割して使う
 * @param hist_rows 1バッファあたりの履歴行数。os_vbuf_effective_hist_rowsで切り上げられる
 * @return 初期状態でアクティブなバッファ(index 0)のアドレス
 */
frame_buffer* initialize_virtual_buffers(UINT64 base, UINT32 width, UINT32 height,
                                         UINT32 pixels_per_scanline,
                                         UINT8 *content_area, UINT32 hist_rows);

/**
 * 現在アクティブな仮想frame bufferを返す
 * @return 現在アクティブな仮想frame buffer
 */
frame_buffer* get_active_frame_buffer(void);

/**
 * アクティブな仮想frame bufferを切り替え、画面を再描画する。
 * view_offsetはバッファごとに保持されるので、戻ってきたときは同じ位置が表示される。
 * @param index 切り替え先のバッファ番号(0〜VBUF_COUNT-1)
 */
void switch_active_frame_buffer(UINT32 index);

/**
 * 画面行screen_rowに表示すべき内容(content上の行の先頭)を返す。view_offsetを考慮する。
 * 描画とテストの両方がこの1箇所の計算を使うことで、窓の位置がずれない。
 * @param self frame buffer
 * @param screen_row 画面内の行(0〜rows-1)
 * @return content上の行の先頭。screen_rowが範囲外なら0
 */
const UINT8 *os_vbuf_visible_row(const frame_buffer *self, UINT32 screen_row);

/**
 * selfがさかのぼれる最大行数(view_offsetの上限)を返す。
 * @param self frame buffer
 * @return count - rows(履歴が無ければ0)
 */
UINT32 os_vbuf_max_view_offset(const frame_buffer *self);

/**
 * アクティブなバッファの表示位置をdelta_rows行ぶん動かし、再描画する。
 * 負なら過去へさかのぼる。範囲外はクランプされる(端で呼んでも何も起きない)。
 * @param delta_rows 動かす行数
 */
void os_vbuf_scroll(INT32 delta_rows);

/**
 * アクティブなバッファの表示位置を最下部へ戻す。
 * @return 実際に戻した場合は1、既に最下部だった場合は0
 */
int os_vbuf_scroll_to_bottom(void);

/**
 * アクティブなバッファの1画面の行数(PageUp/PageDownの移動量)を返す。
 * @return 画面行数
 */
UINT32 os_vbuf_screen_rows(void);


#endif /* _FRAMEBUFFER_H_ */
