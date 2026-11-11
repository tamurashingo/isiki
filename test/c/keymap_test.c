#include <stdio.h>
#include "test_assert.h"
#include "types.h"
#include "keymap.h"

/*
 * [#216] **キーの取り扱いの試験。ここまで 1 件も無かった。**
 *
 * 調査(docs/investigation/app-terminal-io.logic.md §9-15)の機械的な根拠:
 *   grep -n "interrupt.c" Makefile     → SRC の 1 行とコメント 2 行だけ
 *   grep -rn "c_keyboard_handler" test/ → 一致なし
 *   ls test/c/ | grep -i interrupt      → 一致なし
 * つまり「変換表・拡張キーの扱い・プロセス切り替えキー・修飾キーは全体が無試験」
 * だった。無試験だった理由は中身ではなく**在りか**で、interrupt.c は
 * GDT/IDT/TSS と手書きアセンブラを抱えているため母艦からリンクできない。
 *
 * #216 で判定を src/c/keymap.c へ切り出したので、**ここで単体でリンクできる。**
 * この試験が見ているのは次の 5 つである:
 *   §1 修飾キーの状態(Shift / Ctrl)
 *   §2 Break コードと拡張キーの前置きの破棄(**変えていないこと**の固定)
 *   §3 変換表より前で奪われるキー(F1〜F4 / Shift+PgUp・PgDn)と**その順序**
 *   §4 変換表(従来どおり通ること)
 *   §5 変換表に載っていないキー(#216 で足した分: 矢印・ESC・Tab・Delete・Ctrl+英字)
 */

/** 1 件ごとに修飾キーの状態を初期化する。しないと前の件の Shift が漏れる */
static void fresh(void) {
    os_keymap_reset();
}

/** scancode を 1 つ食わせて、文字が返ることと、その文字を確かめる */
static void expect_char(UINT8 scancode, UINT32 want, const char *what) {
    key_action_t a = os_keymap_decode(scancode);
    if (a.kind != KEY_ACTION_CHAR || a.value != want) {
        printf("  scancode 0x%02X: kind=%d value=0x%02X / want kind=%d value=0x%02X\n",
               scancode, (int)a.kind, a.value, (int)KEY_ACTION_CHAR, want);
    }
    assert(a.kind == KEY_ACTION_CHAR && a.value == want, what);
}

static void expect_kind(UINT8 scancode, key_action_kind_t want, const char *what) {
    key_action_t a = os_keymap_decode(scancode);
    if (a.kind != want) {
        printf("  scancode 0x%02X: kind=%d / want %d\n", scancode, (int)a.kind, (int)want);
    }
    assert(a.kind == want, what);
}

/* ---- §1 修飾キーの状態 ---- */

void test_shift_state(void) {
    fresh();
    assert(os_keymap_shift_pressed() == 0, "初期状態ではShiftは押されていない");
    expect_kind(0x2A, KEY_ACTION_MODIFIER, "左Shiftの押下は修飾キーの変化");
    assert(os_keymap_shift_pressed() == 1, "押下でShiftが立つ");
    expect_kind(0xAA, KEY_ACTION_MODIFIER, "左Shiftの離しも修飾キーの変化");
    assert(os_keymap_shift_pressed() == 0, "離しでShiftが落ちる");

    expect_kind(0x36, KEY_ACTION_MODIFIER, "右Shiftの押下");
    assert(os_keymap_shift_pressed() == 1, "右Shiftでも立つ");
    expect_kind(0xB6, KEY_ACTION_MODIFIER, "右Shiftの離し");
    assert(os_keymap_shift_pressed() == 0, "右Shiftでも落ちる");
}

void test_ctrl_state(void) {
    /* [#216] **Ctrl の状態はこれまで存在しなかった。**
       Shift の 1 種類しか状態が無く、だから制御キーとの組み合わせは
       キー割り当てとして作れなかった(調査 §9-4) */
    fresh();
    assert(os_keymap_ctrl_pressed() == 0, "初期状態ではCtrlは押されていない");
    expect_kind(0x1D, KEY_ACTION_MODIFIER, "Ctrlの押下は修飾キーの変化");
    assert(os_keymap_ctrl_pressed() == 1, "押下でCtrlが立つ");
    /* 離しコード 0x9D は上位ビットが立っている。**Break コードの破棄より先に
       見ていなければここで落ちる**(Ctrl が押されたままになる) */
    expect_kind(0x9D, KEY_ACTION_MODIFIER, "Ctrlの離しも修飾キーの変化");
    assert(os_keymap_ctrl_pressed() == 0, "離しでCtrlが落ちる");
}

