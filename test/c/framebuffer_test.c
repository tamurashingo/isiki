#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_assert.h"
#include "types.h"
#include "framebuffer.h"
#include "font8x16.h"   /* 字形の立っているビット数を導出するために使う(#227。規則13) */

/*
 * 仮想バッファの履歴リングと表示窓の検証。
 *
 * framebuffer.c は runtime.c に依存しないので単体でリンクできる(content の確保は
 * 呼び出し側の責任にしてあるため。framebuffer.h の os_vbuf_content_bytes 参照)。
 * ここで見るのは主に境界条件で、「端で余計に動かない」「一周しても窓がずれない」
 * の2つが崩れると画面に出るものが静かに間違う。
 */

/* 直近の setup() が確保した領域。次の setup() で解放する */
static UINT32 *g_pixels = 0;
static UINT8 *g_content = 0;

/**
 * 指定した解像度と履歴行数で仮想バッファを初期化する。
 * 物理フレームバッファは実際に書き込まれるので、本物と同じ大きさで確保する。
 */
static frame_buffer *setup(UINT32 width, UINT32 height, UINT32 hist_rows) {
    free(g_pixels);
    free(g_content);
    g_pixels = (UINT32 *)calloc((size_t)width * height, sizeof(UINT32));
    g_content = (UINT8 *)calloc((size_t)os_vbuf_content_bytes(width, height, hist_rows), 1);
    return initialize_virtual_buffers((UINT64)(UINTN)g_pixels, width, height, width,
                                      g_content, hist_rows);
}

/** 文字列を出力してから改行する */
static void write_line(frame_buffer *fb, const char *s) {
    fb->write_string(fb, s);
    fb->write_char(fb, '\n');
}

/** 画面行 screen_row に見えている内容を NUL 終端文字列として取り出す */
static void row_text(const frame_buffer *fb, UINT32 screen_row, char *out, UINT32 out_size) {
    const UINT8 *row = os_vbuf_visible_row(fb, screen_row);
    UINT32 n = 0;
    if (row != 0) {
        while (n + 1 < out_size && n < fb->cols && row[n] != 0) {
            out[n] = (char)row[n];
            n++;
        }
    }
    out[n] = 0;
}

static void expect_row(const frame_buffer *fb, UINT32 screen_row, const char *want, const char *what) {
    char got[512];
    row_text(fb, screen_row, got, sizeof(got));
    if (strcmp(got, want) != 0) {
        printf("  row %u: got \"%s\" / want \"%s\"\n", screen_row, got, want);
    }
    assert(strcmp(got, want) == 0, what);
}

/* ---- 初期状態 ---- */

void test_initial_state(void) {
    frame_buffer *fb = setup(640, 160, 20);   /* 80桁 x 10行、履歴20行 */
    assert(fb->cols == 80, "桁数は width / 8");
    assert(fb->rows == 10, "行数は height / 16");
    assert(fb->hist_rows == 20, "履歴行数は指定どおり");
    assert(fb->count == fb->rows, "初期の有効行数は画面1枚ぶん(空行で埋まっている)");
    assert(fb->base == 0, "初期のリング先頭は0");
    assert(fb->view_offset == 0, "初期は最下部を表示している");
    assert(os_vbuf_max_view_offset(fb) == 0, "履歴がまだ無いのでさかのぼれない");
    expect_row(fb, 0, "", "初期状態の画面は空");
}

/* ---- リングの前進 ---- */

void test_ring_advances_without_wrapping(void) {
    /* 画面10行・履歴20行。9回改行するまではスクロールが起きない */
    frame_buffer *fb = setup(640, 160, 20);
    for (int i = 0; i < 9; i++) {
        char line[16];
        sprintf(line, "L%d", i);
        write_line(fb, line);
    }
    assert(fb->count == 10, "画面内に収まっている間は有効行数が増えない");
    assert(fb->base == 0, "リング先頭も動かない");
    expect_row(fb, 0, "L0", "先頭行がそのまま見えている");
    expect_row(fb, 8, "L8", "9行目も見えている");
    expect_row(fb, 9, "", "カーソルのいる最終行は空");
}

void test_ring_grows_then_drops_oldest(void) {
    frame_buffer *fb = setup(640, 160, 20);
    /* L0..L29 の30行。最初のスクロールは L9 の改行で起き、以降 L10..L29 で
       20回起きるので、スクロール回数は合計21回 */
    for (int i = 0; i < 30; i++) {
        char line[16];
        sprintf(line, "L%d", i);
        write_line(fb, line);
    }
    assert(fb->count == 20, "有効行数は hist_rows で頭打ちになる");
    assert(fb->base == 11, "頭打ち後はリング先頭が進んで最古の行が捨てられる");

    /* 画面には最後の10行が見えている。最終行はカーソルのいる空行 */
    expect_row(fb, 0, "L21", "画面先頭は L21");
    expect_row(fb, 8, "L29", "最後に書いた行が画面の下から2行目");
    expect_row(fb, 9, "", "カーソルのいる最終行は空");
}

/* ---- 表示窓のスクロール ---- */

void test_scroll_back_shows_history(void) {
    frame_buffer *fb = setup(640, 160, 20);
    for (int i = 0; i < 30; i++) {
        char line[16];
        sprintf(line, "L%d", i);
        write_line(fb, line);
    }
    assert(os_vbuf_max_view_offset(fb) == 10, "さかのぼれるのは count - rows 行");

    os_vbuf_scroll(-10);
    assert(fb->view_offset == 10, "10行さかのぼった");
    /* リングは一周しているので、ここで top の計算が壊れていると内容がずれる */
    expect_row(fb, 0, "L11", "最も古い残存行が画面先頭に来る");
    expect_row(fb, 9, "L20", "窓は10行ぶん");

    os_vbuf_scroll(10);
    assert(fb->view_offset == 0, "戻ると最下部");
    expect_row(fb, 0, "L21", "最下部の表示は元どおり");
}

