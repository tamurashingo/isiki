#include "terminal.h"
#include "framebuffer.h"
#include "process.h"
#include "lisp.h"

/* [#216] 詳しい設計の理由は terminal.h に書いてある。
 *
 * ここが触る表示面は **アクティブな面ではなく「呼び出したプロセスの面」** である
 * (current->stdout_buffer)。理由: エコーは自分の面を触るのに、1 文字消す操作だけが
 * アクティブな面を触っており、いまは同じ番号なので一致しているが分けたときに
 * 食い違う(調査 §9-12)。**新しい口は最初から自分の面に揃える。** */
static frame_buffer *term_target(void) {
    return get_current_process()->stdout_buffer;
}

/**
 * 引数を「画面の行・桁として使える非負の整数」として取り出す。
 *
 * **ここが守りの唯一の入口である**(apps/inspect.lisp が無検査の 3 窓口に
 * 唯一の呼び出し口を作っているのと同じ作法。#214)。
 * FIXNUM でない値・負の値をそのまま下へ流すと、framebuffer 側は
 * UINT32 として受けるので負数が巨大な値に化け、範囲検査をすり抜ける形に
 * なりうる。fixnum は 2 の補数ではなく**符号 + 絶対値**なので、
 * 単にシフトしただけでは符号が落ちない(documents の既知の罠)。
 *
 * @param val 引数の値
 * @param out 取り出した値の格納先
 * @return 使えるなら1、そうでなければ0
 */
static int term_index_arg(lisp_val_t val, UINT32 *out) {
    if ((val & TAG_MASK) != TAG_FIXNUM) {
        return 0;
    }
    if (os_fixnum_is_negative(val)) {
        return 0;
    }
    UINT64 mag = os_fixnum_magnitude(val);
    if (mag > 0xFFFFFFFFULL) {
        return 0;
    }
    *out = (UINT32)mag;
    return 1;
}

lisp_val_t cc_term_size(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    frame_buffer *fb = term_target();
    /* 行数・桁数は起動時の解像度から決まる実行時の値である
       (framebuffer.c: cols = width/8, rows = height/16)。固定値ではないので、
       アプリが自前の定数を持たずに済むようにここで渡す。
       **確保を 2 回する。** 1 つめを 2 つめの確保を跨いで保護する
       (C の引数評価順に依存した入れ子は書かない。pitfalls 原則 4) */
    lisp_val_t cols = os_make_fixnum(fb->cols);
    GC_PROTECT(cols);
    lisp_val_t tail = os_make_cons(cols, nil);
    GC_PROTECT(tail);
    lisp_val_t rows = os_make_fixnum(fb->rows);
    GC_PROTECT(rows);
    return os_make_cons(rows, tail);
}

lisp_val_t cc_term_set_cursor(lisp_val_t args, lisp_val_t env) {
    (void)env;
    UINT32 row, col;
    if (!term_index_arg(cc_car(args), &row)) {
        return nil;
    }
    if (!term_index_arg(cc_car(cc_cdr(args)), &col)) {
        return nil;
    }
    return os_vbuf_set_cursor(term_target(), row, col) ? g_sym_t : nil;
}

lisp_val_t cc_term_clear(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    os_vbuf_clear(term_target());
    return g_sym_t;
}

lisp_val_t cc_term_read_cell(lisp_val_t args, lisp_val_t env) {
    (void)env;
    UINT32 row, col;
    if (!term_index_arg(cc_car(args), &row)) {
        return nil;
    }
    if (!term_index_arg(cc_car(cc_cdr(args)), &col)) {
        return nil;
    }
    UINT8 ch = 0;
    if (!os_vbuf_cell(term_target(), row, col, &ch)) {
        return nil;
    }
    /* 格子の空白は 0 で持っている(字形を描かないため)。Lisp へは
       **空白文字として**返す。コード 0 の文字を返すと、アプリ側が
       「空きかどうか」を調べるのに 0 と空白の 2 通りを見なければならない */
    if (ch == 0) {
        ch = ' ';
    }
    return os_make_char(ch);
}

lisp_val_t cc_term_raw_input(lisp_val_t args, lisp_val_t env) {
    (void)env;
    /* 引数が nil なら切る、それ以外なら入れる。**返すのは切り替える前の状態**で、
       呼び出し側が元へ戻せるようにしてある */
    int on = (args != nil) && (cc_car(args) != nil);
    return process_set_raw_input(get_current_process(), on) ? g_sym_t : nil;
}

void os_register_terminal_primitives(void) {
    os_set_function(os_make_symbol("%%TERM-SIZE"), os_make_native_function((lisp_addr_t)(void *)cc_term_size), global_environment);
    os_set_function(os_make_symbol("%%TERM-SET-CURSOR"), os_make_native_function((lisp_addr_t)(void *)cc_term_set_cursor), global_environment);
    os_set_function(os_make_symbol("%%TERM-CLEAR"), os_make_native_function((lisp_addr_t)(void *)cc_term_clear), global_environment);
    os_set_function(os_make_symbol("%%TERM-READ-CELL"), os_make_native_function((lisp_addr_t)(void *)cc_term_read_cell), global_environment);
    os_set_function(os_make_symbol("%%TERM-RAW-INPUT"), os_make_native_function((lisp_addr_t)(void *)cc_term_raw_input), global_environment);
}