void test_modifiers_are_independent(void) {
    fresh();
    os_keymap_decode(0x2A);   /* Shift 押下 */
    os_keymap_decode(0x1D);   /* Ctrl 押下 */
    assert(os_keymap_shift_pressed() == 1, "両方押せる(Shift)");
    assert(os_keymap_ctrl_pressed() == 1, "両方押せる(Ctrl)");
    os_keymap_decode(0xAA);   /* Shift 離し */
    assert(os_keymap_shift_pressed() == 0, "片方だけ離せる(Shift)");
    assert(os_keymap_ctrl_pressed() == 1, "もう片方は残る(Ctrl)");
}

/* ---- §2 Break コードと拡張キーの前置き ---- */

void test_break_codes_are_dropped(void) {
    fresh();
    /* 修飾キー以外の Break コード(上位ビットが立っているもの)は無視する。
       0x9E = 'a' の離し */
    expect_kind(0x9E, KEY_ACTION_NONE, "英字の離しは何も起こさない");
    expect_kind(0xC8, KEY_ACTION_NONE, "上矢印の離しも何も起こさない");
}

void test_extended_prefix_is_still_dropped(void) {
    /* **#216 では前置きの扱いを変えていない。**
     * 矢印が取れなかったのは前置きを捨てているからではなく、変換表に
     * その番号が載っていなかったからである(調査 §9-2 の「前提の訂正」)。
     * この試験は「変えていない」ことを固定する。もし将来 0xE0 を
     * 状態として持つように変えたら、ここが落ちる */
    fresh();
    expect_kind(0xE0, KEY_ACTION_NONE, "拡張キーの前置きは従来どおり捨てられる");

    /* 前置きを捨てたあと、**矢印の番号は前置き無しで届く**。
       これが #216 の設計の前提そのものである */
    os_keymap_decode(0xE0);
    expect_char(0x48, KEY_CODE_UP, "前置きを捨てても上矢印の番号は届く");
}

/* ---- §3 変換表より前で奪われるキーと、その順序 ---- */

void test_function_keys_switch_process(void) {
    fresh();
    key_action_t a = os_keymap_decode(0x3B);
    assert(a.kind == KEY_ACTION_SWITCH_PROCESS && a.value == 0, "F1はプロセス0へ");
    a = os_keymap_decode(0x3C);
    assert(a.kind == KEY_ACTION_SWITCH_PROCESS && a.value == 1, "F2はプロセス1へ");
    a = os_keymap_decode(0x3E);
    assert(a.kind == KEY_ACTION_SWITCH_PROCESS && a.value == 3, "F4はプロセス3へ");
    /* F5(0x3F)は範囲外。**何も起こさない**(変換表にも無い) */
    expect_kind(0x3F, KEY_ACTION_NONE, "F5は何も起こさない");
}

void test_function_keys_win_over_shift(void) {
    /* Shift を押していても F1〜F4 はプロセス切り替えである。
       変換表より前で奪うという規則は Shift に左右されない */
    fresh();
    os_keymap_decode(0x2A);
    key_action_t a = os_keymap_decode(0x3B);
    assert(a.kind == KEY_ACTION_SWITCH_PROCESS, "Shift+F1でもプロセス切り替え");
}

void test_shift_page_keys_scroll_back(void) {
    fresh();
    os_keymap_decode(0x2A);   /* Shift 押下 */
    key_action_t a = os_keymap_decode(0x49);
    assert(a.kind == KEY_ACTION_SCROLLBACK && a.value == 1, "Shift+PgUpは過去へ");
    a = os_keymap_decode(0x51);
    assert(a.kind == KEY_ACTION_SCROLLBACK && a.value == 0, "Shift+PgDnは未来へ");
}