void test_scroll_clamps_at_both_ends(void) {
    frame_buffer *fb = setup(640, 160, 20);

    /* 空バッファ: さかのぼれる履歴が無い */
    os_vbuf_scroll(-10);
    assert(fb->view_offset == 0, "履歴が無ければ、さかのぼってもクランプされる");

    for (int i = 0; i < 30; i++) {
        char line[16];
        sprintf(line, "L%d", i);
        write_line(fb, line);
    }

    /* 上限を大きく超える要求 */
    os_vbuf_scroll(-1000);
    assert(fb->view_offset == 10, "上限(count - rows)でクランプされる");
    os_vbuf_scroll(-1000);
    assert(fb->view_offset == 10, "端でさらに要求しても動かない");

    /* 下限を大きく超える要求 */
    os_vbuf_scroll(1000);
    assert(fb->view_offset == 0, "最下部でクランプされる");
    os_vbuf_scroll(1000);
    assert(fb->view_offset == 0, "端でさらに要求しても動かない");
}

void test_scroll_to_bottom_reports_whether_it_moved(void) {
    frame_buffer *fb = setup(640, 160, 20);
    for (int i = 0; i < 30; i++) {
        char line[16];
        sprintf(line, "L%d", i);
        write_line(fb, line);
    }
    assert(os_vbuf_scroll_to_bottom() == 0, "既に最下部なら0を返す(再描画しない)");
    os_vbuf_scroll(-5);
    assert(os_vbuf_scroll_to_bottom() == 1, "さかのぼっていれば1を返す");
    assert(fb->view_offset == 0, "最下部へ戻っている");
}

void test_output_while_scrolled_snaps_to_bottom(void) {
    frame_buffer *fb = setup(640, 160, 20);
    for (int i = 0; i < 30; i++) {
        char line[16];
        sprintf(line, "L%d", i);
        write_line(fb, line);
    }
    os_vbuf_scroll(-10);
    assert(fb->view_offset == 10, "さかのぼっている");

    write_line(fb, "NEW");
    assert(fb->view_offset == 0, "出力が来たら最下部へスナップする");
    expect_row(fb, 8, "NEW", "新しい行が見えている");
}

void test_output_to_other_buffer_keeps_this_view_offset(void) {
    /* view_offset はバッファごとに保持される。他のバッファへの出力で動いてはならない */
    frame_buffer *fb0 = setup(640, 160, 20);
    for (int i = 0; i < 30; i++) {
        char line[16];
        sprintf(line, "L%d", i);
        write_line(fb0, line);
    }
    os_vbuf_scroll(-7);
    assert(fb0->view_offset == 7, "バッファ0はさかのぼっている");

    /* バッファ1は同じ content 領域の別スライスを使う。get_active_frame_buffer が
       返すのは0番のままなので、1番へは直接書く */
    frame_buffer *fb1 = fb0 + 1;
    write_line(fb1, "OTHER");
    assert(fb0->view_offset == 7, "他バッファへの出力ではこちらの view_offset に触れない");
    assert(fb1->view_offset == 0, "書かれた側は最下部のまま");
}

void test_switch_preserves_view_offset_per_buffer(void) {
    frame_buffer *fb0 = setup(640, 160, 20);
    for (int i = 0; i < 30; i++) {
        char line[16];
        sprintf(line, "L%d", i);
        write_line(fb0, line);
    }
    os_vbuf_scroll(-6);
    assert(fb0->view_offset == 6, "バッファ0をさかのぼる");

    switch_active_frame_buffer(1);
    assert(get_active_frame_buffer() == fb0 + 1, "バッファ1へ切り替わった");
    assert((fb0 + 1)->view_offset == 0, "切替え先は最下部を表示している");

    switch_active_frame_buffer(0);
    assert(get_active_frame_buffer() == fb0, "バッファ0へ戻った");
    assert(fb0->view_offset == 6, "戻ってきたら同じ位置が保持されている");
    expect_row(fb0, 0, "L15", "表示内容も同じ位置");
}

/* ---- 解像度非依存 ---- */

void test_various_column_counts(void) {
    /* cols を決め打ちしていないこと。80 / 160 / 240 のいずれでも同じ結果になる */
    const UINT32 widths[] = { 640, 1280, 1920 };
    const UINT32 want_cols[] = { 80, 160, 240 };
    for (unsigned k = 0; k < 3; k++) {
        frame_buffer *fb = setup(widths[k], 160, 20);
        char msg[96];
        sprintf(msg, "width=%u のとき cols=%u", widths[k], want_cols[k]);
        assert(fb->cols == want_cols[k], msg);

        for (int i = 0; i < 30; i++) {
            char line[16];
            sprintf(line, "L%d", i);
            write_line(fb, line);
        }
        os_vbuf_scroll(-10);
        sprintf(msg, "width=%u でも履歴の内容は同じ", widths[k]);
        char got[512];
        row_text(fb, 0, got, sizeof(got));
        assert(strcmp(got, "L11") == 0, msg);
    }
}

