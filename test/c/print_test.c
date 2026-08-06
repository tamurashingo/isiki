#include <stdlib.h>
#include <string.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "lisp.h"
#include "print.h"

// os_print が書き込んだ内容を検証できるよう、write_char/write_string を
// 実画面ではなく静的バッファへキャプチャするダミー実装にする
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

static frame_buffer g_frame_buffer = {
    .write_char = capture_write_char,
    .write_string = capture_write_string,
};

frame_buffer* get_active_frame_buffer(void) {
    return &g_frame_buffer;
}

static void reset_capture() {
    g_capture_len = 0;
}

static const char *captured() {
    g_capture_buf[g_capture_len] = '\0';
    return g_capture_buf;
}

#define HEAP_SIZE (1024 * 1024)

static void setup_heap() {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
}

void test_os_print_fixnum() {
    reset_capture();
    lisp_val_t v = os_print(os_make_fixnum(42), &g_frame_buffer);
    assert(strcmp(captured(), "42") == 0, "fixnum 42は\"42\"と表示される");
    assert(v == os_make_fixnum(42), "os_printは渡した値をそのまま返す");
}

void test_os_print_fixnum_zero() {
    reset_capture();
    os_print(os_make_fixnum(0), &g_frame_buffer);
    assert(strcmp(captured(), "0") == 0, "fixnum 0は\"0\"と表示される");
}

void test_os_print_symbol() {
    reset_capture();
    os_print(os_make_symbol("foo"), &g_frame_buffer);
    assert(strcmp(captured(), "FOO") == 0, "symbol fooは大文字化された\"FOO\"と表示される");
}

void test_os_print_string() {
    reset_capture();
    os_print(os_make_string("hi"), &g_frame_buffer);
    assert(strcmp(captured(), "\"hi\"") == 0, "文字列hiはダブルクオートで囲まれて表示される");
}

void test_os_print_nil() {
    reset_capture();
    os_print(nil, &g_frame_buffer);
    assert(strcmp(captured(), "NIL") == 0, "nilは\"NIL\"と表示される(循環consで無限再帰しない)");
}

void test_os_print_list() {
    reset_capture();
    lisp_val_t list = os_make_cons(os_make_fixnum(1),
                        os_make_cons(os_make_fixnum(2),
                          os_make_cons(os_make_fixnum(3), nil)));
    os_print(list, &g_frame_buffer);
    assert(strcmp(captured(), "(1 2 3)") == 0, "(1 2 3)相当のリストは\"(1 2 3)\"と表示される");
}

void test_os_print_dotted_pair() {
    reset_capture();
    lisp_val_t pair = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
    os_print(pair, &g_frame_buffer);
    assert(strcmp(captured(), "(1 . 2)") == 0, "nilで終端しないconsはドット対記法で表示される");
}

void test_os_print_nested_list() {
    reset_capture();
    lisp_val_t inner = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
    lisp_val_t list = os_make_cons(inner, os_make_cons(os_make_fixnum(3), nil));
    os_print(list, &g_frame_buffer);
    assert(strcmp(captured(), "((1 . 2) 3)") == 0, "ネストしたconsも再帰的に表示される");
}

void test_os_print_native_function() {
    reset_capture();
    lisp_val_t fn = os_make_native_function((lisp_addr_t)(void *)primitive_car);
    os_print(fn, &g_frame_buffer);
    assert(strcmp(captured(), "#<FUNCTION>") == 0, "ネイティブ関数は\"#<FUNCTION>\"と表示される");
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    setup_heap();

    test_os_print_fixnum();
    test_os_print_fixnum_zero();
    test_os_print_symbol();
    test_os_print_string();
    test_os_print_nil();
    test_os_print_list();
    test_os_print_dotted_pair();
    test_os_print_nested_list();
    test_os_print_native_function();

    return g_test_failed ? 1 : 0;
}