void test_scrollback_wins_over_the_new_page_keys(void) {
    /* **順序の対照。** #216 で PgUp/PgDn を文字として拾えるようにしたが、
       その分岐は Shift+PgUp/PgDn の**後ろ**に置いてある。
       前に置いてしまうと、さかのぼり表示が効かなくなる(既存の機能の退行)。
       同じキーで、Shift の有無だけで行き先が変わることを見る */
    fresh();
    expect_char(0x49, KEY_CODE_PAGE_UP, "Shiftなしの PgUpは文字として届く");
    os_keymap_decode(0x2A);
    key_action_t a = os_keymap_decode(0x49);
    assert(a.kind == KEY_ACTION_SCROLLBACK, "Shiftありならさかのぼりが勝つ");
}

/* ---- §4 変換表(従来どおり通ること) ---- */

void test_translation_table_still_works(void) {
    fresh();
    expect_char(0x1E, 'a', "英字");
    expect_char(0x02, '1', "数字");
    expect_char(0x39, ' ', "空白");
    expect_char(0x1C, '\n', "改行");
    expect_char(0x0E, '\b', "1文字消し");
    expect_char(0x35, '/', "記号");

    os_keymap_decode(0x2A);   /* Shift 押下 */
    expect_char(0x1E, 'A', "Shiftで大文字");
    expect_char(0x02, '!', "Shiftで記号");
    expect_char(0x35, '?', "Shiftで記号(2)");
}

void test_unmapped_keys_are_still_silent(void) {
    /* 変換表にも特殊キーの表にも無い番号は無反応である。
       **何でも拾う実装になっていないこと**の陰性対照(規則 8)。
       0x3A = CapsLock、0x38 = Alt。どちらも #216 では扱っていない */
    fresh();
    expect_kind(0x3A, KEY_ACTION_NONE, "CapsLockは今も無反応");
    expect_kind(0x38, KEY_ACTION_NONE, "Altは今も無反応");
    expect_kind(0x57, KEY_ACTION_NONE, "F11は今も無反応");
}

/* ---- §5 #216 で足した分 ---- */

void test_arrow_keys_are_delivered(void) {
    /* **これが #216 の主眼。** 変換表を引く前に番号で分岐するだけで取れる */
    fresh();
    expect_char(0x48, KEY_CODE_UP, "上矢印");
    expect_char(0x50, KEY_CODE_DOWN, "下矢印");
    expect_char(0x4B, KEY_CODE_LEFT, "左矢印");
    expect_char(0x4D, KEY_CODE_RIGHT, "右矢印");

    /* 4 つが**別の値**であること(全部同じ値を返す実装では通らない) */
    assert(KEY_CODE_UP != KEY_CODE_DOWN, "上と下は別の値");
    assert(KEY_CODE_LEFT != KEY_CODE_RIGHT, "左と右は別の値");
    assert(KEY_CODE_UP != KEY_CODE_LEFT, "上と左は別の値");
}

void test_navigation_and_editing_keys_are_delivered(void) {
    fresh();
    expect_char(0x01, KEY_CODE_ESCAPE, "脱出キー");
    expect_char(0x0F, KEY_CODE_TAB, "字送りキー");
    expect_char(0x53, KEY_CODE_DELETE, "削除キー");
    expect_char(0x52, KEY_CODE_INSERT, "挿入キー");
    expect_char(0x47, KEY_CODE_HOME, "行頭キー");
    expect_char(0x4F, KEY_CODE_END, "行末キー");
    expect_char(0x49, KEY_CODE_PAGE_UP, "前ページキー");
    expect_char(0x51, KEY_CODE_PAGE_DOWN, "次ページキー");
}

void test_special_codes_do_not_collide_with_ascii(void) {
    /* 矢印などに割り当てた番号が、変換表の文字と重なっていないこと。
       重なるとアプリが「右矢印」と「ある文字」を区別できない。
       **1 バイトに収めている**理由は keymap.h の注記(read-char が
       os_make_char((UINT8)ch) でゼロ拡張するため 0x80〜0xFF が通る) */
    assert(KEY_CODE_UP >= 0x80, "矢印はASCIIの外");
    assert(KEY_CODE_DOWN >= 0x80, "矢印はASCIIの外(下)");
    assert(KEY_CODE_LEFT >= 0x80, "矢印はASCIIの外(左)");
    assert(KEY_CODE_RIGHT >= 0x80, "矢印はASCIIの外(右)");
    assert(KEY_CODE_INSERT <= 0xFF, "1バイトに収まっている");
    /* ESC/Tab/Delete は ASCII の定位置を使う(アプリが自前の定数を
       持たずに書けるようにするため) */
    assert(KEY_CODE_ESCAPE == 0x1B, "脱出キーはASCIIのESC");
    assert(KEY_CODE_TAB == 0x09, "字送りキーはASCIIのTab");
    assert(KEY_CODE_DELETE == 0x7F, "削除キーはASCIIのDEL");
}

