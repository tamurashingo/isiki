#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "lisp.h"
#include "process.h"
#include "reader.h"
#include "stream.h"
#include "stream_lisp.h"
#include "format.h"

// stream.cが参照するos_virtio9p_*は9P経路を使わないため呼ばれないが、
// stream_lisp.c/reader.cとのリンクに必要なので最小限のフェイクを用意する
// (stream_lisp_test.cと同じ方針)。

int os_virtio9p_open(const char *path, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path;
    (void)mode;
    (void)err_msg;
    (void)err_msg_cap;
    *out_fid = 1;
    return 1;
}

int os_virtio9p_create(const char *path, UINT32 perm, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path;
    (void)perm;
    (void)mode;
    (void)err_msg;
    (void)err_msg_cap;
    *out_fid = 1;
    return 1;
}

int os_virtio9p_write_chunk(UINT32 fid, UINT64 offset, const UINT8 *data, UINT32 count,
                             UINT32 *out_written, char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)offset;
    (void)data;
    (void)err_msg;
    (void)err_msg_cap;
    *out_written = count;
    return 1;
}

int os_virtio9p_read_chunk(UINT32 fid, UINT64 offset, UINT32 want,
                            const UINT8 **out_data, UINT32 *out_count,
                            char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)offset;
    (void)want;
    (void)err_msg;
    (void)err_msg_cap;
    *out_data = 0;
    *out_count = 0;
    return 0;
}

int os_virtio9p_close(UINT32 fid, char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)err_msg;
    (void)err_msg_cap;
    return 1;
}

// runtime.c が参照する get_active_frame_buffer のダミー実装
static void dummy_write_string(struct _frame_buffer *self, const char *s) {
    (void)self;
    (void)s;
}

static frame_buffer g_frame_buffer = {
    .write_string = dummy_write_string,
};

frame_buffer* get_active_frame_buffer(void) {
    return &g_frame_buffer;
}

void switch_active_frame_buffer(UINT32 index) {
    (void)index;
}

void enable_timer_irq(void) {
}

void os_repl_step(process_t *proc) {
    (void)proc;
}

void os_wait_for_more_input(process_t *proc) {
    (void)proc;
}

static void dummy_buf_write_char(struct _frame_buffer *self, UINT8 c) {
    (void)self;
    (void)c;
}

static void dummy_buf_write_string(struct _frame_buffer *self, const char *s) {
    (void)self;
    (void)s;
}

static frame_buffer g_buffers[PROCESS_COUNT];

static void setup_buffers() {
    for (UINT32 i = 0; i < PROCESS_COUNT; i++) {
        g_buffers[i].write_char = dummy_buf_write_char;
        g_buffers[i].write_string = dummy_buf_write_string;
    }
}

#define HEAP_SIZE (1024 * 1024)

static void setup_heap() {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
}

/** 文字列出力streamを作り、そこにformat系関数で書き込んだ内容をcstrで取り出すヘルパー */
static lisp_val_t make_out_stream() {
    return cc_create_string_output_stream(nil, nil);
}

static void assert_stream_content(lisp_val_t stream, const char *expected, const char *msg) {
    lisp_val_t s = cc_get_output_stream_string(os_make_cons(stream, nil), nil);
    char buf[256];
    os_string_to_cstr(s, buf, sizeof(buf));
    assert(strcmp(buf, expected) == 0, msg);
}

void test_format_char_writes_char_directly() {
    lisp_val_t stream = make_out_stream();
    cc_format_char(os_make_cons(stream, os_make_cons(os_make_char('Z'), nil)), nil);
    assert_stream_content(stream, "Z", "format-charは文字をそのまま書き込む");
}

void test_format_float_approximates_as_integer_dot_zero() {
    lisp_val_t stream = make_out_stream();
    cc_format_float(os_make_cons(stream, os_make_cons(os_make_fixnum(3), nil)), nil);
    assert_stream_content(stream, "3.0", "format-floatはfixnumを整数+\".0\"として近似出力する");
}

