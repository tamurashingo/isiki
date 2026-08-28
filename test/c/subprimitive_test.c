#include <stdint.h>
#include <stdlib.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "lisp.h"
#include "subprimitive.h"
#include "process.h"
#include "reader.h"

// reader.c は os_read_stream 経由でstream.cをリンクするため、stream.cが
// 参照するos_virtio9p_open/read_chunk/closeが未定義シンボルにならないよう
// ダミー実装を置く(このテストはos_read_streamを呼ばないため中身は使われない)
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

// reader.c の os_read が参照するが、このテストでは実際の割り込みが発生しないため
// 何もしないダミー実装を用意する
void os_wait_for_more_input(process_t *proc) {
    (void)proc;
}

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

// process.c が参照する switch_active_frame_buffer のダミー実装。
// このテストでは process の切替えは行わないため、何もしない
void switch_active_frame_buffer(UINT32 index) {
    (void)index;
}

// process.c(process_scheduler_start/process_trampoline_c)が参照する
// interrupt.c/repl.cの関数のダミー実装。ハードウェア割り込みやREPLの実行に
// 依存する部分はこのテストの対象外なので、リンクを通すためだけに置く
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

// cc_in_16/cc_out_16(%%IN-16/%%OUT-16)用のoutb/inbと対になるモック
static uint16_t g_last_out16_port = 0;
static uint16_t g_last_out16_val = 0;
static uint16_t g_last_in16_port = 0;
static uint16_t g_next_in16_val = 0;

void outw(uint16_t port, uint16_t val) {
    g_last_out16_port = port;
    g_last_out16_val = val;
}

