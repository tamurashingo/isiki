#include <stdint.h>
#include <stdlib.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "lisp.h"
#include "subprimitive.h"

// runtime.c が参照する get_active_frame_buffer のダミー実装。
// テスト環境では実画面がないため、write_string は何もしない
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

// interrupt.c の実outb/inb(実I/O命令)はユーザ空間のテストプロセスでは実行できないため、
// このテストファイル自身が呼ばれたport/valを記録するだけのモックを提供する(実I/O命令はリンクしない)
static uint16_t g_last_out_port = 0;
static uint8_t g_last_out_val = 0;
static uint16_t g_last_in_port = 0;
static uint8_t g_next_in_val = 0;

void outb(uint16_t port, uint8_t val) {
    g_last_out_port = port;
    g_last_out_val = val;
}

uint8_t inb(uint16_t port) {
    g_last_in_port = port;
    return g_next_in_val;
}

#define HEAP_SIZE (1024 * 1024)

static void setup_heap() {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
}

static lisp_val_t make_args(int argc, lisp_val_t *args) {
    lisp_val_t list = nil;
    for (int i = argc - 1; i >= 0; i--) {
        list = os_make_cons(args[i], list);
    }
    return list;
}

void test_cc_in_8() {
    g_next_in_val = 0x42;
    lisp_val_t args[1] = { os_make_fixnum(0x60) };
    lisp_val_t v = cc_in_8(make_args(1, args), nil);

    assert(g_last_in_port == 0x60, "cc_in_8はポート0x60でinbを呼ぶ");
    assert(v == os_make_fixnum(0x42), "cc_in_8はinbが返した0x42をfixnumで返す");
}

void test_cc_out_8() {
    lisp_val_t args[2] = { os_make_fixnum(0x20), os_make_fixnum(0x20) };
    lisp_val_t v = cc_out_8(make_args(2, args), nil);

    assert(g_last_out_port == 0x20, "cc_out_8はポート0x20でoutbを呼ぶ");
    assert(g_last_out_val == 0x20, "cc_out_8は値0x20でoutbを呼ぶ");
    assert(v == os_make_fixnum(0x20), "cc_out_8は書き込んだ値をfixnumで返す");
}

void test_cc_peek() {
    UINT8 target = 0xAB;
    lisp_val_t args[1] = { os_make_fixnum((UINT64)(lisp_addr_t)&target) };
    lisp_val_t v = cc_peek(make_args(1, args), nil);

    assert(v == os_make_fixnum(0xAB), "cc_peekは指定アドレスの1byteをfixnumで返す");
}

void test_cc_poke() {
    UINT8 target = 0;
    lisp_val_t args[2] = { os_make_fixnum((UINT64)(lisp_addr_t)&target), os_make_fixnum(0xCD) };
    lisp_val_t v = cc_poke(make_args(2, args), nil);

    assert(target == 0xCD, "cc_pokeは指定アドレスに1byteを書き込む");
    assert(v == os_make_fixnum(0xCD), "cc_pokeは書き込んだ値をfixnumで返す");
}

void test_os_register_subprimitives() {
    os_register_subprimitives();

    lisp_val_t in8 = os_get_function(os_make_symbol("%%IN-8"), global_environment);
    lisp_val_t out8 = os_get_function(os_make_symbol("%%OUT-8"), global_environment);
    lisp_val_t peek = os_get_function(os_make_symbol("%%PEEK"), global_environment);
    lisp_val_t poke = os_get_function(os_make_symbol("%%POKE"), global_environment);

    assert(in8 != nil, "os_register_subprimitives後は%%IN-8がglobal_environmentから引ける");
    assert(out8 != nil, "os_register_subprimitives後は%%OUT-8がglobal_environmentから引ける");
    assert(peek != nil, "os_register_subprimitives後は%%PEEKがglobal_environmentから引ける");
    assert(poke != nil, "os_register_subprimitives後は%%POKEがglobal_environmentから引ける");
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    setup_heap();

    test_cc_in_8();
    test_cc_out_8();
    test_cc_peek();
    test_cc_poke();
    test_os_register_subprimitives();

    return g_test_failed ? 1 : 0;
}