void test_special_codes_are_pinned_to_exact_values(void) {
    /* **値そのものを固定する。**
     *
     * Lisp 側(src/lisp/init.lisp の *key-up* など)は同じ数を**書き写して**
     * 持っている。番地や記号を Lisp へ渡す手段が無いので、C の定数を
     * Lisp から引くことはできない(調査 refs §7)。
     * だから「ずれたら気づく」仕掛けをここに置く:
     * **C 側の値を動かすとこの試験が落ちる**ので、Lisp 側も一緒に直すことになる。
     *
     * 値を変えるときは src/lisp/init.lisp の対応する defglobal も直すこと。 */
    assert(KEY_CODE_UP == 0x80, "上矢印は0x80");
    assert(KEY_CODE_DOWN == 0x81, "下矢印は0x81");
    assert(KEY_CODE_LEFT == 0x82, "左矢印は0x82");
    assert(KEY_CODE_RIGHT == 0x83, "右矢印は0x83");
    assert(KEY_CODE_HOME == 0x84, "行頭は0x84");
    assert(KEY_CODE_END == 0x85, "行末は0x85");
    assert(KEY_CODE_PAGE_UP == 0x86, "前ページは0x86");
    assert(KEY_CODE_PAGE_DOWN == 0x87, "次ページは0x87");
    assert(KEY_CODE_INSERT == 0x88, "挿入は0x88");
}

void test_ctrl_letter_makes_control_chars(void) {
    /* [#216] Ctrl + 英字を制御文字にする。これが無いと
       「制御キーとの組み合わせ」はキー割り当てとして作れない(調査 §9-4) */
    fresh();
    os_keymap_decode(0x1D);          /* Ctrl 押下 */
    expect_char(0x1E, 1, "C-a は 1");
    expect_char(0x2D, 24, "C-x は 24");
    expect_char(0x11, 23, "C-w は 23");
    expect_char(0x1C, '\n', "C-Enter は改行のまま(英字でないものは素通し)");

    /* Shift も一緒に押していても C-a は 1 である(素の表の文字から作る) */
    os_keymap_decode(0x2A);
    expect_char(0x1E, 1, "C-S-a も 1");

    /* Ctrl を離せば普通の英字に戻る */
    os_keymap_decode(0x9D);
    os_keymap_decode(0xAA);
    expect_char(0x1E, 'a', "Ctrlを離せば英字に戻る");
}

void test_ctrl_does_not_swallow_navigation_keys(void) {
    /* 特殊キーの分岐は Ctrl より前にある。Ctrl+矢印は矢印として届く
       (Ctrl+矢印に別の意味を与えるのは #216 の仕事ではない) */
    fresh();
    os_keymap_decode(0x1D);
    expect_char(0x48, KEY_CODE_UP, "Ctrl+上矢印も上矢印として届く");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    test_shift_state();
    test_ctrl_state();
    test_modifiers_are_independent();

    test_break_codes_are_dropped();
    test_extended_prefix_is_still_dropped();

    test_function_keys_switch_process();
    test_function_keys_win_over_shift();
    test_shift_page_keys_scroll_back();
    test_scrollback_wins_over_the_new_page_keys();

    test_translation_table_still_works();
    test_unmapped_keys_are_still_silent();

    test_arrow_keys_are_delivered();
    test_navigation_and_editing_keys_are_delivered();
    test_special_codes_do_not_collide_with_ascii();
    test_special_codes_are_pinned_to_exact_values();
    test_ctrl_letter_makes_control_chars();
    test_ctrl_does_not_swallow_navigation_keys();

    return g_test_failed ? 1 : 0;
}
