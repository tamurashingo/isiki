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
extern lisp_val_t lisp_ll_transpile_fixture_if(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_if_no_else(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_progn(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_setq(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_and(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_and_empty(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_and_single(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_or(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_or_empty(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_or_single(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_or_three(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_gc_protect(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_count_down(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_is_even(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_is_odd(lisp_val_t evaluated_args, lisp_val_t env);

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

static void test_transpile_fixture_if(void) {
    lisp_val_t result_true = lisp_ll_transpile_fixture_if(os_make_cons(os_make_fixnum(5), nil), 0);
    assert((result_true >> 3) == 1, "if: xがnil以外ならthen節(1)を返す");

    lisp_val_t result_false = lisp_ll_transpile_fixture_if(os_make_cons(nil, nil), 0);
    assert((result_false >> 3) == 2, "if: xがnilならelse節(2)を返す");
}

static void test_transpile_fixture_if_no_else(void) {
    lisp_val_t result_true = lisp_ll_transpile_fixture_if_no_else(os_make_cons(os_make_fixnum(5), nil), 0);
    assert((result_true >> 3) == 42, "if(else省略): xがnil以外ならthen節(42)を返す");

    lisp_val_t result_false = lisp_ll_transpile_fixture_if_no_else(os_make_cons(nil, nil), 0);
    assert(result_false == nil, "if(else省略): xがnilならelse省略時のデフォルトnilを返す");
}

static void test_transpile_fixture_progn(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_progn(os_make_cons(os_make_fixnum(1), nil), 0);
    assert((result >> 3) == 99, "progn: 先頭のxは評価されるが値は捨てられ、最後の式(99)が返る");
}

static void test_transpile_fixture_setq(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_setq(os_make_cons(os_make_fixnum(1), nil), 0);
    assert((result >> 3) == 7, "setq: 代入後の新しい値(7)がsetq式自体の値になる");
}

static void test_transpile_fixture_and(void) {
    lisp_val_t x = os_make_fixnum(5);
    lisp_val_t y = os_make_fixnum(7);
    lisp_val_t result = lisp_ll_transpile_fixture_and(os_make_cons(x, os_make_cons(y, nil)), 0);
    assert(result == y, "and: 両方nil以外なら最後の式の値(y)を返す");

    lisp_val_t result_short = lisp_ll_transpile_fixture_and(os_make_cons(nil, os_make_cons(y, nil)), 0);
    assert(result_short == nil, "and: 先頭がnilならそこで短絡してnilを返す");
}

static void test_transpile_fixture_and_empty(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_and_empty(0, 0);
    assert(result == g_sym_t, "and(引数無し): tを返す");
}

static void test_transpile_fixture_and_single(void) {
    lisp_val_t x = os_make_fixnum(5);
    lisp_val_t result = lisp_ll_transpile_fixture_and_single(os_make_cons(x, nil), 0);
    assert(result == x, "and(1引数): その式自身をそのまま返す");
}

static void test_transpile_fixture_or(void) {
    lisp_val_t x = os_make_fixnum(5);
    lisp_val_t y = os_make_fixnum(7);
    lisp_val_t result = lisp_ll_transpile_fixture_or(os_make_cons(x, os_make_cons(y, nil)), 0);
    assert(result == x, "or: 先頭がnil以外ならそこで短絡してxを返す");

    lisp_val_t result_fallthrough = lisp_ll_transpile_fixture_or(os_make_cons(nil, os_make_cons(y, nil)), 0);
    assert(result_fallthrough == y, "or: 先頭がnilなら次の式(y)を返す");

    lisp_val_t result_all_nil = lisp_ll_transpile_fixture_or(os_make_cons(nil, os_make_cons(nil, nil)), 0);
    assert(result_all_nil == nil, "or: 全てnilならnilを返す");
}

static void test_transpile_fixture_or_empty(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_or_empty(0, 0);
    assert(result == nil, "or(引数無し): nilを返す");
}

static void test_transpile_fixture_or_single(void) {
    lisp_val_t x = os_make_fixnum(5);
    lisp_val_t result = lisp_ll_transpile_fixture_or_single(os_make_cons(x, nil), 0);
    assert(result == x, "or(1引数): その式自身をそのまま返す");
}

static void test_transpile_fixture_or_three(void) {
    lisp_val_t x = os_make_fixnum(1);
    lisp_val_t y = os_make_fixnum(2);
    lisp_val_t z = os_make_fixnum(3);

    // *or-temp-counter*が呼び出しごとに一意なC変数名を振ることを、
    // ネストしたステートメント式(3引数以上のor)経由で間接的に検証する
    lisp_val_t result_last = lisp_ll_transpile_fixture_or_three(
        os_make_cons(nil, os_make_cons(nil, os_make_cons(z, nil))), 0);
    assert(result_last == z, "or(3引数): 先頭2つがnilなら最後の式(z)を返す");

    lisp_val_t result_middle = lisp_ll_transpile_fixture_or_three(
        os_make_cons(nil, os_make_cons(y, os_make_cons(z, nil))), 0);
    assert(result_middle == y, "or(3引数): 先頭がnilで2番目がnil以外ならyで短絡する");

    lisp_val_t result_first = lisp_ll_transpile_fixture_or_three(
        os_make_cons(x, os_make_cons(y, os_make_cons(z, nil))), 0);
    assert(result_first == x, "or(3引数): 先頭がnil以外ならxで短絡する");
}

static void test_transpile_fixture_count_down(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_count_down(os_make_cons(os_make_fixnum(3), nil), 0);
    assert((result & TAG_MASK) == TAG_FIXNUM, "count-down: 自己再帰の結果はfixnum");
    assert((result >> 3) == 42, "count-down: 3から0まで自己再帰しeqで停止して42を返す");

    // 末尾呼び出しのトランポリン化の検証: 実測(160バイト/フレーム、64KB・
    // ガード無しのプロセススタック)では素朴なC再帰は約410段でスタックが
    // 破壊されるが、トランポリンによりCの呼び出しは常にreturnしてから次の
    // 呼び出しが起きるフラットなループになるため、100000段の自己再帰でも
    // クラッシュせず定数スタックで完了することを確認する
    lisp_val_t deep_result = lisp_ll_transpile_fixture_count_down(os_make_cons(os_make_fixnum(100000), nil), 0);
    assert((deep_result >> 3) == 42, "count-down: トランポリンにより100000段の自己再帰でもスタックを溢れさせずに42を返す");
}

static void test_transpile_fixture_is_even(void) {
    lisp_val_t result_even = lisp_ll_transpile_fixture_is_even(os_make_cons(os_make_fixnum(4), nil), 0);
    assert(result_even == g_sym_t, "is-even: 4は相互再帰でis-oddを経由してtを返す");

    lisp_val_t result_odd = lisp_ll_transpile_fixture_is_even(os_make_cons(os_make_fixnum(3), nil), 0);
    assert(result_odd == nil, "is-even: 3は相互再帰でis-oddを経由してnilを返す");
}

static void test_transpile_fixture_is_odd(void) {
    lisp_val_t result_odd = lisp_ll_transpile_fixture_is_odd(os_make_cons(os_make_fixnum(3), nil), 0);
    assert(result_odd == g_sym_t, "is-odd: 3は相互再帰でis-evenを経由してtを返す");

    lisp_val_t result_even = lisp_ll_transpile_fixture_is_odd(os_make_cons(os_make_fixnum(4), nil), 0);
    assert(result_even == nil, "is-odd: 4は相互再帰でis-evenを経由してnilを返す");
}

// M7: パラメータのGC_PROTECT統合の検証。生成物(lisp_ll_transpile_fixture_gc_protect)は
// bodyの評価中に40000文字のダミー文字列を2つ、os_make_stringで順に確保する。これを
// 小さいヒープと組み合わせることで、1つ目は確保できるが2つ目の確保時に空き領域が
// 足りず、パラメータxを束縛した後・xをreturnする前に実際にos_gc_collectが発火する
// 状況を作る。この関数呼び出しは他のテストとglobal_environment/symbol table等の
// 状態を共有しないため、main()の最後で単独で実行する
#define GC_PROTECT_TEST_HEAP_SIZE (160 * 1024)

static void test_transpile_fixture_gc_protect(void) {
    void *heap = malloc(GC_PROTECT_TEST_HEAP_SIZE);
    assert(heap != NULL, "GC_PROTECT検証用の小さいヒープをmallocで確保できる");
    os_heap_init((UINT64)heap, GC_PROTECT_TEST_HEAP_SIZE);
    os_reset_runtime_state_for_test();
    os_bootstrap();

    lisp_val_t arg = os_make_string("payload");
    lisp_addr_t addr_before = arg & ~TAG_MASK;
    UINT64 gc_count_before = os_gc_collect_count();

    lisp_val_t result = lisp_ll_transpile_fixture_gc_protect(os_make_cons(arg, nil), 0);

    UINT64 gc_count_after = os_gc_collect_count();
    assert(gc_count_after > gc_count_before,
           "GC_PROTECT: 40000文字のダミー文字列確保により、body評価中に実際にos_gc_collectが発火する");
    assert((result & TAG_MASK) == TAG_STRING, "GC_PROTECT: 内部GCを跨いでもxのタグはTAG_STRINGのまま");
    assert((result & ~TAG_MASK) != addr_before,
           "GC_PROTECT: 内部GCによりxの指す文字列はTo空間の新しいアドレスへ再配置される");

    char buf[16];
    os_string_to_cstr(result, buf, sizeof(buf));
    assert(strncmp(buf, "payload", 7) == 0,
           "GC_PROTECT: 内部GCを跨いでもxの指す文字列の内容は保たれる(パラメータが正しく追従している)");
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
    test_transpile_fixture_if();
    test_transpile_fixture_if_no_else();
    test_transpile_fixture_progn();
    test_transpile_fixture_setq();
    test_transpile_fixture_and();
    test_transpile_fixture_and_empty();
    test_transpile_fixture_and_single();
    test_transpile_fixture_or();
    test_transpile_fixture_or_empty();
    test_transpile_fixture_or_single();
    test_transpile_fixture_or_three();
    test_transpile_fixture_count_down();
    test_transpile_fixture_is_even();
    test_transpile_fixture_is_odd();
    // GC_PROTECT検証はos_reset_runtime_state_for_testでglobal_environment/symbol table等の
    // 状態を再初期化するため、他のテストに影響しないよう最後に実行する
    test_transpile_fixture_gc_protect();
    return g_test_failed;
}
