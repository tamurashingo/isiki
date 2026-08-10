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

int os_virtio9p_open(const char *path, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path;
    (void)mode;
    if (g_fake_open_fail) {
        if (err_msg != 0 && err_msg_cap > 0) {
            snprintf(err_msg, err_msg_cap, "fake open failure");
        }
        return 0;
    }
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

// process.c(spawn)が参照するinterrupt.cのget_fpu_default_stateのダミー実装。
// FXSAVE領域の初期値はこのテストの対象外なので、ゼロ埋めの512byteバッファを返すだけにする
static UINT8 g_fake_fpu_default_state[512] __attribute__((aligned(16)));

const void *get_fpu_default_state(void) {
    return g_fake_fpu_default_state;
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

void test_open_stream_p_input_stream_p_output_stream_p() {
    reset_fake_state();
    set_fake_data("hi");

    lisp_val_t in_stream = cc_open_input_stream(os_make_cons(os_make_string("fake/path"), nil), nil);
    lisp_val_t out_stream = cc_open_output_stream(nil, nil);

    assert(cc_open_stream_p(os_make_cons(in_stream, nil), nil) == g_sym_t, "open直後のstreamはopen-stream-pが真");
    assert(cc_input_stream_p(os_make_cons(in_stream, nil), nil) == g_sym_t, "9P入力streamはinput-stream-pが真");
    assert(cc_output_stream_p(os_make_cons(in_stream, nil), nil) == nil, "9P入力streamはoutput-stream-pが偽");
    assert(cc_input_stream_p(os_make_cons(out_stream, nil), nil) == nil, "画面出力streamはinput-stream-pが偽");
    assert(cc_output_stream_p(os_make_cons(out_stream, nil), nil) == g_sym_t, "画面出力streamはoutput-stream-pが真");

    cc_close(os_make_cons(in_stream, nil), nil);
    assert(cc_open_stream_p(os_make_cons(in_stream, nil), nil) == nil, "close後はopen-stream-pが偽になる");
}

void test_open_output_file_and_io_file_use_open_then_create_fallback() {
    reset_fake_state();

    lisp_val_t stream1 = cc_open_output_file(os_make_cons(os_make_string("fake/existing"), nil), nil);
    assert((stream1 & TAG_MASK) == TAG_INSTANCE, "既存ファイルへのopen-output-fileはSTREAMを返す");
    cc_close(os_make_cons(stream1, nil), nil);

    g_fake_open_fail = 1; // open失敗→createへフォールバック(os_virtio9p_createは常に成功するフェイク)
    lisp_val_t stream2 = cc_open_output_file(os_make_cons(os_make_string("fake/new"), nil), nil);
    assert((stream2 & TAG_MASK) == TAG_INSTANCE, "新規ファイルへのopen-output-fileはcreateにフォールバックしてSTREAMを返す");
    cc_close(os_make_cons(stream2, nil), nil);

    lisp_val_t stream3 = cc_open_io_file(os_make_cons(os_make_string("fake/new-io"), nil), nil);
    assert((stream3 & TAG_MASK) == TAG_INSTANCE, "open-io-fileも同様にcreateへフォールバックしてSTREAMを返す");
    cc_close(os_make_cons(stream3, nil), nil);

    reset_fake_state();
}

void test_finish_output_flushes_and_returns_nil() {
    reset_fake_state();

    lisp_val_t stream = cc_open_output_file(os_make_cons(os_make_string("fake/path"), nil), nil);
    lisp_val_t args = os_make_cons(stream, nil);
    cc_write_char(os_make_cons(os_make_char('A'), os_make_cons(stream, nil)), nil);

    lisp_val_t result = cc_finish_output(args, nil);
    assert(result == nil, "finish-outputはnilを返す");

    cc_close(args, nil);
}

void test_string_input_stream_reads_content() {
    lisp_val_t stream = cc_create_string_input_stream(os_make_cons(os_make_string("ab"), nil), nil);
    lisp_val_t args = os_make_cons(stream, nil);

    assert(cc_read_char(args, nil) == os_make_char('a'), "string-input-streamの1文字目'a'が読める");
    assert(cc_read_char(args, nil) == os_make_char('b'), "string-input-streamの2文字目'b'が読める");
    assert(cc_read_char(args, nil) == nil, "string-input-streamの末尾はEOFでnil");
}

void test_string_output_stream_accumulates_and_resets() {
    lisp_val_t stream = cc_create_string_output_stream(nil, nil);
    lisp_val_t args = os_make_cons(stream, nil);

    cc_write_char(os_make_cons(os_make_char('x'), os_make_cons(stream, nil)), nil);
    cc_write_char(os_make_cons(os_make_char('y'), os_make_cons(stream, nil)), nil);

    lisp_val_t s1 = cc_get_output_stream_string(args, nil);
    char buf1[8];
    os_string_to_cstr(s1, buf1, sizeof(buf1));
    assert(strcmp(buf1, "xy") == 0, "get-output-stream-stringは書き込まれた内容を返す");

    lisp_val_t s2 = cc_get_output_stream_string(args, nil);
    char buf2[8];
    os_string_to_cstr(s2, buf2, sizeof(buf2));
    assert(strcmp(buf2, "") == 0, "get-output-stream-stringは呼ぶたびに内部バッファをリセットする");
}

void test_preview_char_does_not_advance_cursor() {
    lisp_val_t stream = cc_create_string_input_stream(os_make_cons(os_make_string("ab"), nil), nil);
    lisp_val_t args = os_make_cons(stream, nil);

    assert(cc_preview_char(args, nil) == os_make_char('a'), "preview-charは先頭文字を返す");
    assert(cc_preview_char(args, nil) == os_make_char('a'), "preview-charを連続で呼んでも同じ文字が返る(カーソルは進まない)");
    assert(cc_read_char(args, nil) == os_make_char('a'), "read-charはpreview-charされた文字を消費する");
    assert(cc_read_char(args, nil) == os_make_char('b'), "続く2文字目'b'を読める");
}

void test_read_line_reads_up_to_newline() {
    lisp_val_t stream = cc_create_string_input_stream(os_make_cons(os_make_string("foo\nbar"), nil), nil);
    lisp_val_t args = os_make_cons(stream, nil);

    lisp_val_t line1 = cc_read_line(args, nil);
    char buf1[16];
    os_string_to_cstr(line1, buf1, sizeof(buf1));
    assert(strcmp(buf1, "foo") == 0, "read-lineは改行を含まない1行目'foo'を返す");

    lisp_val_t line2 = cc_read_line(args, nil);
    char buf2[16];
    os_string_to_cstr(line2, buf2, sizeof(buf2));
    assert(strcmp(buf2, "bar") == 0, "read-lineは末尾改行の無い2行目'bar'も返す");

    assert(cc_read_line(args, nil) == nil, "read-lineは即EOFでnilを返す");
}

void test_stream_ready_p_is_always_true() {
    lisp_val_t stream = cc_create_string_input_stream(os_make_cons(os_make_string(""), nil), nil);
    assert(cc_stream_ready_p(os_make_cons(stream, nil), nil) == g_sym_t, "stream-ready-pは常にtrueのスタブ");
}

void test_read_byte_and_write_byte_wrap_char_ops() {
    lisp_val_t in_stream = cc_create_string_input_stream(os_make_cons(os_make_string("A"), nil), nil);
    lisp_val_t byte_val = cc_read_byte(os_make_cons(in_stream, nil), nil);
    assert(byte_val == os_make_fixnum(65), "read-byteは文字コードのFIXNUMを返す('A'=65)");
    assert(cc_read_byte(os_make_cons(in_stream, nil), nil) == nil, "read-byteもEOFでnilを返す");

    lisp_val_t out_stream = cc_create_string_output_stream(nil, nil);
    lisp_val_t args = os_make_cons(os_make_fixnum(66), os_make_cons(out_stream, nil));
    lisp_val_t written = cc_write_byte(args, nil);
    assert(written == os_make_fixnum(66), "write-byteは書き込んだ値自身を返す");

    lisp_val_t s = cc_get_output_stream_string(os_make_cons(out_stream, nil), nil);
    char buf[4];
    os_string_to_cstr(s, buf, sizeof(buf));
    assert(strcmp(buf, "B") == 0, "write-byteで書き込んだ66('B')が文字として反映される");
}

void test_probe_file_reflects_open_success() {
    reset_fake_state();
    assert(cc_probe_file(os_make_cons(os_make_string("fake/exists"), nil), nil) == g_sym_t,
           "openできるパスはprobe-fileが真");

    g_fake_open_fail = 1;
    assert(cc_probe_file(os_make_cons(os_make_string("fake/missing"), nil), nil) == nil,
           "openできないパスはprobe-fileが偽");
    reset_fake_state();
}

void test_file_position_and_set_file_position_on_string_stream() {
    lisp_val_t stream = cc_create_string_input_stream(os_make_cons(os_make_string("abc"), nil), nil);
    lisp_val_t args = os_make_cons(stream, nil);

    assert(cc_file_position(args, nil) == os_make_fixnum(0), "読み込み前のfile-positionは0");
    cc_read_char(args, nil);
    assert(cc_file_position(args, nil) == os_make_fixnum(1), "1文字読んだ後のfile-positionは1");

    lisp_val_t set_args = os_make_cons(stream, os_make_cons(os_make_fixnum(0), nil));
    lisp_val_t result = cc_set_file_position(set_args, nil);
    assert(result == os_make_fixnum(0), "set-file-positionは設定した値を返す");
    assert(cc_read_char(args, nil) == os_make_char('a'), "set-file-positionで巻き戻した後は先頭から読める");
}

void test_file_length_counts_bytes_by_reading_whole_file() {
    reset_fake_state();
    set_fake_data("hello");

    lisp_val_t result = cc_file_length(os_make_cons(os_make_string("fake/path"), nil), nil);
    assert(result == os_make_fixnum(5), "file-lengthは全部読み切ったバイト数を返す");
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
    test_open_stream_p_input_stream_p_output_stream_p();
    test_open_output_file_and_io_file_use_open_then_create_fallback();
    test_finish_output_flushes_and_returns_nil();
    test_string_input_stream_reads_content();
    test_string_output_stream_accumulates_and_resets();
    test_preview_char_does_not_advance_cursor();
    test_read_line_reads_up_to_newline();
    test_stream_ready_p_is_always_true();
    test_read_byte_and_write_byte_wrap_char_ops();
    test_probe_file_reflects_open_success();
    test_file_position_and_set_file_position_on_string_stream();
    test_file_length_counts_bytes_by_reading_whole_file();

    return g_test_failed ? 1 : 0;
}