void test_format_integer_supports_various_radixes() {
    lisp_val_t s1 = make_out_stream();
    cc_format_integer(os_make_cons(s1, os_make_cons(os_make_fixnum(150), os_make_cons(os_make_fixnum(2), nil))), nil);
    assert_stream_content(s1, "10010110", "format-integerはradix=2で2進数を出力する");

    lisp_val_t s2 = make_out_stream();
    cc_format_integer(os_make_cons(s2, os_make_cons(os_make_fixnum(493), os_make_cons(os_make_fixnum(8), nil))), nil);
    assert_stream_content(s2, "755", "format-integerはradix=8で8進数を出力する");

    lisp_val_t s3 = make_out_stream();
    cc_format_integer(os_make_cons(s3, os_make_cons(os_make_fixnum(2989), os_make_cons(os_make_fixnum(16), nil))), nil);
    assert_stream_content(s3, "BAD", "format-integerはradix=16で16進数を大文字で出力する");
}

void test_format_object_escape_p_controls_quoting() {
    lisp_val_t s1 = make_out_stream();
    cc_format_object(os_make_cons(s1, os_make_cons(os_make_string("x"), os_make_cons(nil, nil))), nil);
    assert_stream_content(s1, "x", "format-objectはescape-p=nilならダブルクオート無しで出力する");

    lisp_val_t s2 = make_out_stream();
    cc_format_object(os_make_cons(s2, os_make_cons(os_make_string("x"), os_make_cons(g_sym_t, nil))), nil);
    assert_stream_content(s2, "\"x\"", "format-objectはescape-p=tならダブルクオート付きで出力する");
}

void test_format_fresh_line_only_when_not_at_column_zero() {
    lisp_val_t stream = make_out_stream();
    cc_format_fresh_line(os_make_cons(stream, nil), nil);
    assert_stream_content(stream, "", "行頭ではformat-fresh-lineは何も出力しない");

    cc_format_char(os_make_cons(stream, os_make_cons(os_make_char('A'), nil)), nil);
    cc_format_fresh_line(os_make_cons(stream, nil), nil);
    lisp_val_t s = cc_get_output_stream_string(os_make_cons(stream, nil), nil);
    char buf[8];
    os_string_to_cstr(s, buf, sizeof(buf));
    assert(strcmp(buf, "A\n") == 0, "行頭でなければformat-fresh-lineは改行を出力する");
}

void test_format_tab_pads_to_column_or_one_space() {
    lisp_val_t stream = make_out_stream();
    cc_format_char(os_make_cons(stream, os_make_cons(os_make_char('A'), nil)), nil);
    cc_format_tab(os_make_cons(stream, os_make_cons(os_make_fixnum(5), nil)), nil);
    cc_format_char(os_make_cons(stream, os_make_cons(os_make_char('B'), nil)), nil);
    assert_stream_content(stream, "A    B", "format-tabは指定列まで空白を出力する");

    lisp_val_t s2 = make_out_stream();
    cc_format_char(os_make_cons(s2, os_make_cons(os_make_char('A'), nil)), nil);
    cc_format_tab(os_make_cons(s2, os_make_cons(os_make_fixnum(0), nil)), nil);
    assert_stream_content(s2, "A ", "既に列を超えている場合はformat-tabは空白を1つだけ出力する");
}

