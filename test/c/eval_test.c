#include <stdlib.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "eval.h"

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

#define HEAP_SIZE (1024 * 1024)

static void setup_heap() {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
}

// テスト用に + と - だけを登録した環境を作る
static lisp_val_t make_arith_env() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    os_set_function(os_make_symbol("+"), os_make_native_function((lisp_addr_t)(void *)primitive_add), env);
    os_set_function(os_make_symbol("-"), os_make_native_function((lisp_addr_t)(void *)primitive_subtract), env);
    return env;
}

static lisp_val_t make_call(const char *op_name, int argc, lisp_val_t *args) {
    lisp_val_t list = nil;
    for (int i = argc - 1; i >= 0; i--) {
        list = os_make_cons(args[i], list);
    }
    return os_make_cons(os_make_symbol(op_name), list);
}

void test_os_eval_fixnum_self_evaluates() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t v = os_eval(os_make_fixnum(42), env);
    assert(v == os_make_fixnum(42), "fixnumは自己評価する");
}

void test_os_eval_string_self_evaluates() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t s = os_make_string("hello");
    lisp_val_t v = os_eval(s, env);
    assert(v == s, "文字列は自己評価する");
}

void test_os_eval_symbol_looks_up_variable() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    os_set_variable(os_make_symbol("x"), os_make_fixnum(10), env);

    lisp_val_t v = os_eval(os_make_symbol("x"), env);
    assert(v == os_make_fixnum(10), "symbolはenvから変数の値をlookupして返す");

    lisp_val_t undefined = os_eval(os_make_symbol("undefined-var"), env);
    assert(undefined == nil, "未定義のsymbolはnilが返る");
}

void test_os_eval_add() {
    lisp_val_t env = make_arith_env();
    lisp_val_t args[3] = { os_make_fixnum(1), os_make_fixnum(2), os_make_fixnum(3) };
    lisp_val_t form = make_call("+", 3, args);

    lisp_val_t v = os_eval(form, env);
    assert(v == os_make_fixnum(6), "(+ 1 2 3)は6と評価される");
}

void test_os_eval_subtract() {
    lisp_val_t env = make_arith_env();

    lisp_val_t args[3] = { os_make_fixnum(10), os_make_fixnum(3), os_make_fixnum(2) };
    lisp_val_t form = make_call("-", 3, args);
    lisp_val_t v = os_eval(form, env);
    assert(v == os_make_fixnum(5), "(- 10 3 2)は5と評価される");
}

void test_os_eval_nested_form() {
    lisp_val_t env = make_arith_env();

    lisp_val_t sub_args[2] = { os_make_fixnum(5), os_make_fixnum(2) };
    lisp_val_t sub_form = make_call("-", 2, sub_args); // (- 5 2) -> 3

    lisp_val_t add_args[2] = { sub_form, os_make_fixnum(3) };
    lisp_val_t add_form = make_call("+", 2, add_args); // (+ (- 5 2) 3) -> 6

    lisp_val_t v = os_eval(add_form, env);
    assert(v == os_make_fixnum(6), "(+ (- 5 2) 3)は再帰的に評価され6になる");
}

void test_os_eval_undefined_function_returns_eval_error() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t args[1] = { os_make_fixnum(1) };
    lisp_val_t form = make_call("undefined-fn", 1, args);

    lisp_val_t v = os_eval(form, env);
    assert(v == g_sym_eval_error, "未定義の関数を呼ぶとeval-errorが返る");
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    setup_heap();

    test_os_eval_fixnum_self_evaluates();
    test_os_eval_string_self_evaluates();
    test_os_eval_symbol_looks_up_variable();
    test_os_eval_add();
    test_os_eval_subtract();
    test_os_eval_nested_form();
    test_os_eval_undefined_function_returns_eval_error();

    return g_test_failed ? 1 : 0;
}
