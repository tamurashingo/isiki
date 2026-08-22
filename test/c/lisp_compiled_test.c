#include <stdlib.h>
#include <string.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "process.h"

// runtime.c/process.c/za.c/reader.c/stream.cをリンクするため、それらが参照する
// ハードウェア/REPL依存の関数のダミー実装が必要になる(runtime_test.cと同じパターン)
int os_virtio9p_open(const char *path, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path;
    (void)mode;
    (void)out_fid;
    (void)err_msg;
    (void)err_msg_cap;
    return 0;
}

int os_virtio9p_create(const char *path, UINT32 perm, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path;
    (void)perm;
    (void)mode;
    (void)out_fid;
    (void)err_msg;
    (void)err_msg_cap;
    return 0;
}

int os_virtio9p_write_chunk(UINT32 fid, UINT64 offset, const UINT8 *data, UINT32 count,
                             UINT32 *out_written, char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)offset;
    (void)data;
    (void)count;
    (void)out_written;
    (void)err_msg;
    (void)err_msg_cap;
    return 0;
}

int os_virtio9p_read_chunk(UINT32 fid, UINT64 offset, UINT32 want,
                            const UINT8 **out_data, UINT32 *out_count,
                            char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)offset;
    (void)want;
    (void)out_data;
    (void)out_count;
    (void)err_msg;
    (void)err_msg_cap;
    return 0;
}

int os_virtio9p_close(UINT32 fid, char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)err_msg;
    (void)err_msg_cap;
    return 0;
}

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

static UINT8 g_fake_fpu_default_state[512] __attribute__((aligned(16)));

const void *get_fpu_default_state(void) {
    return g_fake_fpu_default_state;
}

void os_repl_step(process_t *proc) {
    (void)proc;
}

void os_wait_for_more_input(process_t *proc) {
    (void)proc;
}

extern lisp_val_t lisp_ll_transpile_fixture_answer(lisp_val_t args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_string(lisp_val_t args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_symbol(lisp_val_t args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_nil(lisp_val_t args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_t(lisp_val_t args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_quoted_fixnum(lisp_val_t args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_identity(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_second_param(lisp_val_t evaluated_args, lisp_val_t env);

// os_make_string/os_make_symbolはヒープ確保とnilの初期化が前提なので、
// それらを呼ぶ生成物のテストの前にheap_initとbootを済ませておく
#define HEAP_SIZE (1024 * 1024)

static void setup_heap(void) {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
}

static void test_transpile_fixture_answer(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_answer(0, 0);
    assert((result & TAG_MASK) == TAG_FIXNUM, "transpiled function returns a fixnum");
    assert((result >> 3) == 42, "transpiled function returns 42");
}

static void test_transpile_fixture_string(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_string(0, 0);
    assert((result & TAG_MASK) == TAG_STRING, "transpiled function returns a string");

    lisp_addr_t addr = result & ~TAG_MASK;
    UINT64 *header = (UINT64 *)addr;
    assert(header[0] == 5, "string length matches \"hello\"");
    const char *bytes = (const char *)(addr + 8);
    assert(strncmp(bytes, "hello", 5) == 0, "string content matches \"hello\"");
}

static void test_transpile_fixture_symbol(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_symbol(0, 0);
    assert((result & TAG_MASK) == TAG_SYMBOL, "transpiled function returns a symbol");
    assert(result == os_make_symbol("FOO"), "transpiled symbol is interned as FOO");
}

static void test_transpile_fixture_nil(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_nil(0, 0);
    assert(result == nil, "transpiled function returns nil");
}

static void test_transpile_fixture_t(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_t(0, 0);
    assert(result == g_sym_t, "transpiled function returns the T symbol");
}

static void test_transpile_fixture_quoted_fixnum(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_quoted_fixnum(0, 0);
    assert((result & TAG_MASK) == TAG_FIXNUM, "quoted fixnum is still a fixnum");
    assert((result >> 3) == 99, "quoted fixnum keeps its value");
}

static void test_transpile_fixture_identity(void) {
    lisp_val_t arg = os_make_fixnum(7);
    lisp_val_t evaluated_args = os_make_cons(arg, nil);
    lisp_val_t result = lisp_ll_transpile_fixture_identity(evaluated_args, 0);
    assert(result == arg, "identity(x) returns the value bound to its parameter");
}

static void test_transpile_fixture_second_param(void) {
    lisp_val_t first = os_make_fixnum(1);
    lisp_val_t second = os_make_fixnum(2);
    lisp_val_t evaluated_args = os_make_cons(first, os_make_cons(second, nil));
    lisp_val_t result = lisp_ll_transpile_fixture_second_param(evaluated_args, 0);
    assert(result == second, "second-param(x, y) walks evaluated_args via cc_cdr to reach y");
}

int main(void) {
    setup_heap();
    test_transpile_fixture_answer();
    test_transpile_fixture_string();
    test_transpile_fixture_symbol();
    test_transpile_fixture_nil();
    test_transpile_fixture_t();
    test_transpile_fixture_quoted_fixnum();
    test_transpile_fixture_identity();
    test_transpile_fixture_second_param();
    return g_test_failed;
}