void test_content_bytes_scales_with_width_and_history(void) {
    assert(os_vbuf_content_bytes(1280, 800, 1000) == (UINT64)160 * 1000 * VBUF_COUNT,
           "必要バイト数は 桁数 x 履歴行数 x バッファ数");
    assert(os_vbuf_content_bytes(640, 160, 10) == (UINT64)80 * 10 * VBUF_COUNT,
           "小さい構成でも同じ式");
    /* 切り上げが確保側にも効くこと。ここが食い違うと初期化が範囲外へ書き込む */
    assert(os_vbuf_effective_hist_rows(160, 3) == 10, "画面行数未満は切り上げられる");
    assert(os_vbuf_effective_hist_rows(160, 50) == 50, "画面行数以上はそのまま");
    assert(os_vbuf_content_bytes(640, 160, 3) == os_vbuf_content_bytes(640, 160, 10),
           "切り上げ後の行数でバイト数が決まる");
}

/* ---- 履歴の確保に失敗した場合 ---- */

void test_no_history_still_works(void) {
    /* hist_rows == rows。スクロールバックは効かないが、コンソールとしては動く */
    frame_buffer *fb = setup(640, 160, 10);
    assert(fb->hist_rows == 10, "履歴行数が画面行数と等しい");
    assert(os_vbuf_max_view_offset(fb) == 0, "さかのぼれない");

    for (int i = 0; i < 30; i++) {
        char line[16];
        sprintf(line, "L%d", i);
        write_line(fb, line);
    }
    expect_row(fb, 0, "L21", "通常の出力は従来どおり動く");
    expect_row(fb, 8, "L29", "最新行も正しい");

    os_vbuf_scroll(-5);
    assert(fb->view_offset == 0, "さかのぼり要求はクランプされる");
    assert(os_vbuf_scroll_to_bottom() == 0, "既に最下部");
}

void test_hist_rows_below_screen_is_raised(void) {
    /* 画面行数未満を渡されたら画面行数まで切り上げる。そうしないと窓の計算
       (count - rows)が成立しない */
    frame_buffer *fb = setup(640, 160, 3);
    assert(fb->hist_rows == 10, "画面行数まで切り上げられる");
    assert(fb->count == 10, "有効行数も画面行数");
}

/* ---- 行末の折り返し(off-by-one の修正確認) ---- */

void test_line_wraps_exactly_at_cols(void) {
    /* 以前は `cursor_x(self) > self->width` で判定しており、cols+1 文字目が
       content へ書かれてしまっていた。render は cols 桁で切るため、バッファを
       切り替えて戻ると その1文字が消えるという食い違いが起きていた */
    frame_buffer *fb = setup(640, 160, 20);
    for (UINT32 i = 0; i < fb->cols; i++) {
        fb->write_char(fb, 'A');
    }
    assert(fb->cursor_position.y == 1, "cols 文字ちょうどで次の行へ折り返す");
    assert(fb->cursor_position.x == 0, "折り返し後は行頭");

    fb->write_char(fb, 'B');
    expect_row(fb, 1, "B", "cols+1 文字目は次の行の先頭に入る");

    char got[512];
    row_text(fb, 0, got, sizeof(got));
    assert(strlen(got) == fb->cols, "1行目はちょうど cols 文字");
}

void test_wrapped_line_survives_buffer_switch(void) {
    /* 折り返しの食い違いが直っていることを、実際の症状(切替えで消える)で確認する */
    frame_buffer *fb = setup(640, 160, 20);
    for (UINT32 i = 0; i < fb->cols; i++) {
        fb->write_char(fb, 'A');
    }
    fb->write_char(fb, 'B');

    char before[512];
    row_text(fb, 0, before, sizeof(before));
    switch_active_frame_buffer(1);
    switch_active_frame_buffer(0);
    char after[512];
    row_text(fb, 0, after, sizeof(after));
    assert(strcmp(before, after) == 0, "バッファを切り替えて戻っても内容が変わらない");
}


/* ---- 位置指定・読み出し・消去(#216) ----
 *
 * **3 つを同じ節で見るのは、読み出しが無いと位置指定が効いていることを
 * 確かめられないからである**(規則 8 の陽性対照)。
 * 「書いた場所を読み返したら同じ文字が返る」が陽性、
 * 「置いていない場所は空白」「範囲外は 0 を返す」が陰性にあたる。 */

void test_set_cursor_moves_where_the_next_char_lands(void) {
    frame_buffer *fb = setup(640, 160, 20);   /* 80桁 x 10行 */

    /* 【陽性対照】位置を指定して書いたら、その位置の格子から読み返せる */
    assert(os_vbuf_set_cursor(fb, 3, 5) == 1, "画面内なら置ける");
    assert(fb->cursor_position.y == 3, "行が入る");
    assert(fb->cursor_position.x == 5, "桁が入る");
    fb->write_char(fb, 'X');

    UINT8 ch = 0;
    assert(os_vbuf_cell(fb, 3, 5, &ch) == 1, "書いたマスは読める");
    assert(ch == 'X', "書いた文字が返る");

    /* 【陰性対照】隣は空白のまま。位置指定を無視して先頭に書いていたら
       (0,0) に 'X' が入り、ここが崩れる */
    assert(os_vbuf_cell(fb, 3, 4, &ch) == 1, "隣も読める");
    assert(ch == 0, "書いていない桁は空白(0)");
    assert(os_vbuf_cell(fb, 0, 0, &ch) == 1, "左上も読める");
    assert(ch == 0, "左上には何も書かれていない");

    /* 書いたあとカーソルは1つ進んでいる(write_char の約束は変えていない) */
    assert(fb->cursor_position.x == 6, "書いたら桁が1つ進む");
}

