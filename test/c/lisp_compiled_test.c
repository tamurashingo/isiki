#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "process.h"
#include "lisp.h"
#include "eval.h"

// runtime.c/process.c/za.c/reader.c/stream.c/stream_lisp.cをリンクするため、それらが
// 参照するハードウェア/REPL依存の関数のダミー実装が必要になる(runtime_test.cと同じ
// パターン)。M14基盤E: with-open-input-stream/with-open-input-fileがunwind-protect
// 経由で必ずcloseすることをfixtureから検証するため、os_virtio9p_openは実際のfidを返して
// 成功させる(stream_lisp_test.cのフェイク9Pと同じ方針。読み込む内容自体は検証対象では
// ないため、read/write/closeは従来通りダミーのままでよい)
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
extern lisp_val_t lisp_ll_transpile_fixture_lambda_capture_value(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_lambda_box_mutate(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_make_counter(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_call_twice(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_defdynamic(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_dynamic_read(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_let(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_let_multi(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_let_body_progn(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_let_star(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_cond(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_cond_body_progn(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_case(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_case_using(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_setf_setq(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_setf_car(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_setf_cdr(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_setf_aref(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_setf_elt(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_setf_slot_value(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_rest(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_rest_with_fixed(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_for_sum(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_for_early_exit(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_while_sum(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_while_named_block_exit(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_unwind_protect_normal(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_unwind_protect_non_local_exit(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_unwind_protect_cleanup_exit_ignored(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_with_open_input_stream(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_with_open_input_stream_early_exit(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_with_open_input_file(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_with_open_output_stream_early_exit(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_with_open_output_file(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_catch_throw_basic(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_catch_no_throw(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_catch_mismatched_tag(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_catch_throw_runtime_tag(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_catch_with_cleanup(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_register_and_find_class(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_find_class_missing(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_register_builtin_class_then_find(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_slot_value_read(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_fill_slots(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_subclassp(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_class_of_builtin(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_class_of_instance(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_typep(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_instancep(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_generic_dispatch(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_generic_dispatch_order(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_call_next_method(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_generic_no_applicable_method(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_call_next_method_no_next(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_make_instance(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_nested_let_dynamic_restore(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_transpile_fixture_signal_condition_like(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_list(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_append(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_create_list(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_nreverse(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_apply(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_mapcar(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_mapc(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_mapcan(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_mapcon(lisp_val_t evaluated_args, lisp_val_t env);
extern lisp_val_t lisp_ll_map_into(lisp_val_t evaluated_args, lisp_val_t env);

// os_make_string/os_make_symbolはヒープ確保とnilの初期化が前提なので、
// それらを呼ぶ生成物のテストの前にheap_initとbootを済ませておく
#define HEAP_SIZE (1024 * 1024)

static void setup_heap(void) {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
    os_register_eval_primitives();
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

static void test_transpile_fixture_lambda_capture_value(void) {
    lisp_val_t x = os_make_fixnum(5);
    lisp_val_t result = lisp_ll_transpile_fixture_lambda_capture_value(os_make_cons(x, nil), 0);
    assert(result == x, "lambda-capture-value: 値コピー捕捉により、クロージャ内から見えるxは呼び出し元のxと同じ値");
}

static void test_transpile_fixture_lambda_box_mutate(void) {
    lisp_val_t n = os_make_fixnum(1);
    lisp_val_t result = lisp_ll_transpile_fixture_lambda_box_mutate(os_make_cons(n, nil), 0);
    assert((result >> 3) == 99, "lambda-box-mutate: ネストしたlambda内のsetqがboxを介して外側のnに伝播し99を返す");
}

static void test_transpile_fixture_make_counter_and_call_twice(void) {
    lisp_val_t initial = os_make_fixnum(5);
    lisp_val_t closure = lisp_ll_transpile_fixture_make_counter(os_make_cons(initial, nil), 0);
    lisp_val_t result = lisp_ll_transpile_fixture_call_twice(os_make_cons(closure, nil), 0);
    assert((result >> 3) == 3,
           "make-counter/call-twice: make-counterのスタックフレームが失われた後も、"
           "返されたクロージャをcall-twiceが2回funcallすることでboxを共有したまま5→4→3とデクリメントする");
}

static void test_transpile_fixture_defdynamic(void) {
    lisp_val_t n = os_make_fixnum(42);
    lisp_val_t result = lisp_ll_transpile_fixture_defdynamic(os_make_cons(n, nil), 0);
    assert(result == n,
           "defdynamic: %%test-dynamic-varへnを書き込んだ直後にdynamicで読み出すとnそのものが返る");
}

static void test_transpile_fixture_dynamic_read(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_dynamic_read(nil, 0);
    assert((result >> 3) == 42,
           "dynamic: レキシカルな親子関係を持たない別の関数呼び出しからでも、"
           "直前のdefdynamicがg_dynamic_bindingsへ書き込んだ%%test-dynamic-varの値がそのまま読める");
}

static void test_transpile_fixture_let(void) {
    lisp_val_t x = os_make_fixnum(7);
    lisp_val_t result = lisp_ll_transpile_fixture_let(os_make_cons(x, nil), 0);
    assert(result == x, "let: (let ((y x)) y)はxをyへ束縛して評価するのでxそのものが返る");
}

static void test_transpile_fixture_let_multi(void) {
    lisp_val_t x = os_make_fixnum(1);
    lisp_val_t y = os_make_fixnum(2);
    lisp_val_t result = lisp_ll_transpile_fixture_let_multi(os_make_cons(x, os_make_cons(y, nil)), 0);
    assert(cc_car(result) == x, "let-multi: aはxへ並列に束縛される");
    assert(cc_cdr(result) == y, "let-multi: bはyへ並列に束縛される(bの初期化式はaを参照しない)");
}

static void test_transpile_fixture_let_body_progn(void) {
    lisp_val_t x = os_make_fixnum(3);
    lisp_val_t result = lisp_ll_transpile_fixture_let_body_progn(os_make_cons(x, nil), 0);
    assert(cc_car(result) == x, "let-body-progn: setqで書き換えた後のyのcarは元のxのまま");
    assert(cc_cdr(result) == x, "let-body-progn: bodyの2式目(y)がsetq後の値(cons y y)を返す(逐次評価の確認)");
}

static void test_transpile_fixture_let_star(void) {
    lisp_val_t x = os_make_fixnum(9);
    lisp_val_t result = lisp_ll_transpile_fixture_let_star(os_make_cons(x, nil), 0);
    assert(cc_car(result) == x, "let-star: bの初期化式(cons a a)が同じlet*内のaを参照できる(carはx)");
    assert(cc_cdr(result) == x, "let-star: bの初期化式(cons a a)が同じlet*内のaを参照できる(cdrもx)");
}

static void test_transpile_fixture_cond(void) {
    lisp_val_t x = os_make_fixnum(1);
    lisp_val_t y = os_make_fixnum(2);

    lisp_val_t result_first = lisp_ll_transpile_fixture_cond(os_make_cons(x, os_make_cons(y, nil)), 0);
    assert(result_first == x, "cond: xがnil以外なら最初のclauseがマッチしxを返す");

    lisp_val_t result_second = lisp_ll_transpile_fixture_cond(os_make_cons(nil, os_make_cons(y, nil)), 0);
    assert(result_second == y, "cond: xがnilでyがnil以外なら2番目のclauseがマッチしyを返す");

    lisp_val_t result_default = lisp_ll_transpile_fixture_cond(os_make_cons(nil, os_make_cons(nil, nil)), 0);
    assert(result_default == os_make_symbol("COND-FALLBACK"),
           "cond: どのclauseもマッチしなければt節(デフォルト)のシンボルを返す");
}

static void test_transpile_fixture_cond_body_progn(void) {
    lisp_val_t x = os_make_fixnum(3);
    lisp_val_t result_true = lisp_ll_transpile_fixture_cond_body_progn(os_make_cons(x, nil), 0);
    assert(cc_car(result_true) == x, "cond-body-progn: testが真ならsetq後のxのcarは元のxのまま");
    assert(cc_cdr(result_true) == x, "cond-body-progn: bodyの2式目(x)がsetq後の値(cons x x)を返す(逐次評価の確認)");

    lisp_val_t result_false = lisp_ll_transpile_fixture_cond_body_progn(os_make_cons(nil, nil), 0);
    assert(result_false == nil, "cond-body-progn: testが偽ならt節のnilを返す");
}

static void test_transpile_fixture_case(void) {
    lisp_val_t result_one = lisp_ll_transpile_fixture_case(os_make_cons(os_make_fixnum(1), nil), 0);
    assert(result_one == os_make_symbol("ONE-OR-TWO"), "case: 1は(1 2)のkeylistにmemberでマッチしone-or-twoを返す");

    lisp_val_t result_two = lisp_ll_transpile_fixture_case(os_make_cons(os_make_fixnum(2), nil), 0);
    assert(result_two == os_make_symbol("ONE-OR-TWO"), "case: 2も(1 2)のkeylistにmemberでマッチしone-or-twoを返す");

    lisp_val_t result_three = lisp_ll_transpile_fixture_case(os_make_cons(os_make_fixnum(3), nil), 0);
    assert(result_three == os_make_symbol("THREE"), "case: 3は(3)のkeylistにマッチしthreeを返す");

    lisp_val_t result_other = lisp_ll_transpile_fixture_case(os_make_cons(os_make_fixnum(99), nil), 0);
    assert(result_other == os_make_symbol("OTHER"), "case: どのkeylistにもマッチしなければt節のotherを返す");
}

static void test_transpile_fixture_case_using(void) {
    lisp_val_t result_one = lisp_ll_transpile_fixture_case_using(os_make_cons(os_make_fixnum(1), nil), 0);
    assert(result_one == os_make_symbol("ONE-OR-TWO"), "case-using: 1は%case-using-match経由で(1 2)にマッチしone-or-twoを返す");

    lisp_val_t result_three = lisp_ll_transpile_fixture_case_using(os_make_cons(os_make_fixnum(3), nil), 0);
    assert(result_three == os_make_symbol("THREE"), "case-using: 3は%case-using-match経由で(3)にマッチしthreeを返す");

    lisp_val_t result_other = lisp_ll_transpile_fixture_case_using(os_make_cons(os_make_fixnum(99), nil), 0);
    assert(result_other == os_make_symbol("OTHER"), "case-using: どのkeylistにもマッチしなければt節のotherを返す");
}

static void test_transpile_fixture_setf_setq(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_setf_setq(os_make_cons(os_make_fixnum(1), nil), 0);
    assert((result >> 3) == 42, "setf: placeがsymbolならsetqへ展開され新しい値(42)が返る");
}

static void test_transpile_fixture_setf_car(void) {
    lisp_val_t pair = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
    lisp_val_t result = lisp_ll_transpile_fixture_setf_car(os_make_cons(pair, nil), 0);
    assert(cc_car(result) == os_make_fixnum(42), "setf: (car x)はset-carへ展開されcarが書き換わる");
    assert(cc_cdr(result) == os_make_fixnum(2), "setf: (car x)への書き込みはcdrへ影響しない");
}

static void test_transpile_fixture_setf_cdr(void) {
    lisp_val_t pair = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
    lisp_val_t result = lisp_ll_transpile_fixture_setf_cdr(os_make_cons(pair, nil), 0);
    assert(cc_car(result) == os_make_fixnum(1), "setf: (cdr x)への書き込みはcarへ影響しない");
    assert(cc_cdr(result) == os_make_fixnum(42), "setf: (cdr x)はset-cdrへ展開されcdrが書き換わる");
}

static void test_transpile_fixture_setf_aref(void) {
    lisp_val_t arr = primitive_make_array(os_make_cons(os_make_fixnum(3), nil), nil);
    lisp_val_t result = lisp_ll_transpile_fixture_setf_aref(os_make_cons(arr, nil), 0);
    lisp_val_t elem = primitive_aref(os_make_cons(result, os_make_cons(os_make_fixnum(0), nil)), nil);
    assert(elem == os_make_fixnum(42), "setf: (aref x i)はset-arefへ展開され添字0の要素が書き換わる");
}

static void test_transpile_fixture_setf_elt(void) {
    lisp_val_t lst = os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(2), nil));
    lisp_val_t result = lisp_ll_transpile_fixture_setf_elt(os_make_cons(lst, nil), 0);
    assert(cc_car(result) == os_make_fixnum(1), "setf: (elt x 1)への書き込みは0番目の要素に影響しない");
    assert(cc_car(cc_cdr(result)) == os_make_fixnum(99),
           "setf: (elt x i)はset-eltへ展開され(値が先頭引数という順序で)1番目の要素が書き換わる");
}

static void test_transpile_fixture_setf_slot_value(void) {
    lisp_val_t slot_descriptor = os_make_cons(os_make_symbol("VAL"), nil);
    lisp_val_t slots = os_make_cons(slot_descriptor, nil);
    lisp_val_t class = primitive_make_class_raw(
        os_make_cons(os_make_symbol("POINT"), os_make_cons(nil, os_make_cons(slots, nil))), nil);
    lisp_val_t slots_vector = primitive_make_array(os_make_cons(os_make_fixnum(1), nil), nil);
    lisp_val_t instance = primitive_make_instance_raw(
        os_make_cons(class, os_make_cons(slots_vector, nil)), nil);

    lisp_val_t result = lisp_ll_transpile_fixture_setf_slot_value(os_make_cons(instance, nil), 0);
    assert(result == instance, "setf: (slot-value x slot)はset-slot-valueへ展開されinstance自体を返す");

    lisp_val_t stored = primitive_aref(os_make_cons(slots_vector, os_make_cons(os_make_fixnum(0), nil)), nil);
    assert(stored == os_make_fixnum(42),
           "setf: (slot-value x 'val)への書き込みが%slot-index経由でslots-vectorの正しい添字に反映される");
}

static void test_transpile_fixture_rest(void) {
    lisp_val_t a = os_make_fixnum(1);
    lisp_val_t b = os_make_fixnum(2);
    lisp_val_t c = os_make_fixnum(3);

    lisp_val_t result_empty = lisp_ll_transpile_fixture_rest(nil, 0);
    assert(result_empty == nil, "rest: 実引数無しで呼ぶとitemsはnilになる");

    lisp_val_t result_many = lisp_ll_transpile_fixture_rest(os_make_cons(a, os_make_cons(b, os_make_cons(c, nil))), 0);
    assert(cc_car(result_many) == a, "rest: 1番目の実引数がitemsの先頭に束縛される");
    assert(cc_car(cc_cdr(result_many)) == b, "rest: 2番目の実引数がitemsの2番目に束縛される");
    assert(cc_car(cc_cdr(cc_cdr(result_many))) == c, "rest: 3番目の実引数がitemsの3番目に束縛される");
    assert(cc_cdr(cc_cdr(cc_cdr(result_many))) == nil, "rest: 実引数の個数分だけでitemsが終端する");
}

static void test_transpile_fixture_rest_with_fixed(void) {
    lisp_val_t a = os_make_fixnum(10);
    lisp_val_t b = os_make_fixnum(20);
    lisp_val_t c = os_make_fixnum(30);

    lisp_val_t result = lisp_ll_transpile_fixture_rest_with_fixed(os_make_cons(a, os_make_cons(b, os_make_cons(c, nil))), 0);
    assert(cc_car(result) == a, "rest-with-fixed: 固定パラメータaはcc_car/cc_cdrで通常通り1番目の実引数へ束縛される");
    assert(cc_car(cc_cdr(result)) == b, "rest-with-fixed: 残りのevaluated_args(2番目以降)がitemsへそのまま束縛される(その1)");
    assert(cc_car(cc_cdr(cc_cdr(result))) == c, "rest-with-fixed: 残りのevaluated_args(2番目以降)がitemsへそのまま束縛される(その2)");
}

static void test_transpile_fixture_for_sum(void) {
    lisp_val_t result_five = lisp_ll_transpile_fixture_for_sum(os_make_cons(os_make_fixnum(5), nil), 0);
    assert(result_five == os_make_fixnum(15), "for: 1から5までの合計は15(bindingのstep式と反復終了判定の確認)");

    lisp_val_t result_zero = lisp_ll_transpile_fixture_for_sum(os_make_cons(os_make_fixnum(0), nil), 0);
    assert(result_zero == os_make_fixnum(0), "for: nが0なら1回目のtestで即座にresult(sum=0)を返す");
}

static void test_transpile_fixture_for_early_exit(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_for_early_exit(os_make_cons(os_make_fixnum(10), nil), 0);
    lisp_val_t exit_value = cc_car(result);
    lisp_val_t count = cc_cdr(result);
    assert(exit_value == os_make_fixnum(999),
           "for: 本体中のreturn-fromの値999がfor式全体の値になる(test-and-resultのresultではない)");
    assert(count == os_make_fixnum(203),
           "for: return-from以降は同一反復の残りのbodyも残りの反復も実行されない(countが203で止まる)");
}

static void test_transpile_fixture_while_sum(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_while_sum(os_make_cons(os_make_fixnum(4), nil), 0);
    assert(result == os_make_fixnum(10), "while: 4+3+2+1を加算した結果10を返す(tagbody/goループの基本動作)");
}

static void test_transpile_fixture_while_named_block_exit(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_while_named_block_exit(os_make_cons(os_make_fixnum(10), nil), 0);
    lisp_val_t exit_value = cc_car(result);
    lisp_val_t count = cc_cdr(result);
    assert(exit_value == os_make_symbol("STOPPED"),
           "while: while自身のblock nilを素通りし外側の名前付きblockまでreturn-fromが伝播しstoppedを返す");
    assert(count == os_make_fixnum(3),
           "while: countが3になった時点で脱出し以降の反復(nの残りの減算)は実行されない");
}

static void test_transpile_fixture_unwind_protect_normal(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_unwind_protect_normal(os_make_cons(os_make_fixnum(5), nil), 0);
    lisp_val_t value = cc_car(result);
    lisp_val_t count = cc_cdr(result);
    assert(value == os_make_fixnum(6), "unwind-protect: protected-formの値(n+1=6)が式全体の値になる");
    assert(count == os_make_fixnum(1), "unwind-protect: cleanup-formは必ず1回実行される");
}

static void test_transpile_fixture_unwind_protect_non_local_exit(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_unwind_protect_non_local_exit(0, 0);
    lisp_val_t exit_value = cc_car(result);
    lisp_val_t count = cc_cdr(result);
    assert(exit_value == os_make_fixnum(42),
           "unwind-protect: protected-form中のreturn-fromの値42がunwind-protect式全体の値として外側のblockまで伝播する");
    assert(count == os_make_fixnum(1),
           "unwind-protect: 非局所脱出であってもcleanup-formは必ず実行される");
}

static void test_transpile_fixture_unwind_protect_cleanup_exit_ignored(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_unwind_protect_cleanup_exit_ignored(0, 0);
    assert(result == os_make_fixnum(7),
           "unwind-protect: cleanup-form自身のreturn-fromは無視され、protected-formの結果(7)が式全体の値になる");
}

static void test_transpile_fixture_with_open_input_stream(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_with_open_input_stream(0, 0);
    assert(result == nil,
           "with-open-input-stream: body正常終了後、unwind-protect経由でstreamが必ずcloseされる(open-stream-pがnil)");
}

static void test_transpile_fixture_with_open_input_stream_early_exit(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_with_open_input_stream_early_exit(0, 0);
    lisp_val_t exit_value = cc_car(result);
    lisp_val_t still_open = cc_cdr(result);
    assert(exit_value == os_make_fixnum(42),
           "with-open-input-stream: body中のreturn-fromの値42がwith-open-input-stream式全体の値として伝播する");
    assert(still_open == nil,
           "with-open-input-stream: return-fromによる早期脱出でもunwind-protect経由で必ずcloseされる");
}

static void test_transpile_fixture_with_open_input_file(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_with_open_input_file(0, 0);
    assert(result == nil,
           "with-open-input-file: with-open-input-stream経由でも同様にcloseが保証される(open-stream-pがnil)");
}

static void test_transpile_fixture_with_open_output_stream_early_exit(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_with_open_output_stream_early_exit(0, 0);
    lisp_val_t exit_value = cc_car(result);
    lisp_val_t still_open = cc_cdr(result);
    assert(exit_value == os_make_fixnum(42),
           "with-open-output-stream: body中のreturn-fromの値42がwith-open-output-stream式全体の値として伝播する");
    assert(still_open == nil,
           "with-open-output-stream: return-fromによる早期脱出でもunwind-protect経由で必ずcloseされる");
}

static void test_transpile_fixture_with_open_output_file(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_with_open_output_file(0, 0);
    assert(result == nil,
           "with-open-output-file: open-output-fileで開いたstreamもwith-open-output-stream経由でcloseが保証される(open-stream-pがnil)");
}

static void test_transpile_fixture_catch_throw_basic(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_catch_throw_basic(0, 0);
    assert(result == os_make_fixnum(42),
           "catch/throw: throwされた値(42)がcatch式全体の値になる");
}

static void test_transpile_fixture_catch_no_throw(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_catch_no_throw(0, 0);
    assert(result == os_make_fixnum(3),
           "catch: throwが起きない場合はprognと同じくbody最後のformの値を返す");
}

static void test_transpile_fixture_catch_mismatched_tag(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_catch_mismatched_tag(0, 0);
    assert(result == os_make_fixnum(99),
           "catch/throw: タグが一致しない内側のcatchは素通りし、一致する外側のcatchで捕捉される");
}

static void test_transpile_fixture_catch_throw_runtime_tag(void) {
    lisp_val_t tag = os_make_symbol("ISIKI-TEST-DYNAMIC-TAG");
    lisp_val_t result = lisp_ll_transpile_fixture_catch_throw_runtime_tag(os_make_cons(tag, nil), 0);
    assert(cc_car(result) == os_make_symbol("CAUGHT"),
           "catch/throw: タグは実行時に評価された値同士のeq比較で一致する");
    assert(cc_cdr(result) == tag,
           "catch/throw: throwに渡した実行時タグの値がそのまま伝わる");
}

static void test_transpile_fixture_catch_with_cleanup(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_catch_with_cleanup(0, 0);
    lisp_val_t exit_value = cc_car(result);
    lisp_val_t count = cc_cdr(result);
    assert(exit_value == os_make_fixnum(7),
           "catch/throw: unwind-protectのprotected-form中のthrowの値がcatch式全体の値として伝播する");
    assert(count == os_make_fixnum(1),
           "catch/throw: throwによる非局所脱出であってもunwind-protectのcleanup-formは必ず実行される");
}

static void test_transpile_fixture_register_and_find_class(void) {
    lisp_val_t name = os_make_symbol("ISIKI-TEST-CLASS-REGISTRY-NAME");
    lisp_val_t value = os_make_symbol("ISIKI-TEST-CLASS-REGISTRY-VALUE");
    lisp_val_t result = lisp_ll_transpile_fixture_register_and_find_class(
        os_make_cons(name, os_make_cons(value, nil)), 0);
    assert(result == value,
           "%register-class/%find-class: 登録した値をそのままeqで引ける");
}

static void test_transpile_fixture_find_class_missing(void) {
    lisp_val_t name = os_make_symbol("ISIKI-TEST-CLASS-REGISTRY-MISSING");
    lisp_val_t result = lisp_ll_transpile_fixture_find_class_missing(os_make_cons(name, nil), 0);
    assert(result == nil,
           "%find-class: 未登録の名前に対してはnilを返す");
}

static void test_transpile_fixture_register_builtin_class_then_find(void) {
    lisp_val_t name = os_make_symbol("ISIKI-TEST-BUILTIN-CLASS-NAME");
    lisp_val_t result = lisp_ll_transpile_fixture_register_builtin_class_then_find(
        os_make_cons(name, nil), 0);
    assert(result == g_sym_t,
           "%register-builtin-class: 登録したクラスオブジェクトを直後の%find-classがeqで引ける");
}

static void test_transpile_fixture_slot_value_read(void) {
    lisp_val_t slot_descriptor = os_make_cons(os_make_symbol("VAL"), nil);
    lisp_val_t slots = os_make_cons(slot_descriptor, nil);
    lisp_val_t class = primitive_make_class_raw(
        os_make_cons(os_make_symbol("POINT"), os_make_cons(nil, os_make_cons(slots, nil))), nil);
    lisp_val_t slots_vector = primitive_make_array(os_make_cons(os_make_fixnum(1), nil), nil);
    primitive_set_aref(os_make_cons(slots_vector,
        os_make_cons(os_make_fixnum(0), os_make_cons(os_make_fixnum(42), nil))), nil);
    lisp_val_t instance = primitive_make_instance_raw(
        os_make_cons(class, os_make_cons(slots_vector, nil)), nil);

    lisp_val_t result = lisp_ll_transpile_fixture_slot_value_read(
        os_make_cons(instance, os_make_cons(os_make_symbol("VAL"), nil)), 0);
    assert(result == os_make_fixnum(42),
           "slot-value: instanceのスロットベクタから%slot-indexが算出した添字の値を読む");
}

static void test_transpile_fixture_fill_slots(void) {
    lisp_val_t vec = primitive_make_array(os_make_cons(os_make_fixnum(2), nil), nil);
    lisp_val_t a = os_make_fixnum(10);
    lisp_val_t b = os_make_fixnum(20);
    lisp_val_t values = os_make_cons(a, os_make_cons(b, nil));

    lisp_val_t result = lisp_ll_transpile_fixture_fill_slots(os_make_cons(vec, os_make_cons(values, nil)), 0);
    assert(result == vec, "%fill-slots: vec自身を返す");
    assert(primitive_aref(os_make_cons(vec, os_make_cons(os_make_fixnum(0), nil)), nil) == a,
           "%fill-slots: valuesの1番目がvecの添字0に書き込まれる");
    assert(primitive_aref(os_make_cons(vec, os_make_cons(os_make_fixnum(1), nil)), nil) == b,
           "%fill-slots: valuesの2番目がvecの添字1に書き込まれる");
}

static void test_transpile_fixture_subclassp(void) {
    lisp_val_t class_a = primitive_make_class_raw(
        os_make_cons(os_make_symbol("A"), os_make_cons(nil, os_make_cons(nil, nil))), nil);
    lisp_val_t class_b = primitive_make_class_raw(
        os_make_cons(os_make_symbol("B"), os_make_cons(os_make_cons(class_a, nil), os_make_cons(nil, nil))), nil);
    lisp_val_t class_c = primitive_make_class_raw(
        os_make_cons(os_make_symbol("C"), os_make_cons(os_make_cons(class_b, nil), os_make_cons(nil, nil))), nil);

    assert(lisp_ll_transpile_fixture_subclassp(os_make_cons(class_c, os_make_cons(class_c, nil)), 0) == g_sym_t,
           "subclassp: 自分自身はサブクラス(eq)としてtを返す");
    assert(lisp_ll_transpile_fixture_subclassp(os_make_cons(class_c, os_make_cons(class_a, nil)), 0) == g_sym_t,
           "subclassp: C -> B -> Aと%%class-supersを推移的に辿ってtを返す");
    assert(lisp_ll_transpile_fixture_subclassp(os_make_cons(class_a, os_make_cons(class_c, nil)), 0) == nil,
           "subclassp: 逆方向(親から子)はサブクラスではないのでnilを返す");
}

static void test_transpile_fixture_class_of_builtin(void) {
    /* class-ofのconsp分岐は内部で(%find-class '<cons>)を呼ぶため、任意の名前
       ではなく実際に参照される<CONS>という名前で登録しないとeqが成立しない */
    lisp_val_t name = os_make_symbol("<CONS>");
    lisp_val_t obj = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));

    assert(lisp_ll_transpile_fixture_class_of_builtin(os_make_cons(name, os_make_cons(obj, nil)), 0) == g_sym_t,
           "class-of: consp分岐が(%find-class '<cons>)経由でその場で%register-builtin-classしたクラスとeqで一致する");
}

static void test_transpile_fixture_class_of_instance(void) {
    lisp_val_t class = primitive_make_class_raw(
        os_make_cons(os_make_symbol("ISIKI-TEST-CLASS-OF-INSTANCE-CLASS"), os_make_cons(nil, os_make_cons(nil, nil))), nil);
    lisp_val_t instance = primitive_make_instance_raw(os_make_cons(class, os_make_cons(nil, nil)), nil);

    assert(lisp_ll_transpile_fixture_class_of_instance(os_make_cons(instance, nil), 0) == class,
           "class-of: %%class-instance-p分岐が組み込み型判定より先にinstanceの直接のクラスを返す");
}

static void test_transpile_fixture_typep_instancep(void) {
    lisp_val_t class_a = primitive_make_class_raw(
        os_make_cons(os_make_symbol("ISIKI-TEST-TYPEP-A"), os_make_cons(nil, os_make_cons(nil, nil))), nil);
    lisp_val_t class_b = primitive_make_class_raw(
        os_make_cons(os_make_symbol("ISIKI-TEST-TYPEP-B"), os_make_cons(os_make_cons(class_a, nil), os_make_cons(nil, nil))), nil);
    lisp_val_t instance_b = primitive_make_instance_raw(os_make_cons(class_b, os_make_cons(nil, nil)), nil);
    lisp_val_t name_a = os_make_symbol("ISIKI-TEST-TYPEP-NAME-A");
    lisp_val_t classes_sym = os_make_symbol("*CLASSES*");
    os_set_dynamic(classes_sym, os_make_cons(os_make_cons(name_a, class_a), os_get_dynamic(classes_sym)));

    assert(lisp_ll_transpile_fixture_typep(os_make_cons(instance_b, os_make_cons(class_a, nil)), 0) == g_sym_t,
           "typep: class-designatorがクラスオブジェクトそのもの(%%classp)の場合、subclassp経由で親クラスに一致する");
    assert(lisp_ll_transpile_fixture_typep(os_make_cons(instance_b, os_make_cons(name_a, nil)), 0) == g_sym_t,
           "typep: class-designatorがクラス名シンボルの場合、%find-class経由で解決してから一致する");
    assert(lisp_ll_transpile_fixture_typep(os_make_cons(instance_b, os_make_cons(class_b, nil)), 0) == g_sym_t,
           "typep: instance自身のクラスとも一致する");
    assert(lisp_ll_transpile_fixture_instancep(os_make_cons(instance_b, os_make_cons(class_a, nil)), 0) == g_sym_t,
           "instancep: typepへの委譲がそのまま成り立つ");
}

static void test_transpile_fixture_generic_dispatch(void) {
    lisp_val_t gf_name = os_make_symbol("ISIKI-TEST-GENERIC-DISPATCH");
    lisp_val_t arg = os_make_fixnum(42);
    lisp_val_t result = lisp_ll_transpile_fixture_generic_dispatch(os_make_cons(gf_name, os_make_cons(arg, nil)), 0);

    assert(cc_car(result) == os_make_symbol("MATCHED"),
           "%generic-call: specializer無指定の1メソッドがargで正しく呼ばれる(car)");
    assert(cc_cdr(result) == arg,
           "%generic-call: specializer無指定の1メソッドがargで正しく呼ばれる(cdr)");
}

static void test_transpile_fixture_generic_dispatch_order(void) {
    lisp_val_t gf_name = os_make_symbol("ISIKI-TEST-GENERIC-DISPATCH-ORDER");
    lisp_val_t arg = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));

    lisp_val_t result = lisp_ll_transpile_fixture_generic_dispatch_order(os_make_cons(gf_name, os_make_cons(arg, nil)), 0);

    assert(result == os_make_symbol("SPECIFIC"),
           "%order-methods: <cons>指定のメソッドがspecializer無指定のメソッドより特定的として先に選ばれる");
}

static void test_transpile_fixture_call_next_method(void) {
    lisp_val_t gf_name = os_make_symbol("ISIKI-TEST-CALL-NEXT-METHOD");
    lisp_val_t arg = os_make_cons(os_make_fixnum(3), os_make_fixnum(4));
    /* lisp_ll_transpile_fixture_call_next_method呼び出し中に発火するGCがargを
       移動させる可能性があるため、呼び出しを跨いでeq比較する変数はGC_PROTECTする
       (argはコピー先compact GC後もこのローカル変数が指す先が更新されて初めて
       resultとeqになる) */
    GC_PROTECT(arg);

    lisp_val_t result = lisp_ll_transpile_fixture_call_next_method(os_make_cons(gf_name, os_make_cons(arg, nil)), 0);

    assert(cc_car(result) == os_make_symbol("GENERAL"),
           "call-next-method: 特定的なメソッドから次の(汎用)メソッドへ正しく処理が引き渡される(car)");
    assert(cc_cdr(result) == arg,
           "call-next-method: 特定的なメソッドから次の(汎用)メソッドへ正しく処理が引き渡される(cdr)");
}

static void test_transpile_fixture_generic_no_applicable_method(void) {
    /* このC側テスト環境はinit.lispをロードしないため、%invoke-method-chainの
       「no applicable method」分岐が%%funcall-by-name経由で呼ぶerrorは未定義
       (os_get_functionがnilを返す)。os_signal_conditionと同じ規約で
       g_sym_eval_errorへ安全にフォールバックすることを確認する */
    lisp_val_t gf_name = os_make_symbol("ISIKI-TEST-GENERIC-NO-APPLICABLE-METHOD");
    lisp_val_t arg = os_make_fixnum(1);

    assert(lisp_ll_transpile_fixture_generic_no_applicable_method(os_make_cons(gf_name, os_make_cons(arg, nil)), 0) == g_sym_eval_error,
           "%invoke-method-chain: 適用可能なメソッドが無い場合、未定義のerrorを%%funcall-by-name経由で呼びg_sym_eval_errorへフォールバックする");
}

static void test_transpile_fixture_call_next_method_no_next(void) {
    /* アクティブなメソッド呼び出しフレームが無い状態でcall-next-methodを直接呼ぶ。
       「no next method」分岐も同じく%%funcall-by-name経由でerror(未定義)を呼び、
       g_sym_eval_errorへフォールバックする */
    assert(lisp_ll_transpile_fixture_call_next_method_no_next(nil, 0) == g_sym_eval_error,
           "call-next-method: 次のメソッドが無い場合、未定義のerrorを%%funcall-by-name経由で呼びg_sym_eval_errorへフォールバックする");
}

static void test_transpile_fixture_make_instance(void) {
    /* class-designatorがクラスオブジェクト自身の場合の%%classp分岐、
       %%make-instance-raw+make-arrayによるインスタンス確保、固定の
       ジェネリック関数名'initialize-objectを%generic-call経由で呼ぶことを
       一貫して確認する(M12 Phase 6、#27) */
    lisp_val_t initial_value = os_make_fixnum(42);
    lisp_val_t result = lisp_ll_transpile_fixture_make_instance(os_make_cons(initial_value, nil), 0);
    assert(result == initial_value,
           "make-instance: %generic-call経由で呼ばれたinitialize-objectがinitargsの値をスロットへ書き込む");
}

// signal-conditionと同じ形の多段ネストclosure捕捉を再現するfuncall先。
// outer/innerをそのままconsで返すことで、呼び出し時点でのouter/innerの値が
// 正しく捕捉されているかを検証できる
static lisp_val_t test_nested_let_dynamic_restore_fn(lisp_val_t evaluated_args, lisp_val_t env) {
    (void)env;
    lisp_val_t outer = cc_car(evaluated_args);
    lisp_val_t inner = cc_car(cc_cdr(evaluated_args));
    return os_make_cons(outer, inner);
}

static void test_transpile_fixture_nested_let_dynamic_restore(void) {
    /* M12 Phase 8(#27)デバッグ用: signal-conditionと同型の
       let(outer)→let(inner)→catch→unwind-protect→%%set-dynamic→
       let(result)→funcall→cleanupで%%set-dynamic復元、という多段ネストが
       正しく動くかを直接確認する */
    lisp_val_t dyn_sym = os_make_symbol("*%%FIXTURE-DYN*");
    os_set_dynamic(dyn_sym, os_make_symbol("INITIAL-VAL"));
    lisp_val_t fn = os_make_native_function((lisp_addr_t)(void *)test_nested_let_dynamic_restore_fn);

    lisp_val_t result = lisp_ll_transpile_fixture_nested_let_dynamic_restore(os_make_cons(fn, nil), 0);

    assert(cc_car(result) == os_make_symbol("OUTER-VAL"),
           "nested-let-dynamic-restore: funcall呼び出し時点でouterがOUTER-VALのまま正しく捕捉されている");
    assert(cc_cdr(result) == os_make_symbol("INNER-VAL"),
           "nested-let-dynamic-restore: funcall呼び出し時点でinnerがINNER-VALのまま正しく捕捉されている");
    assert(os_get_dynamic(dyn_sym) == os_make_symbol("OUTER-VAL"),
           "nested-let-dynamic-restore: unwind-protectのcleanupが*%%fixture-dyn*をouterの値へ正しく復元する");
}

static lisp_val_t test_signal_condition_like_handler_fn(lisp_val_t evaluated_args, lisp_val_t env) {
    (void)env;
    return cc_car(evaluated_args);
}

static void test_transpile_fixture_signal_condition_like(void) {
    /* M12 Phase 8(#27)デバッグ用: signal-conditionの構造をより厳密に再現した
       fixtureで、呼び出し後に*%%fixture-dyn*(=handlers相当)が元のリストへ
       正しく復元されるかを確認する */
    lisp_val_t dyn_sym = os_make_symbol("*%%FIXTURE-DYN*");
    lisp_val_t handler_fn = os_make_native_function((lisp_addr_t)(void *)test_signal_condition_like_handler_fn);
    lisp_val_t original_handlers = os_make_cons(handler_fn, os_make_cons(handler_fn, nil));
    os_set_dynamic(dyn_sym, original_handlers);
    lisp_val_t marker = os_make_symbol("MARKER-VAL");

    lisp_val_t result = lisp_ll_transpile_fixture_signal_condition_like(os_make_cons(marker, nil), 0);

    assert(result == marker,
           "signal-condition-like: (car handlers)にmarkerを渡したfuncallの結果がそのまま返る");
    assert(os_get_dynamic(dyn_sym) == original_handlers,
           "signal-condition-like: unwind-protectのcleanupが*%%fixture-dyn*を元のhandlersリストへeqで復元する");
}

static void test_transpile_fixture_signal_condition_nonlocal(void) {
    /* M12 Phase 8(#27): with-handler/ignore-errorsの実際の使い方
       (handlerがそれ自身のlambdaより外側のblockへreturn-fromする)を再現する。
       handlerは(インタプリタ側の)os_evalで作ったインタプリタクロージャとし、
       signal-condition-like(AOT)から実際にfuncallで呼び出す。非局所脱出シグナルが
       catch/unwind-protect/letの3重ネストとfuncallのAOT/インタプリタ境界を
       飛び越えてblockまで正しく伝播し、かつunwind-protectのcleanupで
       *%%fixture-dyn*が元のhandlersへ復元されることを確認する */
    lisp_val_t dyn_sym = os_make_symbol("*%%FIXTURE-DYN*");
    lisp_val_t block_name = os_make_symbol("%%FIXTURE-NONLOCAL-BLOCK");

    lisp_val_t lambda_sym = os_make_symbol("LAMBDA");
    lisp_val_t return_from_sym = os_make_symbol("RETURN-FROM");
    lisp_val_t c_sym = os_make_symbol("C");
    lisp_val_t return_from_form = os_make_cons(
        return_from_sym, os_make_cons(block_name, os_make_cons(c_sym, nil)));
    lisp_val_t lambda_form = os_make_cons(
        lambda_sym,
        os_make_cons(os_make_cons(c_sym, nil), os_make_cons(return_from_form, nil)));
    lisp_val_t handler = os_eval(lambda_form, global_environment);

    lisp_val_t original_handlers = os_make_cons(handler, nil);
    os_set_dynamic(dyn_sym, original_handlers);

    lisp_val_t fixture_native = os_make_native_function(
        (lisp_addr_t)(void *)lisp_ll_transpile_fixture_signal_condition_like);
    lisp_val_t marker = os_make_fixnum(424242);
    lisp_val_t funcall_sym = os_make_symbol("FUNCALL");

    lisp_val_t body_form = os_make_cons(
        funcall_sym, os_make_cons(fixture_native, os_make_cons(marker, nil)));
    lisp_val_t block_sym = os_make_symbol("BLOCK");
    lisp_val_t block_form = os_make_cons(
        block_sym, os_make_cons(block_name, os_make_cons(body_form, nil)));

    lisp_val_t result = os_eval(block_form, global_environment);

    assert(result == marker,
           "signal-condition-nonlocal: handlerのreturn-fromした値がcatch/unwind-protect/letとfuncallを越えてblockまで正しく伝播する");
    assert(os_get_dynamic(dyn_sym) == original_handlers,
           "signal-condition-nonlocal: return-fromによる非局所脱出でもunwind-protectのcleanupが実行され*%%fixture-dyn*が元のhandlersへ復元される");
}

static void test_list(void) {
    lisp_val_t a = os_make_fixnum(1);
    lisp_val_t b = os_make_fixnum(2);

    lisp_val_t result_empty = lisp_ll_list(nil, 0);
    assert(result_empty == nil, "list: 実引数無しで呼ぶとnilを返す");

    lisp_val_t result = lisp_ll_list(os_make_cons(a, os_make_cons(b, nil)), 0);
    assert(cc_car(result) == a, "list: (list a b)のcarはa");
    assert(cc_car(cc_cdr(result)) == b, "list: (list a b)の2番目はb");
    assert(cc_cdr(cc_cdr(result)) == nil, "list: (list a b)は2要素で終端する");
}

static void test_append(void) {
    lisp_val_t a = os_make_fixnum(1);
    lisp_val_t b = os_make_fixnum(2);
    lisp_val_t c = os_make_fixnum(3);
    lisp_val_t d = os_make_fixnum(4);

    lisp_val_t result_empty = lisp_ll_append(nil, 0);
    assert(result_empty == nil, "append: 実引数無しで呼ぶとnilを返す");

    lisp_val_t list1 = os_make_cons(a, os_make_cons(b, nil));
    lisp_val_t result_one = lisp_ll_append(os_make_cons(list1, nil), 0);
    assert(result_one == list1, "append: 1個のリストのみを渡すとそのリスト自身を返す");

    lisp_val_t list2 = os_make_cons(c, os_make_cons(d, nil));
    lisp_val_t result = lisp_ll_append(os_make_cons(list1, os_make_cons(list2, nil)), 0);
    assert(cc_car(result) == a, "append: (append (1 2) (3 4))の1番目はa");
    assert(cc_car(cc_cdr(result)) == b, "append: (append (1 2) (3 4))の2番目はb");
    assert(cc_car(cc_cdr(cc_cdr(result))) == c, "append: (append (1 2) (3 4))の3番目はc");
    assert(cc_car(cc_cdr(cc_cdr(cc_cdr(result)))) == d, "append: (append (1 2) (3 4))の4番目はd");
    assert(cc_cdr(cc_cdr(cc_cdr(cc_cdr(result)))) == nil, "append: (append (1 2) (3 4))は4要素で終端する");
}

static void test_create_list(void) {
    lisp_val_t zero = os_make_fixnum(0);
    lisp_val_t two = os_make_fixnum(2);
    lisp_val_t e = os_make_fixnum(9);

    lisp_val_t result_zero = lisp_ll_create_list(os_make_cons(zero, nil), 0);
    assert(result_zero == nil, "create-list: (create-list 0)はnilを返す");

    lisp_val_t result_default = lisp_ll_create_list(os_make_cons(two, nil), 0);
    assert(cc_car(result_default) == nil, "create-list: initial-element省略時は各要素がnilになる(その1)");
    assert(cc_car(cc_cdr(result_default)) == nil, "create-list: initial-element省略時は各要素がnilになる(その2)");
    assert(cc_cdr(cc_cdr(result_default)) == nil, "create-list: (create-list 2)は2要素で終端する");

    lisp_val_t result_with_elt = lisp_ll_create_list(os_make_cons(two, os_make_cons(e, nil)), 0);
    assert(cc_car(result_with_elt) == e, "create-list: (create-list 2 9)は各要素がinitial-elementになる(その1)");
    assert(cc_car(cc_cdr(result_with_elt)) == e, "create-list: (create-list 2 9)は各要素がinitial-elementになる(その2)");
    assert(cc_cdr(cc_cdr(result_with_elt)) == nil, "create-list: (create-list 2 9)は2要素で終端する");
}

static void test_nreverse(void) {
    lisp_val_t a = os_make_fixnum(1);
    lisp_val_t b = os_make_fixnum(2);
    lisp_val_t c = os_make_fixnum(3);

    lisp_val_t result_empty = lisp_ll_nreverse(os_make_cons(nil, nil), 0);
    assert(result_empty == nil, "nreverse: (nreverse ())はnilを返す");

    lisp_val_t original = os_make_cons(a, os_make_cons(b, os_make_cons(c, nil)));
    lisp_val_t result = lisp_ll_nreverse(os_make_cons(original, nil), 0);
    assert(cc_car(result) == c, "nreverse: (nreverse (1 2 3))の1番目はc");
    assert(cc_car(cc_cdr(result)) == b, "nreverse: (nreverse (1 2 3))の2番目はb");
    assert(cc_car(cc_cdr(cc_cdr(result))) == a, "nreverse: (nreverse (1 2 3))の3番目はa");
    assert(cc_cdr(cc_cdr(cc_cdr(result))) == nil, "nreverse: (nreverse (1 2 3))は3要素で終端する");
}

static void test_apply(void) {
    lisp_val_t add_fn = os_make_native_function((lisp_addr_t)(void *)primitive_add);
    lisp_val_t tail_list = os_make_cons(os_make_fixnum(3), os_make_cons(os_make_fixnum(4), nil));
    lisp_val_t evaluated_args = os_make_cons(
        add_fn,
        os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(2), os_make_cons(tail_list, nil))));

    lisp_val_t result = lisp_ll_apply(evaluated_args, 0);
    assert((result >> 3) == 10,
           "apply: (apply #'+ 1 2 '(3 4))はobj*(1 2)と末尾list(3 4)を1本の引数リストに"
           "組み立てて+へ渡し10を返す");
}

static void test_mapcar(void) {
    lisp_val_t add_fn = os_make_native_function((lisp_addr_t)(void *)primitive_add);
    lisp_val_t list1 = os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(2), os_make_cons(os_make_fixnum(3), nil)));
    lisp_val_t list2 = os_make_cons(os_make_fixnum(10), os_make_cons(os_make_fixnum(20), os_make_cons(os_make_fixnum(30), nil)));

    lisp_val_t result = lisp_ll_mapcar(os_make_cons(add_fn, os_make_cons(list1, os_make_cons(list2, nil))), 0);
    assert((cc_car(result) >> 3) == 11, "mapcar: (mapcar #'+ '(1 2 3) '(10 20 30))の1番目は1+10=11");
    assert((cc_car(cc_cdr(result)) >> 3) == 22, "mapcar: 2番目は2+20=22");
    assert((cc_car(cc_cdr(cc_cdr(result))) >> 3) == 33, "mapcar: 3番目は3+30=33");
    assert(cc_cdr(cc_cdr(cc_cdr(result))) == nil, "mapcar: 3要素で終端する");
}

// mapcの検証専用: funcallされた回数だけ要素の値を積算する副作用付きネイティブ関数
static int g_mapc_side_effect_sum = 0;
static lisp_val_t test_mapc_side_effect_fn(lisp_val_t evaluated_args, lisp_val_t env) {
    (void)env;
    g_mapc_side_effect_sum += (cc_car(evaluated_args) >> 3);
    return nil;
}

static void test_mapc(void) {
    g_mapc_side_effect_sum = 0;
    lisp_val_t fn = os_make_native_function((lisp_addr_t)(void *)test_mapc_side_effect_fn);
    lisp_val_t list1 = os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(2), os_make_cons(os_make_fixnum(3), nil)));

    lisp_val_t result = lisp_ll_mapc(os_make_cons(fn, os_make_cons(list1, nil)), 0);
    assert(result == list1, "mapc: 副作用目的でfnを適用した後、list自身を返す");
    assert(g_mapc_side_effect_sum == 6, "mapc: fnがlistの各要素(1,2,3)に対してfuncallされ、副作用の合計が1+2+3=6になる");
}

// mapcanの検証専用: xを2回並べたリストを返すネイティブ関数(mapcanのappend連結の検証用)
static lisp_val_t test_mapcan_double_fn(lisp_val_t evaluated_args, lisp_val_t env) {
    (void)env;
    lisp_val_t x = cc_car(evaluated_args);
    return os_make_cons(x, os_make_cons(x, nil));
}

static void test_mapcan(void) {
    lisp_val_t fn = os_make_native_function((lisp_addr_t)(void *)test_mapcan_double_fn);
    lisp_val_t list1 = os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(2), nil));

    lisp_val_t result = lisp_ll_mapcan(os_make_cons(fn, os_make_cons(list1, nil)), 0);
    assert((cc_car(result) >> 3) == 1, "mapcan: (mapcan fn '(1 2))の1番目は1");
    assert((cc_car(cc_cdr(result)) >> 3) == 1, "mapcan: 2番目もfn(1)がappendした2つ目の1");
    assert((cc_car(cc_cdr(cc_cdr(result))) >> 3) == 2, "mapcan: 3番目はfn(2)がappendした1つ目の2");
    assert((cc_car(cc_cdr(cc_cdr(cc_cdr(result)))) >> 3) == 2, "mapcan: 4番目はfn(2)がappendした2つ目の2");
    assert(cc_cdr(cc_cdr(cc_cdr(cc_cdr(result)))) == nil, "mapcan: 4要素で終端する");
}

// mapconの検証専用: sublistの先頭要素を2回並べたリストを返すネイティブ関数
// (mapcanと違いsublist自身を受け取ることを、carで取り出す形で検証する)
static lisp_val_t test_mapcon_double_car_fn(lisp_val_t evaluated_args, lisp_val_t env) {
    (void)env;
    lisp_val_t sublist = cc_car(evaluated_args);
    lisp_val_t x = cc_car(sublist);
    return os_make_cons(x, os_make_cons(x, nil));
}

static void test_mapcon(void) {
    lisp_val_t fn = os_make_native_function((lisp_addr_t)(void *)test_mapcon_double_car_fn);
    lisp_val_t list1 = os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(2), nil));

    lisp_val_t result = lisp_ll_mapcon(os_make_cons(fn, os_make_cons(list1, nil)), 0);
    assert((cc_car(result) >> 3) == 1, "mapcon: (mapcon fn '(1 2))の1番目はfnがsublist(1 2)のcar(1)から作る1");
    assert((cc_car(cc_cdr(result)) >> 3) == 1, "mapcon: 2番目もfn(sublist (1 2))がappendした2つ目の1");
    assert((cc_car(cc_cdr(cc_cdr(result))) >> 3) == 2, "mapcon: 3番目はfn(sublist (2))がappendした1つ目の2");
    assert((cc_car(cc_cdr(cc_cdr(cc_cdr(result)))) >> 3) == 2, "mapcon: 4番目はfn(sublist (2))がappendした2つ目の2");
    assert(cc_cdr(cc_cdr(cc_cdr(cc_cdr(result)))) == nil, "mapcon: 4要素で終端する");
}

static void test_map_into(void) {
    lisp_val_t add_fn = os_make_native_function((lisp_addr_t)(void *)primitive_add);
    lisp_val_t destination = os_make_cons(os_make_fixnum(0), os_make_cons(os_make_fixnum(0), os_make_cons(os_make_fixnum(0), nil)));
    lisp_val_t seq1 = os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(2), os_make_cons(os_make_fixnum(3), nil)));
    lisp_val_t seq2 = os_make_cons(os_make_fixnum(10), os_make_cons(os_make_fixnum(20), os_make_cons(os_make_fixnum(30), nil)));

    lisp_val_t evaluated_args = os_make_cons(
        destination, os_make_cons(add_fn, os_make_cons(seq1, os_make_cons(seq2, nil))));
    lisp_val_t result = lisp_ll_map_into(evaluated_args, 0);

    assert(result == destination, "map-into: destinationを破壊的に書き換えて返す");
    assert((cc_car(result) >> 3) == 11, "map-into: 0番目は(+ (elt seq1 0) (elt seq2 0)) = 1+10=11");
    assert((cc_car(cc_cdr(result)) >> 3) == 22, "map-into: 1番目は2+20=22");
    assert((cc_car(cc_cdr(cc_cdr(result))) >> 3) == 33, "map-into: 2番目は3+30=33");
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
    test_transpile_fixture_lambda_capture_value();
    test_transpile_fixture_lambda_box_mutate();
    test_transpile_fixture_make_counter_and_call_twice();
    test_transpile_fixture_defdynamic();
    test_transpile_fixture_dynamic_read();
    test_transpile_fixture_let();
    test_transpile_fixture_let_multi();
    test_transpile_fixture_let_body_progn();
    test_transpile_fixture_let_star();
    test_transpile_fixture_cond();
    test_transpile_fixture_cond_body_progn();
    test_transpile_fixture_case();
    test_transpile_fixture_case_using();
    test_transpile_fixture_setf_setq();
    test_transpile_fixture_setf_car();
    test_transpile_fixture_setf_cdr();
    test_transpile_fixture_setf_aref();
    test_transpile_fixture_setf_elt();
    test_transpile_fixture_setf_slot_value();
    test_transpile_fixture_rest();
    test_transpile_fixture_rest_with_fixed();
    test_list();
    test_append();
    test_create_list();
    test_nreverse();
    test_apply();
    test_mapcar();
    test_mapc();
    test_mapcan();
    test_mapcon();
    test_map_into();
    test_transpile_fixture_for_sum();
    test_transpile_fixture_for_early_exit();
    test_transpile_fixture_while_sum();
    test_transpile_fixture_while_named_block_exit();
    test_transpile_fixture_unwind_protect_normal();
    test_transpile_fixture_unwind_protect_non_local_exit();
    test_transpile_fixture_unwind_protect_cleanup_exit_ignored();
    test_transpile_fixture_with_open_input_stream();
    test_transpile_fixture_with_open_input_stream_early_exit();
    test_transpile_fixture_with_open_input_file();
    test_transpile_fixture_with_open_output_stream_early_exit();
    test_transpile_fixture_with_open_output_file();
    test_transpile_fixture_catch_throw_basic();
    test_transpile_fixture_catch_no_throw();
    test_transpile_fixture_catch_mismatched_tag();
    test_transpile_fixture_catch_throw_runtime_tag();
    test_transpile_fixture_catch_with_cleanup();
    test_transpile_fixture_register_and_find_class();
    test_transpile_fixture_find_class_missing();
    test_transpile_fixture_register_builtin_class_then_find();
    test_transpile_fixture_slot_value_read();
    test_transpile_fixture_fill_slots();
    test_transpile_fixture_subclassp();
    test_transpile_fixture_class_of_builtin();
    test_transpile_fixture_class_of_instance();
    test_transpile_fixture_typep_instancep();
    test_transpile_fixture_generic_dispatch();
    test_transpile_fixture_generic_dispatch_order();
    test_transpile_fixture_call_next_method();
    test_transpile_fixture_generic_no_applicable_method();
    test_transpile_fixture_call_next_method_no_next();
    test_transpile_fixture_make_instance();
    test_transpile_fixture_nested_let_dynamic_restore();
    test_transpile_fixture_signal_condition_like();
    test_transpile_fixture_signal_condition_nonlocal();
    // GC_PROTECT検証はos_reset_runtime_state_for_testでglobal_environment/symbol table等の
    // 状態を再初期化するため、他のテストに影響しないよう最後に実行する
    test_transpile_fixture_gc_protect();
    return g_test_failed;
}
