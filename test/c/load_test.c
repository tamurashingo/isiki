#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "lisp.h"
#include "process.h"
#include "reader.h"
#include "eval.h"
#include "load.h"

// stream.c/reader.c/load.c は process.c/virtio9p.c/p9.c/drivers/*.c をリンクしない。
// cc_load/os_read_stream/os_stream_* が参照する外部シンボルだけダミー実装を置く。

#define FAKE_DATA_MAX 512
static UINT8 g_fake_data[FAKE_DATA_MAX];
static UINT32 g_fake_data_len = 0;
static UINT32 g_fake_chunk_size = 5; // 0以外なら1回のTreadで返す最大バイト数(不揃いな再充填を再現するため小さめ)
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
    g_fake_chunk_size = 5;
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
    UINT32 remaining = g_fake_data_len - off;
    UINT32 count = remaining;
    if (g_fake_chunk_size > 0 && g_fake_chunk_size < count) {
        count = g_fake_chunk_size;
    }
    *out_data = g_fake_data + off;
    *out_count = count;
    return 1;
}

int os_virtio9p_close(UINT32 fid, char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)err_msg;
    (void)err_msg_cap;
    return 1;
}

// runtime.c(os_alloc_bytesのOOM表示)が参照するダミー実装
static void dummy_write_string(struct _frame_buffer *self, const char *s) {
    (void)self;
    (void)s;
}

static frame_buffer g_active_frame_buffer = {
    .write_string = dummy_write_string,
};

frame_buffer* get_active_frame_buffer(void) {
    return &g_active_frame_buffer;
}

// reader.c(process用reader_source)が参照するダミー実装。
// load_testはos_read_stream経由のみを使うため実際には呼ばれない
void os_wait_for_more_input(process_t *proc) {
    (void)proc;
}

// load.c(cc_loadのエラー表示)が参照する現在プロセスのダミー実装。
// stdout_bufferへの書き込みは静的バッファへキャプチャする
#define CAPTURE_BUF_SIZE 256
static char g_capture_buf[CAPTURE_BUF_SIZE];
static UINT32 g_capture_len = 0;

static void capture_write_char(struct _frame_buffer *self, UINT8 c) {
    (void)self;
    if (g_capture_len < CAPTURE_BUF_SIZE - 1) {
        g_capture_buf[g_capture_len++] = (char)c;
    }
}

static void capture_write_string(struct _frame_buffer *self, const char *s) {
    while (*s) {
        capture_write_char(self, (UINT8)*s);
        s++;
    }
}

static frame_buffer g_proc_frame_buffer = {
    .write_char = capture_write_char,
    .write_string = capture_write_string,
};

static process_t g_process;

process_t* get_current_process(void) {
    return &g_process;
}

static void reset_capture(void) {
    g_capture_len = 0;
}

#define HEAP_SIZE (1024 * 1024)

static void setup_heap(void) {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
    os_register_load();
}

static void setup_process(void) {
    memset(&g_process, 0, sizeof(g_process));
    g_process.stdout_buffer = &g_proc_frame_buffer;
}

static lisp_val_t call_load(const char *path) {
    lisp_val_t form = os_make_cons(os_make_symbol("LOAD"),
                                    os_make_cons(os_make_string(path), nil));
    return os_eval(form, global_environment);
}

static void test_load_evaluates_multiple_forms(void) {
    reset_fake_state();
    reset_capture();
    set_fake_data("(defun add1 (x) (+ x 1))\n(setq loaded-flag 42)\n");

    lisp_val_t result = call_load("fake/multi.lisp");

    assert(result == g_sym_t, "複数フォームを含むファイルのloadはg_sym_tを返す");

    lisp_val_t call_result = os_eval(
        os_make_cons(os_make_symbol("add1"), os_make_cons(os_make_fixnum(5), nil)),
        global_environment);
    assert(call_result == os_make_fixnum(6), "loadで定義された関数を後から呼び出せる");

    lisp_val_t flag = os_get_variable(os_make_symbol("loaded-flag"), global_environment);
    assert(flag == os_make_fixnum(42), "loadで設定された変数を後から参照できる");
}

static void test_load_syntax_error_returns_eval_error(void) {
    reset_fake_state();
    reset_capture();
    set_fake_data("(defun bad (x)\n");

    lisp_val_t result = call_load("fake/bad.lisp");

    assert(result == g_sym_eval_error, "構文エラーを含むファイルのloadはg_sym_eval_errorを返す");
}

static void test_load_empty_file_returns_t(void) {
    reset_fake_state();
    reset_capture();
    set_fake_data("");

    lisp_val_t result = call_load("fake/empty.lisp");

    assert(result == g_sym_t, "空ファイルのloadは何も評価せずg_sym_tを返す");
}

static void test_load_open_failure_returns_eval_error(void) {
    reset_fake_state();
    reset_capture();
    g_fake_open_fail = 1;

    lisp_val_t result = call_load("fake/missing.lisp");

    assert(result == g_sym_eval_error, "openに失敗した場合はg_sym_eval_errorを返す");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    setup_heap();
    setup_process();

    test_load_evaluates_multiple_forms();
    test_load_syntax_error_returns_eval_error();
    test_load_empty_file_returns_t();
    test_load_open_failure_returns_eval_error();

    return g_test_failed ? 1 : 0;
}