void test_set_cursor_rejects_out_of_range(void) {
    frame_buffer *fb = setup(640, 160, 20);   /* 80桁 x 10行 */
    os_vbuf_set_cursor(fb, 2, 2);

    /* 範囲外は 0 を返し、**カーソルを動かさない**。
       content_put(framebuffer.c:180-182)は範囲外を黙って捨てるが、
       この口は呼び出し側に見せる(そうしないと Lisp 側が
       「効いたのか効かなかったのか」を言えない) */
    assert(os_vbuf_set_cursor(fb, 10, 0) == 0, "行が画面行数以上なら置けない");
    assert(os_vbuf_set_cursor(fb, 0, 80) == 0, "桁が桁数以上なら置けない");
    assert(fb->cursor_position.y == 2, "失敗しても行は動かない");
    assert(fb->cursor_position.x == 2, "失敗しても桁は動かない");

    /* 端ちょうどは入る(境界の off-by-one) */
    assert(os_vbuf_set_cursor(fb, 9, 79) == 1, "右下の端は置ける");
}

void test_cell_rejects_out_of_range(void) {
    frame_buffer *fb = setup(640, 160, 20);
    UINT8 ch = 0xEE;
    assert(os_vbuf_cell(fb, 10, 0, &ch) == 0, "行が範囲外なら読めない");
    assert(ch == 0xEE, "読めなかったときは格納先に触らない");
    assert(os_vbuf_cell(fb, 0, 80, &ch) == 0, "桁が範囲外なら読めない");
    assert(ch == 0xEE, "こちらも触らない");
}

void test_cell_reads_the_page_that_writes_go_to(void) {
    frame_buffer *fb = setup(640, 160, 20);   /* 80桁 x 10行、履歴20行 */
    /* 履歴を進めてから書く。**書き込みと同じ行の対応で読めること**を見る
       (os_vbuf_visible_row は view_offset を見るので、この口とは別物である) */
    for (UINT32 i = 0; i < 15; i++) {
        write_line(fb, "pad");
    }
    os_vbuf_set_cursor(fb, 4, 1);
    fb->write_char(fb, 'Z');

    UINT8 ch = 0;
    assert(os_vbuf_cell(fb, 4, 1, &ch) == 1, "履歴が進んだあとでも読める");
    assert(ch == 'Z', "書いた文字が返る");

    /* さかのぼっても、読めるのは**書き込み先の1画面**である */
    os_vbuf_scroll(-5);
    assert(fb->view_offset == 5, "さかのぼっている");
    ch = 0;
    assert(os_vbuf_cell(fb, 4, 1, &ch) == 1, "さかのぼり中でも読める");
    assert(ch == 'Z', "返るのは書き込み先の1画面の内容");
    os_vbuf_scroll_to_bottom();
}

void test_set_cursor_snaps_back_to_bottom(void) {
    frame_buffer *fb = setup(640, 160, 20);
    for (UINT32 i = 0; i < 15; i++) {
        write_line(fb, "pad");
    }
    os_vbuf_scroll(-5);
    assert(fb->view_offset == 5, "さかのぼっている");

    /* さかのぼったまま位置を指定すると「指した場所」と「書かれる場所」が
       食い違うので、入口で最下部へ戻す(write_char と同じ) */
    assert(os_vbuf_set_cursor(fb, 1, 1) == 1, "置ける");
    assert(fb->view_offset == 0, "位置指定は最下部へ戻してから置く");
}

void test_clear_empties_both_grid_and_screen(void) {
    frame_buffer *fb = setup(640, 160, 20);
    write_line(fb, "hello");
    write_line(fb, "world");

    UINT8 ch = 0;
    assert(os_vbuf_cell(fb, 0, 0, &ch) == 1 && ch == 'h', "消す前は文字がある");

    os_vbuf_clear(fb);

    /* **格子が空になっている。** 物理画面だけを塗る clear_screen と違い、
       消したものが描き直しで戻ってこない(kernel.c の食い違いの危険) */
    UINT32 nonblank = 0;
    for (UINT32 row = 0; row < fb->rows; row++) {
        for (UINT32 col = 0; col < fb->cols; col++) {
            UINT8 c = 0xFF;
            assert(os_vbuf_cell(fb, row, col, &c) == 1, "全マス読める");
            if (c != 0) {
                nonblank++;
            }
        }
    }
    assert(nonblank == 0, "消したあとは1マスも残らない");
    assert(fb->cursor_position.x == 0, "カーソルは左上へ戻る");
    assert(fb->cursor_position.y == 0, "カーソルは左上へ戻る(行)");
    assert(fb->view_offset == 0, "さかのぼりは解除される");

    /* 【陰性対照】消したのは「いま見えている1画面」だけで、**履歴は残る**。
       count が縮んでいないことで見る(縮めていたら過去が捨てられている) */
    assert(fb->count == fb->rows, "この時点の有効行数は画面1枚ぶん");

    /* 消したあとも普通に書ける */
    fb->write_char(fb, 'a');
    ch = 0;
    assert(os_vbuf_cell(fb, 0, 0, &ch) == 1 && ch == 'a', "消したあとは左上から書ける");
}