uint16_t inw(uint16_t port) {
    g_last_in16_port = port;
    return g_next_in16_val;
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

void test_cc_in_16() {
    g_next_in16_val = 0x1234;
    lisp_val_t args[1] = { os_make_fixnum(0x170) };
    lisp_val_t v = cc_in_16(make_args(1, args), nil);

    assert(g_last_in16_port == 0x170, "cc_in_16はポート0x170でinwを呼ぶ");
    assert(v == os_make_fixnum(0x1234), "cc_in_16はinwが返した0x1234をfixnumで返す");
}

void test_cc_out_16() {
    lisp_val_t args[2] = { os_make_fixnum(0x170), os_make_fixnum(0x5678) };
    lisp_val_t v = cc_out_16(make_args(2, args), nil);

    assert(g_last_out16_port == 0x170, "cc_out_16はポート0x170でoutwを呼ぶ");
    assert(g_last_out16_val == 0x5678, "cc_out_16は値0x5678でoutwを呼ぶ");
    assert(v == os_make_fixnum(0x5678), "cc_out_16は書き込んだ値をfixnumで返す");
}

void test_cc_logand() {
    lisp_val_t args[2] = { os_make_fixnum(0xF0F0), os_make_fixnum(0xFF00) };
    lisp_val_t v = cc_logand(make_args(2, args), nil);

    assert(v == os_make_fixnum(0xF000), "cc_logandは2引数のビットごとのANDをfixnumで返す");
}

void test_cc_logior() {
    lisp_val_t args[2] = { os_make_fixnum(0xF0F0), os_make_fixnum(0x0F0F) };
    lisp_val_t v = cc_logior(make_args(2, args), nil);

    assert(v == os_make_fixnum(0xFFFF), "cc_logiorは2引数のビットごとのORをfixnumで返す");
}

void test_cc_logxor() {
    lisp_val_t args[2] = { os_make_fixnum(0xFF00), os_make_fixnum(0xF0F0) };
    lisp_val_t v = cc_logxor(make_args(2, args), nil);

    assert(v == os_make_fixnum(0x0FF0), "cc_logxorは2引数のビットごとのXORをfixnumで返す");
}

void test_cc_ash() {
    lisp_val_t left_args[2] = { os_make_fixnum(0x01), os_make_fixnum(8) };
    lisp_val_t left = cc_ash(make_args(2, left_args), nil);
    assert(left == os_make_fixnum(0x100), "cc_ashは正のcountで左シフトする");

    lisp_val_t right_args[2] = { os_make_fixnum(0x100), os_make_fixnum_signed(1, 8) };
    lisp_val_t right = cc_ash(make_args(2, right_args), nil);
    assert(right == os_make_fixnum(0x01), "cc_ashは負のcountで右シフトする");
}

void test_cc_char_code() {
    lisp_val_t args[1] = { ((lisp_val_t)0x41 << 3) | TAG_CHAR };
    lisp_val_t v = cc_char_code(make_args(1, args), nil);

    assert(v == os_make_fixnum(0x41), "cc_char_codeは文字'A'(0x41)のタグを外した文字コードをfixnumで返す");
}

void test_cc_code_char() {
    lisp_val_t args[1] = { os_make_fixnum(0x41) };
    lisp_val_t v = cc_code_char(make_args(1, args), nil);

    assert((v & TAG_MASK) == TAG_CHAR, "cc_code_charはTAG_CHARタグ付きの値を返す");
    assert((v >> 3) == 0x41, "cc_code_charは文字コード0x41をタグ付けした値を返す");
}

void test_os_register_subprimitives() {
    os_register_subprimitives();

    lisp_val_t in8 = os_get_function(os_make_symbol("%%IN-8"), global_environment);
    lisp_val_t out8 = os_get_function(os_make_symbol("%%OUT-8"), global_environment);
    lisp_val_t in16 = os_get_function(os_make_symbol("%%IN-16"), global_environment);
    lisp_val_t out16 = os_get_function(os_make_symbol("%%OUT-16"), global_environment);
    lisp_val_t peek = os_get_function(os_make_symbol("%%PEEK"), global_environment);
    lisp_val_t poke = os_get_function(os_make_symbol("%%POKE"), global_environment);
    lisp_val_t logand = os_get_function(os_make_symbol("%%LOGAND"), global_environment);
    lisp_val_t logior = os_get_function(os_make_symbol("%%LOGIOR"), global_environment);
    lisp_val_t logxor = os_get_function(os_make_symbol("%%LOGXOR"), global_environment);
    lisp_val_t ash = os_get_function(os_make_symbol("%%ASH"), global_environment);
    lisp_val_t char_code = os_get_function(os_make_symbol("%%CHAR-CODE"), global_environment);
    lisp_val_t code_char = os_get_function(os_make_symbol("%%CODE-CHAR"), global_environment);

    assert(in8 != nil, "os_register_subprimitives後は%%IN-8がglobal_environmentから引ける");
    assert(out8 != nil, "os_register_subprimitives後は%%OUT-8がglobal_environmentから引ける");
    assert(in16 != nil, "os_register_subprimitives後は%%IN-16がglobal_environmentから引ける");
    assert(out16 != nil, "os_register_subprimitives後は%%OUT-16がglobal_environmentから引ける");
    assert(peek != nil, "os_register_subprimitives後は%%PEEKがglobal_environmentから引ける");
    assert(poke != nil, "os_register_subprimitives後は%%POKEがglobal_environmentから引ける");
    assert(logand != nil, "os_register_subprimitives後は%%LOGANDがglobal_environmentから引ける");
    assert(logior != nil, "os_register_subprimitives後は%%LOGIORがglobal_environmentから引ける");
    assert(logxor != nil, "os_register_subprimitives後は%%LOGXORがglobal_environmentから引ける");
    assert(ash != nil, "os_register_subprimitives後は%%ASHがglobal_environmentから引ける");
    assert(char_code != nil, "os_register_subprimitives後は%%CHAR-CODEがglobal_environmentから引ける");
    assert(code_char != nil, "os_register_subprimitives後は%%CODE-CHARがglobal_environmentから引ける");
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    setup_heap();

    test_cc_in_8();
    test_cc_out_8();
    test_cc_in_16();
    test_cc_out_16();
    test_cc_peek();
    test_cc_poke();
    test_cc_logand();
    test_cc_logior();
    test_cc_logxor();
    test_cc_ash();
    test_cc_char_code();
    test_cc_code_char();
    test_os_register_subprimitives();

    return g_test_failed ? 1 : 0;
}
