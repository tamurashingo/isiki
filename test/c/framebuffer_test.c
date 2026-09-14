#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_assert.h"
#include "types.h"
#include "framebuffer.h"

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

    free(g_pixels);
    free(g_content);
    return g_test_failed ? 1 : 0;
}