void test_clear_keeps_history(void) {
    frame_buffer *fb = setup(640, 160, 20);   /* 10行、履歴20行 */
    for (UINT32 i = 0; i < 15; i++) {
        write_line(fb, "pad");
    }
    UINT32 count_before = fb->count;
    os_vbuf_clear(fb);
    assert(fb->count == count_before, "消しても履歴の行数は減らない");
    assert(os_vbuf_max_view_offset(fb) == count_before - fb->rows, "さかのぼれる量も変わらない");
    /* さかのぼれば過去がまだ見える(消したのは見えている1画面だけ)。
       **1 行さかのぼったときの画面行 0** が、消した 1 画面の 1 つ手前の行である
       (画面行 1〜9 は消した 1 画面の中に入るので空のまま。ここを間違えて
       最下行で見ると「消えている」と読めてしまう) */
    os_vbuf_scroll(-1);
    expect_row(fb, 0, "pad", "1行さかのぼると消す前の行が見える");
    expect_row(fb, 1, "", "消した1画面の中は空のまま");
    os_vbuf_scroll_to_bottom();
}

/* ---- 画素を見る(#227) ----
 *
 * **このファイルには画素を見る試験が1件も無かった。** カーソルの試験は 23 箇所
 * あり、`%%TERM-READ-CELL`(#216)や editor_ / filer_ 系 の画面検査もあったが、
 * どれも**論理的な文字の格子**しか読んでいなかった。だから
 * 「真っ白な矩形がカーソルの下の文字を潰している」「カーソルが通り過ぎたマスの
 * 文字が黒く消えている」ことを**誰も見ていなかった**(#227。利用者が実機の
 * エディタで踏んだ)。これは「検出器が黙って効かなくなる」の画素版である。
 *
 * ここの計器は `fb->buffer` を直接読む。framebuffer_test は母艦で走り、
 * 物理フレームバッファは setup() が calloc で本物と同じ大きさを確保しているので、
 * 描いた画素をそのまま数えられる。
 *
 * **陽性対照は 2 段で置いてある**(規則 8):
 *  (1) 同じ 1 周の中で、直す前の塗り方(old_draw_cursor_pixels /
 *      old_erase_cursor_pixels)を同じマスへ当て、**同じ計器が 0 を返す**ことを見る。
 *  (2) framebuffer.c の draw_cursor / erase_cursor から字形の描き直しを外し、
 *      os_vbuf_backspace から content_put を外した版(= 直す前の姿)で
 *      この試験を走らせると **9 件が NG になる**ことを確かめた(#227 の PR に出力あり)。
 *      (1) だけでは「計器が区別できる」までしか言えず、
 *      「直す前の framebuffer.c なら落ちる」は (2) で示す。
 */

/** セル1つぶんの画素数。期待値を書き写さずに字形の立っているビット数から導出する */
#define CELL_PIXELS (VBUF_GLYPH_WIDTH * VBUF_GLYPH_HEIGHT)

/** セル(row,col)の 8x16 の画素のうち、色が color のものを数える */
static UINT32 cell_count_color(const frame_buffer *fb, UINT32 row, UINT32 col, UINT32 color) {
    UINT32 x = col * VBUF_GLYPH_WIDTH;
    UINT32 y = row * VBUF_GLYPH_HEIGHT;
    UINT32 n = 0;
    for (UINT32 dy = 0; dy < VBUF_GLYPH_HEIGHT; dy++) {
        for (UINT32 dx = 0; dx < VBUF_GLYPH_WIDTH; dx++) {
            if (fb->buffer[(y + dy) * fb->pixels_per_scanline + (x + dx)] == color) {
                n++;
            }
        }
    }
    return n;
}

/**
 * 文字 c の字形で立っているビットの数。**フォント表から導出する**(規則13)。
 * 「'c' は 24 画素」などと数えて書き写すと、フォントを差し替えた日に腐る。
 */
static UINT32 glyph_bits(char c) {
    UINT32 n = 0;
    for (UINT32 dy = 0; dy < VBUF_GLYPH_HEIGHT; dy++) {
        for (UINT32 dx = 0; dx < VBUF_GLYPH_WIDTH; dx++) {
            if (font8x16[(unsigned char)c][dy] & (0x80 >> dx)) {
                n++;
            }
        }
    }
    return n;
}

/**
 * セル(row,col)に「文字 c が fg 色で、残りが bg 色で」描かれているかを判定する。
 * **これがこのファイルで初めての画素の計器である。**
 * 真っ白(全画素 fg)でも真っ黒(全画素 bg)でも 0 を返す ——
 * つまり #227 の 2 つの症状はどちらもここで落ちる。
 * @return 文字が見えるなら1
 */
static int cell_shows_glyph(const frame_buffer *fb, UINT32 row, UINT32 col,
                            char c, UINT32 bg, UINT32 fg) {
    UINT32 bits = glyph_bits(c);
    return cell_count_color(fb, row, col, fg) == bits
        && cell_count_color(fb, row, col, bg) == CELL_PIXELS - bits;
}

/* ---- 直す前の実装の写し(陽性対照。規則 8) ----
 *
 * #227 より前の framebuffer.c:71-100 の draw_cursor / erase_cursor を写してある。
 * 元は `cursor_x(self)` / `cursor_y(self)` を使っていたが、その中身は
 * `cursor_position.x * VBUF_GLYPH_WIDTH` / `.y * VBUF_GLYPH_HEIGHT` なので、
 * ここでは見たいマスを (row, col) で直に渡す形にしてある(塗る画素は同じ)。
 *
 * **これを同じ計器(cell_shows_glyph)にかけて落ちることを、直した版が通る
 * 同じ試験の中で示す。** 「直したから通る」ではなく「直す前なら落ちる」。
 */
