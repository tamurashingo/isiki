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
#include "stream_lisp.h"

// stream.cが参照するos_virtio9p_open/read_chunk/closeをフェイク実装に差し替え、
// インメモリのバイト列を返すことで9P経由の入力streamを検証する(stream_test.cと同じ方針)。

#define FAKE_DATA_MAX 256

static UINT8 g_fake_data[FAKE_DATA_MAX];
static UINT32 g_fake_data_len = 0;
static int g_fake_open_fail = 0;

static void set_fake_data(const char *s) {
    UINT32 n = 0;
    while (s[n] != '\0' && n < FAKE_DATA_MAX) {
        g_fake_data[n] = (UINT8)s[n];
        n++;
    }
    g_fake_data_len = n;
}

static void reset_fake_state(void) {
    g_fake_open_fail = 0;
}

int os_virtio9p_open(const char *path, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path;
    if (g_fake_open_fail) {
        if (err_msg != 0 && err_msg_cap > 0) {
            snprintf(err_msg, err_msg_cap, "fake open failure");
        }
        return 0;
    }
    *out_fid = 1;
    return 1;
}

int os_virtio9p_read_chunk(UINT32 fid, UINT64 offset, UINT32 want,
                            const UINT8 **out_data, UINT32 *out_count,
                            char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)want;
    (void)err_msg;
    (void)err_msg_cap;
    UINT32 off = (UINT32)offset;
    *out_data = g_fake_data + off;
    *out_count = g_fake_data_len - off;
    return 1;
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

// process.c が参照する switch_active_frame_buffer のダミー実装
void switch_active_frame_buffer(UINT32 index) {
    (void)index;
}

// process.c(process_scheduler_start/process_trampoline_c)が参照する
// interrupt.c/repl.cの関数のダミー実装
void enable_timer_irq(void) {
}

void os_repl_step(process_t *proc) {
    (void)proc;
}

// reader.c の ensure_data が proc->stdout_buffer 経由でプロンプトを書き込むための
// 各プロセス用ダミーバッファ(通常時は何もしない)
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

// OPEN-OUTPUT-STREAM経由の画面出力を検証するための、書き込まれた文字を記録するバッファ
#define CAPTURE_MAX 64
static char g_captured[CAPTURE_MAX];
static UINT32 g_captured_len = 0;

static void capture_write_char(struct _frame_buffer *self, UINT8 c) {
    (void)self;
    if (g_captured_len < CAPTURE_MAX) {
        g_captured[g_captured_len++] = (char)c;
    }
}

void os_wait_for_more_input(process_t *proc) {
    (void)proc;
}

#define HEAP_SIZE (1024 * 1024)

static void setup_heap() {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
}

void test_open_output_stream_write_char_and_close() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    proc->stdout_buffer->write_char = capture_write_char;
    g_captured_len = 0;

    lisp_val_t stream = cc_open_output_stream(nil, nil);
    assert((stream & TAG_MASK) == TAG_INSTANCE, "open-output-streamはINSTANCEを返す");

    lisp_val_t args1 = os_make_cons(os_make_char('A'), os_make_cons(stream, nil));
    lisp_val_t r1 = cc_write_char(args1, nil);
    assert(r1 == os_make_char('A'), "write-charは書き込んだCHAR自身を返す");
    assert(g_captured_len == 1 && g_captured[0] == 'A', "画面へ'A'が書き込まれる");

    lisp_val_t args2 = os_make_cons(os_make_char('B'), os_make_cons(stream, nil));
    cc_write_char(args2, nil);
    assert(g_captured_len == 2 && g_captured[1] == 'B', "2文字目'B'も続けて書き込まれる");

    cc_close(os_make_cons(stream, nil), nil);
    UINT32 before_close_len = g_captured_len;
    lisp_val_t args3 = os_make_cons(os_make_char('C'), os_make_cons(stream, nil));
    cc_write_char(args3, nil);
    assert(g_captured_len == before_close_len, "close後のwrite-charは画面に書き込まれない");
}

void test_open_input_stream_read_char_reads_fake_data() {
    reset_fake_state();
    set_fake_data("hi");

    lisp_val_t path = os_make_string("fake/path");
    lisp_val_t stream = cc_open_input_stream(os_make_cons(path, nil), nil);
    assert((stream & TAG_MASK) == TAG_INSTANCE, "open-input-streamはINSTANCEを返す");

    lisp_val_t args = os_make_cons(stream, nil);
    lisp_val_t c1 = cc_read_char(args, nil);
    assert(c1 == os_make_char('h'), "1文字目'h'が読める");

    lisp_val_t c2 = cc_read_char(args, nil);
    assert(c2 == os_make_char('i'), "2文字目'i'が読める");

    lisp_val_t c3 = cc_read_char(args, nil);
    assert(c3 == nil, "EOFに達したらnilが返る");

    cc_close(args, nil);
}

void test_open_input_stream_open_failure_returns_eval_error() {
    reset_fake_state();
    g_fake_open_fail = 1;

    lisp_val_t path = os_make_string("fake/missing");
    lisp_val_t result = cc_open_input_stream(os_make_cons(path, nil), nil);
    assert(result == g_sym_eval_error, "openに失敗したらg_sym_eval_errorが返る");
}

void test_close_then_read_char_returns_nil() {
    reset_fake_state();
    set_fake_data("xyz");

    lisp_val_t path = os_make_string("fake/path");
    lisp_val_t stream = cc_open_input_stream(os_make_cons(path, nil), nil);
    lisp_val_t args = os_make_cons(stream, nil);

    cc_close(args, nil);
    lisp_val_t r = cc_read_char(args, nil);
    assert(r == nil, "close後のread-charはnilを返す");
}

void test_read_parses_sexpr_from_stream() {
    reset_fake_state();
    set_fake_data("(1 2 3)");

    lisp_val_t path = os_make_string("fake/path");
    lisp_val_t stream = cc_open_input_stream(os_make_cons(path, nil), nil);
    lisp_val_t args = os_make_cons(stream, nil);

    lisp_val_t form = cc_read(args, nil);
    assert(cc_car(form) == os_make_fixnum(1), "readで読んだリストの1番目は1");
    assert(cc_car(cc_cdr(form)) == os_make_fixnum(2), "readで読んだリストの2番目は2");
    assert(cc_car(cc_cdr(cc_cdr(form))) == os_make_fixnum(3), "readで読んだリストの3番目は3");
    assert(cc_cdr(cc_cdr(cc_cdr(form))) == nil, "readで読んだリストの終端はnil");

    cc_close(args, nil);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    setup_heap();
    setup_buffers();

    test_open_output_stream_write_char_and_close();
    test_open_input_stream_read_char_reads_fake_data();
    test_open_input_stream_open_failure_returns_eval_error();
    test_close_then_read_char_returns_nil();
    test_read_parses_sexpr_from_stream();

    return g_test_failed ? 1 : 0;
}