void test_format_aesthetic_and_sexpr_directives() {
    lisp_val_t stream = make_out_stream();
    cc_format(os_make_cons(stream,
        os_make_cons(os_make_string("The result is ~A and nothing else."),
            os_make_cons(os_make_string("meningitis"), nil))), nil);
    assert_stream_content(stream, "The result is meningitis and nothing else.",
        "~Aはprinc相当(ダブルクオート無し)でobjを出力する");

    // 注: 既存のprint.c(TAG_CHAR)はescaped指定に関わらず文字をそのまま出力する実装のため、
    // 仕様上のprin1表記("#\a")ではなく生の文字"a"が出力される(既存の簡略化、本タスクの対象外)
    lisp_val_t s2 = make_out_stream();
    cc_format(os_make_cons(s2,
        os_make_cons(os_make_string("The results are ~S and ~S."),
            os_make_cons(os_make_fixnum(1), os_make_cons(os_make_char('a'), nil)))), nil);
    assert_stream_content(s2, "The results are 1 and a.",
        "~Sはprin1相当でobjを出力する(CHARのエスケープ表記は既存print.cの制約により非対応)");
}

void test_format_numeric_directives() {
    lisp_val_t s1 = make_out_stream();
    cc_format(os_make_cons(s1, os_make_cons(os_make_string("Binary code ~B"), os_make_cons(os_make_fixnum(150), nil))), nil);
    assert_stream_content(s1, "Binary code 10010110", "~Bは2進数を出力する");

    lisp_val_t s2 = make_out_stream();
    cc_format(os_make_cons(s2, os_make_cons(os_make_string("permission ~O"), os_make_cons(os_make_fixnum(493), nil))), nil);
    assert_stream_content(s2, "permission 755", "~Oは8進数を出力する");

    lisp_val_t s3 = make_out_stream();
    cc_format(os_make_cons(s3, os_make_cons(os_make_string("You ~X ~X"),
        os_make_cons(os_make_fixnum(2989), os_make_cons(os_make_fixnum(64206), nil)))), nil);
    assert_stream_content(s3, "You BAD FACE", "~Xは16進数を大文字で出力する");

    lisp_val_t s4 = make_out_stream();
    cc_format(os_make_cons(s4, os_make_cons(os_make_string("~3R"), os_make_cons(os_make_fixnum(5), nil))), nil);
    assert_stream_content(s4, "12", "~nRは指定したradix(3進数の5=12)で出力する");
}

void test_format_newline_and_tilde_directives() {
    lisp_val_t s1 = make_out_stream();
    cc_format(os_make_cons(s1, os_make_cons(os_make_string("This will be split into~%two lines."), nil)), nil);
    assert_stream_content(s1, "This will be split into\ntwo lines.", "~%は改行を出力する");

    lisp_val_t s2 = make_out_stream();
    cc_format(os_make_cons(s2, os_make_cons(os_make_string("This is a tilde: ~~"), nil)), nil);
    assert_stream_content(s2, "This is a tilde: ~", "~~はチルダ自身を出力する");
}

void test_format_tab_and_freshline_directives_together() {
    lisp_val_t stream = make_out_stream();
    cc_format(os_make_cons(stream, os_make_cons(os_make_string("~&Name ~10Tincome ~20Ttax~%"), nil)), nil);
    cc_format(os_make_cons(stream,
        os_make_cons(os_make_string("~A ~10T~D ~20T~D"),
            os_make_cons(os_make_string("Grummy"),
                os_make_cons(os_make_fixnum(23000), os_make_cons(os_make_fixnum(7500), nil))))), nil);
    assert_stream_content(stream,
        "Name      income    tax\nGrummy    23000     7500",
        "~&/~nT/~%を組み合わせた表形式出力が仕様の例と一致する");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    setup_heap();
    setup_buffers();

    test_format_char_writes_char_directly();
    test_format_float_approximates_as_integer_dot_zero();
    test_format_integer_supports_various_radixes();
    test_format_object_escape_p_controls_quoting();
    test_format_fresh_line_only_when_not_at_column_zero();
    test_format_tab_pads_to_column_or_one_space();
    test_format_aesthetic_and_sexpr_directives();
    test_format_numeric_directives();
    test_format_newline_and_tilde_directives();
    test_format_tab_and_freshline_directives_together();

    return g_test_failed ? 1 : 0;
}