static void old_draw_cursor_pixels(frame_buffer *self, UINT32 row, UINT32 col) {
    UINT32 x = col * VBUF_GLYPH_WIDTH;
    UINT32 y = row * VBUF_GLYPH_HEIGHT;
    for (UINT32 dy = 0; dy < VBUF_GLYPH_HEIGHT; dy++) {
        for (UINT32 dx = 0; dx < VBUF_GLYPH_WIDTH; dx++) {
            self->buffer[(y + dy) * self->pixels_per_scanline + (x + dx)] = 0x00FFFFFF;
        }
    }
}

static void old_erase_cursor_pixels(frame_buffer *self, UINT32 row, UINT32 col) {
    UINT32 x = col * VBUF_GLYPH_WIDTH;
    UINT32 y = row * VBUF_GLYPH_HEIGHT;
    for (UINT32 dy = 0; dy < VBUF_GLYPH_HEIGHT; dy++) {
        for (UINT32 dx = 0; dx < VBUF_GLYPH_WIDTH; dx++) {
            self->buffer[(y + dy) * self->pixels_per_scanline + (x + dx)] = 0x00000000;
        }
    }
}

/** セル1つを好きな色で塗る(さかのぼり中に「触っていない」ことを見るための見張り) */
static void paint_cell(frame_buffer *fb, UINT32 row, UINT32 col, UINT32 color) {
    UINT32 x = col * VBUF_GLYPH_WIDTH;
    UINT32 y = row * VBUF_GLYPH_HEIGHT;
    for (UINT32 dy = 0; dy < VBUF_GLYPH_HEIGHT; dy++) {
        for (UINT32 dx = 0; dx < VBUF_GLYPH_WIDTH; dx++) {
            fb->buffer[(y + dy) * fb->pixels_per_scanline + (x + dx)] = color;
        }
    }
}

void test_pixels_written_char_is_fg_on_bg(void) {
    frame_buffer *fb = setup(640, 160, 20);   /* 80桁 x 10行 */

    /* 計器の足場。字形が空っぽの文字では「見える/見えない」を区別できないので、
       使う 3 文字にビットが立っていることを先に見ておく(陰性対照の足場) */
    assert(glyph_bits('a') > 0 && glyph_bits('b') > 0 && glyph_bits('c') > 0,
           "a/b/c の字形にはビットが立っている(空白ではない)");
    assert(glyph_bits(0) == 0, "格子の初期値 0 の字形は全ビット 0(描いても何も出ない)");
    assert(glyph_bits(' ') == 0, "空白 ' ' の字形も全ビット 0");

    fb->write_string(fb, "abc");

    /* 書いた3文字は白地ではなく**黒地に白**で描かれている */
    assert(cell_shows_glyph(fb, 0, 0, 'a', VBUF_COLOR_BG, VBUF_COLOR_FG) == 1, "'a' が描かれている");
    assert(cell_shows_glyph(fb, 0, 1, 'b', VBUF_COLOR_BG, VBUF_COLOR_FG) == 1, "'b' が描かれている");
    assert(cell_shows_glyph(fb, 0, 2, 'c', VBUF_COLOR_BG, VBUF_COLOR_FG) == 1, "'c' が描かれている");

    /* カーソルは次のマスにいる。そこは格子が空なので真っ白な矩形になる */
    assert(fb->cursor_position.x == 3, "カーソルは 'c' の右");
    assert(cell_count_color(fb, 0, 3, VBUF_COLOR_FG) == CELL_PIXELS,
           "空のマスのカーソルは白い矩形");

    /* 【陰性対照】書いていないマスは真っ黒のまま */
    assert(cell_count_color(fb, 0, 4, VBUF_COLOR_BG) == CELL_PIXELS, "書いていないマスは黒");
}

void test_pixels_cursor_shows_the_char_underneath(void) {
    frame_buffer *fb = setup(640, 160, 20);
    fb->write_string(fb, "abc");

    /* 利用者の操作: 'c' の右から左を押して 'c' の位置へ */
    assert(os_vbuf_set_cursor(fb, 0, 2) == 1, "'c' の位置へカーソルを置ける");

    /* 【完了条件】カーソルの下の文字が見える(反転表示: 白地に黒の字形) */
    assert(cell_shows_glyph(fb, 0, 2, 'c', VBUF_COLOR_FG, VBUF_COLOR_BG) == 1,
           "カーソルの下の 'c' が見える(白地に黒)");
    assert(cell_count_color(fb, 0, 2, VBUF_COLOR_FG) == CELL_PIXELS - glyph_bits('c'),
           "白い画素は字形のビット以外のぶんだけ");
    assert(cell_count_color(fb, 0, 2, VBUF_COLOR_FG) != CELL_PIXELS,
           "真っ白に潰れていない");

    /* 【陽性対照】同じマスに**直す前の塗り方**を当てると、同じ計器が落ちる。
       真っ白になり、cell_shows_glyph は 0 を返す */
    old_draw_cursor_pixels(fb, 0, 2);
    assert(cell_count_color(fb, 0, 2, VBUF_COLOR_FG) == CELL_PIXELS,
           "【陽性対照】直す前は真っ白に塗っていた");
    assert(cell_shows_glyph(fb, 0, 2, 'c', VBUF_COLOR_FG, VBUF_COLOR_BG) == 0,
           "【陽性対照】直す前の塗り方では文字が見えない(この計器が落ちる)");
}

void test_pixels_moving_cursor_leaves_the_char_behind(void) {
    frame_buffer *fb = setup(640, 160, 20);
    fb->write_string(fb, "abc");

    /* 報告された操作そのまま: 左、左 */
    assert(os_vbuf_set_cursor(fb, 0, 2) == 1, "左を押して 'c' の位置へ");
    assert(os_vbuf_set_cursor(fb, 0, 1) == 1, "さらに左を押して 'b' の位置へ");

    /* 【完了条件】通り過ぎたマスの 'c' が画面に残っている(黒地に白へ戻る)。
       直す前はここが真っ黒で、「c が画面から消える」に見えていた */
    assert(cell_shows_glyph(fb, 0, 2, 'c', VBUF_COLOR_BG, VBUF_COLOR_FG) == 1,
           "通り過ぎたマスの 'c' が描き直されている");
    assert(cell_count_color(fb, 0, 2, VBUF_COLOR_FG) == glyph_bits('c'),
           "白い画素は字形のビットのぶんだけ");
    assert(cell_count_color(fb, 0, 2, VBUF_COLOR_BG) != CELL_PIXELS,
           "真っ黒に消えていない");

    /* いるマスの 'b' は反転して見えている */
    assert(cell_shows_glyph(fb, 0, 1, 'b', VBUF_COLOR_FG, VBUF_COLOR_BG) == 1,
           "いまカーソルがいる 'b' も見える");

    /* 報告どおり**格子は壊れていない**(壊れていたのは画素だけ) */
    UINT8 ch = 0;
    assert(os_vbuf_cell(fb, 0, 2, &ch) == 1 && ch == 'c', "格子の 'c' はそのまま");

    /* 【陽性対照】同じマスに**直す前の消し方**を当てると、同じ計器が落ちる */
    old_erase_cursor_pixels(fb, 0, 2);
    assert(cell_count_color(fb, 0, 2, VBUF_COLOR_BG) == CELL_PIXELS,
           "【陽性対照】直す前は真っ黒に塗って文字を描き直さなかった");
    assert(cell_shows_glyph(fb, 0, 2, 'c', VBUF_COLOR_BG, VBUF_COLOR_FG) == 0,
           "【陽性対照】直す前の消し方では文字が消える(この計器が落ちる)");
}

void test_pixels_cursor_on_blank_cell_is_a_solid_block(void) {
    frame_buffer *fb = setup(640, 160, 20);

    /* 格子の初期値 0(未書き込み)の上のカーソル。
       **初期化だけではカーソルは描かれない**(initialize_virtual_buffers は画素に
       触らない)。kernel_main が起動時にこの 2 つを呼ぶのと同じ順で呼ぶ */
    fb->clear_screen(fb);
    fb->draw_cursor(fb);
    assert(cell_count_color(fb, 0, 0, VBUF_COLOR_FG) == CELL_PIXELS,
           "未書き込み(格子 0)のマスのカーソルは白い矩形");

    /* アプリが空白 ' '(0x20)を書いたマスも同じ見た目になる。
       **格子は 0 と ' ' を別の値として持つ**が、どちらの字形も全ビット 0 なので
       反転表示の見え方は同じである(0 のときだけ字形を引かない。framebuffer.c の
       cursor_cell_char のコメント参照) */
    fb->write_char(fb, ' ');
    UINT8 ch = 0xEE;
    assert(os_vbuf_cell(fb, 0, 0, &ch) == 1 && ch == ' ',
           "空白を書いたマスの格子は ' '(0x20)で、0 ではない");
    assert(os_vbuf_set_cursor(fb, 0, 0) == 1, "書いた空白の上へ戻す");
    assert(cell_count_color(fb, 0, 0, VBUF_COLOR_FG) == CELL_PIXELS,
           "空白(' ')のマスのカーソルも白い矩形");
}

void test_pixels_backspace_clears_grid_and_screen(void) {
    frame_buffer *fb = setup(640, 160, 20);
    fb->write_string(fb, "abc");

    assert(os_vbuf_backspace(fb) == 1, "カーソルの手前の1文字を消せる");
    assert(fb->cursor_position.x == 2, "カーソルが1桁戻る");

    UINT8 ch = 0xEE;
    assert(os_vbuf_cell(fb, 0, 2, &ch) == 1 && ch == 0, "**格子からも**消える");
    assert(cell_count_color(fb, 0, 2, VBUF_COLOR_FG) == CELL_PIXELS,
           "消した位置はカーソルの白い矩形になる");

    /* 【陰性対照】消しすぎていない。隣の 'b' は描かれたまま */
    assert(cell_shows_glyph(fb, 0, 1, 'b', VBUF_COLOR_BG, VBUF_COLOR_FG) == 1,
           "隣の 'b' は消えない");

    /* 面を切り替えて戻っても 'c' は**復活しない**。格子を更新していなかった
       直す前の backspace(interrupt.c)は、ここで文字が戻ってきていた */
    switch_active_frame_buffer(1);
    switch_active_frame_buffer(0);
    assert(os_vbuf_cell(fb, 0, 2, &ch) == 1 && ch == 0, "戻ってきても格子は空のまま");
    assert(cell_shows_glyph(fb, 0, 2, 'c', VBUF_COLOR_BG, VBUF_COLOR_FG) == 0,
           "描き直しで 'c' が復活しない");
    /* 戻ってきたカーソルは消した位置(0,2)にいる。格子が空なので白い矩形 */
    assert(cell_count_color(fb, 0, 2, VBUF_COLOR_FG) == CELL_PIXELS,
           "戻ってきてもカーソルは消した位置の白い矩形");
    assert(cell_shows_glyph(fb, 0, 1, 'b', VBUF_COLOR_BG, VBUF_COLOR_FG) == 1,
           "隣の 'b' は描き直しでも黒地に白のまま");

    /* 行頭では何もしない(プロンプトを食わないため) */
    assert(os_vbuf_set_cursor(fb, 0, 0) == 1, "行頭へ置く");
    assert(os_vbuf_backspace(fb) == 0, "行頭では消さない");
    assert(fb->cursor_position.x == 0, "行頭のままで動かない");
}

void test_pixels_cursor_not_drawn_while_scrolled_back(void) {
    frame_buffer *fb = setup(640, 160, 20);
    for (UINT32 i = 0; i < 15; i++) {
        write_line(fb, "pad");
    }
    os_vbuf_scroll(-5);
    assert(fb->view_offset == 5, "さかのぼっている");

    /* さかのぼり中はカーソルの実位置が窓の外にあるので描かない・消さない
       (framebuffer.c の view_offset の番人。**この振る舞いは #227 で変えていない**)。
       見張りの色で塗っておき、画素が1つも書き換わらないことで見る */
    UINT32 row = fb->cursor_position.y;
    UINT32 col = fb->cursor_position.x;
    const UINT32 sentinel = 0x00123456u;
    paint_cell(fb, row, col, sentinel);

    fb->draw_cursor(fb);
    assert(cell_count_color(fb, row, col, sentinel) == CELL_PIXELS,
           "さかのぼり中の draw_cursor は画素に触らない");
    fb->erase_cursor(fb);
    assert(cell_count_color(fb, row, col, sentinel) == CELL_PIXELS,
           "さかのぼり中の erase_cursor も画素に触らない");

    /* 最下部へ戻せば描き直される(ここで初めてカーソルが出る) */
    assert(os_vbuf_scroll_to_bottom() == 1, "最下部へ戻る");
    assert(cell_count_color(fb, row, col, sentinel) == 0, "戻ると描き直される");
    assert(cell_count_color(fb, row, col, VBUF_COLOR_FG) == CELL_PIXELS,
           "戻った先の空のマスにカーソルの白い矩形が出る");
}

void test_cursor_is_per_buffer_not_shared(void) {
    /* 4面は**物理画面を共有し、カーソルは面ごとに持つ**。
       だから描画はすべて is_active の番人の内側にある */
    frame_buffer *fb0 = setup(640, 160, 20);
    fb0->write_string(fb0, "abc");

    switch_active_frame_buffer(1);
    frame_buffer *fb1 = get_active_frame_buffer();
    assert(fb1 != fb0, "面ごとに別の frame_buffer がある");
    assert(fb1->buffer == fb0->buffer, "物理画面は共有している");
    assert(fb1->cursor_position.x == 0 && fb1->cursor_position.y == 0,
           "切り替え先のカーソルは自分の位置(共有されていない)");

    os_vbuf_set_cursor(fb1, 5, 7);
    assert(fb0->cursor_position.x == 3, "他の面でカーソルを動かしてもこちらは動かない");
    assert(fb0->cursor_position.y == 0, "行も動かない");

    /* 戻ってくると自分の格子から描き直され、カーソルも自分の位置に出る */
    switch_active_frame_buffer(0);
    assert(cell_shows_glyph(fb0, 0, 0, 'a', VBUF_COLOR_BG, VBUF_COLOR_FG) == 1,
           "戻ってくると 'a' が描き直される");
    assert(cell_count_color(fb0, 0, 3, VBUF_COLOR_FG) == CELL_PIXELS,
           "カーソルは自分の面の位置(0,3)に出る");
    /* 【陰性対照】他の面で動かした位置(5,7)にはカーソルが出ていない */
    assert(cell_count_color(fb0, 5, 7, VBUF_COLOR_BG) == CELL_PIXELS,
           "他の面のカーソル位置には何も出ない");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    test_initial_state();
    test_ring_advances_without_wrapping();
    test_ring_grows_then_drops_oldest();
    test_scroll_back_shows_history();
    test_scroll_clamps_at_both_ends();
    test_scroll_to_bottom_reports_whether_it_moved();
    test_output_while_scrolled_snaps_to_bottom();
    test_output_to_other_buffer_keeps_this_view_offset();
    test_switch_preserves_view_offset_per_buffer();
    test_various_column_counts();
    test_content_bytes_scales_with_width_and_history();
    test_no_history_still_works();
    test_hist_rows_below_screen_is_raised();
    test_line_wraps_exactly_at_cols();
    test_wrapped_line_survives_buffer_switch();

    /* [#216] 位置指定・読み出し・消去 */
    test_set_cursor_moves_where_the_next_char_lands();
    test_set_cursor_rejects_out_of_range();
    test_cell_rejects_out_of_range();
    test_cell_reads_the_page_that_writes_go_to();
    test_set_cursor_snaps_back_to_bottom();
    test_clear_empties_both_grid_and_screen();
    test_clear_keeps_history();

    /* [#227] 画素を見る試験。カーソルがセルの中身を壊していないことを
       fb->buffer から直接確かめる(それまで画素の計器は0件だった) */
    test_pixels_written_char_is_fg_on_bg();
    test_pixels_cursor_shows_the_char_underneath();
    test_pixels_moving_cursor_leaves_the_char_behind();
    test_pixels_cursor_on_blank_cell_is_a_solid_block();
    test_pixels_backspace_clears_grid_and_screen();
    test_pixels_cursor_not_drawn_while_scrolled_back();
    test_cursor_is_per_buffer_not_shared();

    free(g_pixels);
    free(g_content);
    return g_test_failed ? 1 : 0;
}
