#include <stdlib.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "eval.h"
#include "lisp.h"

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

void test_os_eval_nil_self_evaluates() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t v = os_eval(nil, env);
    assert(v == nil, "nilは自己評価する");
}

void test_os_eval_quote() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t inner = os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(2), nil));
    lisp_val_t args[1] = { inner };
    lisp_val_t form = make_call("quote", 1, args);

    lisp_val_t v = os_eval(form, env);
    assert(v == inner, "(quote (1 2))は評価されずそのまま(1 2)を返す");
}

void test_os_eval_if() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);

    lisp_val_t true_args[3] = { os_make_fixnum(1), os_make_fixnum(10), os_make_fixnum(20) };
    lisp_val_t v1 = os_eval(make_call("if", 3, true_args), env);
    assert(v1 == os_make_fixnum(10), "(if 1 10 20)はtestが非nilなのでthenの10になる");

    lisp_val_t false_args[3] = { nil, os_make_fixnum(10), os_make_fixnum(20) };
    lisp_val_t v2 = os_eval(make_call("if", 3, false_args), env);
    assert(v2 == os_make_fixnum(20), "(if nil 10 20)はtestがnilなのでelseの20になる");

    lisp_val_t no_else_args[2] = { nil, os_make_fixnum(10) };
    lisp_val_t v3 = os_eval(make_call("if", 2, no_else_args), env);
    assert(v3 == nil, "(if nil 10)はelseが省略されているのでnilになる");
}

void test_os_eval_progn() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);

    lisp_val_t args[3] = { os_make_fixnum(1), os_make_fixnum(2), os_make_fixnum(3) };
    lisp_val_t v = os_eval(make_call("progn", 3, args), env);
    assert(v == os_make_fixnum(3), "(progn 1 2 3)は最後の評価結果である3になる");

    lisp_val_t empty = os_eval(make_call("progn", 0, NULL), env);
    assert(empty == nil, "(progn)はnilになる");
}

void test_os_eval_setq_writes_current_env_only() {
    lisp_val_t base_env = os_make_environment(os_make_symbol("BASE-ENV"), nil);
    lisp_val_t current_env = os_make_environment(os_make_symbol("CURRENT-ENV"), base_env);
    os_set_variable(os_make_symbol("x"), os_make_fixnum(1), base_env);

    lisp_val_t args[2] = { os_make_symbol("x"), os_make_fixnum(99) };
    lisp_val_t v = os_eval(make_call("setq", 2, args), current_env);
    assert(v == os_make_fixnum(99), "(setq x 99)はセットした値99を返す");

    lisp_val_t current_x = os_get_variable(os_make_symbol("x"), current_env);
    assert(current_x == os_make_fixnum(99), "current_envのxは99に上書きされる");

    lisp_val_t base_x = os_get_variable(os_make_symbol("x"), base_env);
    assert(base_x == os_make_fixnum(1), "base_env自身のxは書き換わらない");
}

void test_os_eval_defun() {
    lisp_val_t env = make_arith_env();

    // (defun add1 (x) (+ x 1))
    lisp_val_t name = os_make_symbol("add1");
    lisp_val_t params = os_make_cons(os_make_symbol("x"), nil);
    lisp_val_t plus_args[2] = { os_make_symbol("x"), os_make_fixnum(1) };
    lisp_val_t body = os_make_cons(make_call("+", 2, plus_args), nil);
    lisp_val_t defun_form = os_make_cons(os_make_symbol("defun"),
                                os_make_cons(name, os_make_cons(params, body)));

    lisp_val_t defined = os_eval(defun_form, env);
    assert(defined == name, "defunの戻り値は関数名add1");

    lisp_val_t call_args[1] = { os_make_fixnum(5) };
    lisp_val_t v = os_eval(make_call("add1", 1, call_args), env);
    assert(v == os_make_fixnum(6), "(add1 5)は(+ x 1)が評価され6になる");
}

void test_os_eval_defun_multi_form_body() {
    lisp_val_t env = make_arith_env();

    // (defun foo (x) (setq x (+ x 1)) x) : bodyは複数式でprognとして評価される
    lisp_val_t name = os_make_symbol("foo");
    lisp_val_t params = os_make_cons(os_make_symbol("x"), nil);
    lisp_val_t plus_args[2] = { os_make_symbol("x"), os_make_fixnum(1) };
    lisp_val_t setq_args[2] = { os_make_symbol("x"), make_call("+", 2, plus_args) };
    lisp_val_t form1 = make_call("setq", 2, setq_args);
    lisp_val_t form2 = os_make_symbol("x");
    lisp_val_t body = os_make_cons(form1, os_make_cons(form2, nil));
    lisp_val_t defun_form = os_make_cons(os_make_symbol("defun"),
                                os_make_cons(name, os_make_cons(params, body)));

    os_eval(defun_form, env);

    lisp_val_t call_args[1] = { os_make_fixnum(10) };
    lisp_val_t v = os_eval(make_call("foo", 1, call_args), env);
    assert(v == os_make_fixnum(11), "複数式のbodyはprognとして評価され最後の式(x=11)が返る");
}

void test_os_eval_defun_recursion() {
    lisp_val_t env = os_make_environment(os_make_symbol("REC-ENV"), nil);
    os_set_function(os_make_symbol("+"), os_make_native_function((lisp_addr_t)(void *)primitive_add), env);
    os_set_function(os_make_symbol("-"), os_make_native_function((lisp_addr_t)(void *)primitive_subtract), env);
    os_set_function(os_make_symbol("eq"), os_make_native_function((lisp_addr_t)(void *)primitive_eq), env);

    // (defun sum-to (n) (if (eq n 0) 0 (+ n (sum-to (- n 1)))))
    lisp_val_t name = os_make_symbol("sum-to");
    lisp_val_t params = os_make_cons(os_make_symbol("n"), nil);

    lisp_val_t eq_args[2] = { os_make_symbol("n"), os_make_fixnum(0) };
    lisp_val_t test_form = make_call("eq", 2, eq_args);

    lisp_val_t sub_args[2] = { os_make_symbol("n"), os_make_fixnum(1) };
    lisp_val_t sub_form = make_call("-", 2, sub_args);
    lisp_val_t rec_args[1] = { sub_form };
    lisp_val_t rec_call = make_call("sum-to", 1, rec_args);
    lisp_val_t add_args[2] = { os_make_symbol("n"), rec_call };
    lisp_val_t else_form = make_call("+", 2, add_args);

    lisp_val_t if_args[3] = { test_form, os_make_fixnum(0), else_form };
    lisp_val_t if_form = make_call("if", 3, if_args);

    lisp_val_t body = os_make_cons(if_form, nil);
    lisp_val_t defun_form = os_make_cons(os_make_symbol("defun"),
                                os_make_cons(name, os_make_cons(params, body)));
    os_eval(defun_form, env);

    lisp_val_t call_args[1] = { os_make_fixnum(3) };
    lisp_val_t v = os_eval(make_call("sum-to", 1, call_args), env);
    assert(v == os_make_fixnum(6), "(sum-to 3)は再帰呼び出しで3+2+1+0=6になる");
}

void test_os_eval_lambda_immediate_invocation() {
    lisp_val_t env = make_arith_env();

    // ((lambda (x) (+ x 1)) 5)
    lisp_val_t params = os_make_cons(os_make_symbol("x"), nil);
    lisp_val_t plus_args[2] = { os_make_symbol("x"), os_make_fixnum(1) };
    lisp_val_t body = os_make_cons(make_call("+", 2, plus_args), nil);
    lisp_val_t lambda_form = os_make_cons(os_make_symbol("lambda"), os_make_cons(params, body));

    lisp_val_t call_form = os_make_cons(lambda_form, os_make_cons(os_make_fixnum(5), nil));
    lisp_val_t v = os_eval(call_form, env);
    assert(v == os_make_fixnum(6), "((lambda (x) (+ x 1)) 5)は即時に呼び出され6になる");
}

void test_os_eval_lambda_closes_over_defining_env() {
    lisp_val_t outer_env = os_make_environment(os_make_symbol("OUTER-ENV"), nil);
    os_set_variable(os_make_symbol("y"), os_make_fixnum(100), outer_env);
    os_set_function(os_make_symbol("+"), os_make_native_function((lisp_addr_t)(void *)primitive_add), outer_env);

    // outer_envで (lambda (x) (+ x y)) を作る
    lisp_val_t params = os_make_cons(os_make_symbol("x"), nil);
    lisp_val_t plus_args[2] = { os_make_symbol("x"), os_make_symbol("y") };
    lisp_val_t body = os_make_cons(make_call("+", 2, plus_args), nil);
    lisp_val_t lambda_form = os_make_cons(os_make_symbol("lambda"), os_make_cons(params, body));
    lisp_val_t fn = os_eval(lambda_form, outer_env);

    // yの束縛が無い別環境からfnを(直接オブジェクトとして)呼び出してもouter_envのyが見える
    lisp_val_t caller_env = os_make_environment(os_make_symbol("CALLER-ENV"), nil);
    os_set_variable(os_make_symbol("fn"), fn, caller_env);
    os_set_function(os_make_symbol("funcall-fn"), fn, caller_env);

    lisp_val_t v = os_eval(make_call("funcall-fn", 1, (lisp_val_t[]){ os_make_fixnum(5) }), caller_env);
    assert(v == os_make_fixnum(105), "lambdaは定義時のenv(outer_env)をクロージャとして保持し、y=100が見える");
}

void test_os_eval_defun_rest_param_collects_remaining_args() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);

    // (defun my-list (&rest args) args)
    lisp_val_t name = os_make_symbol("my-list");
    lisp_val_t params = os_make_cons(g_sym_rest, os_make_cons(os_make_symbol("args"), nil));
    lisp_val_t body = os_make_cons(os_make_symbol("args"), nil);
    lisp_val_t defun_form = os_make_cons(os_make_symbol("defun"),
                                os_make_cons(name, os_make_cons(params, body)));
    os_eval(defun_form, env);

    lisp_val_t call_args[3] = { os_make_fixnum(1), os_make_fixnum(2), os_make_fixnum(3) };
    lisp_val_t v = os_eval(make_call("my-list", 3, call_args), env);
    assert(cc_car(v) == os_make_fixnum(1), "(my-list 1 2 3)のargsの1番目は1");
    assert(cc_car(cc_cdr(v)) == os_make_fixnum(2), "(my-list 1 2 3)のargsの2番目は2");
    assert(cc_car(cc_cdr(cc_cdr(v))) == os_make_fixnum(3), "(my-list 1 2 3)のargsの3番目は3");
    assert(cc_cdr(cc_cdr(cc_cdr(v))) == nil, "(my-list 1 2 3)のargsの末尾はnil");

    lisp_val_t empty = os_eval(make_call("my-list", 0, NULL), env);
    assert(empty == nil, "(my-list)は実引数が無いのでargsはnil");
}

void test_os_eval_defun_rest_param_with_leading_fixed_params() {
    lisp_val_t env = make_arith_env();

    // (defun my-add-all (first &rest rest) (+ first (apply-sum rest))) の代わりに
    // firstとrestそれぞれが正しく束縛されることだけを直接確認する:
    // (defun first-and-rest (first &rest rest) (cons first rest))
    lisp_val_t name = os_make_symbol("first-and-rest");
    lisp_val_t params = os_make_cons(os_make_symbol("first"),
                            os_make_cons(g_sym_rest, os_make_cons(os_make_symbol("rest"), nil)));
    lisp_val_t body = os_make_cons(
                            make_call("cons", 2, (lisp_val_t[]){ os_make_symbol("first"), os_make_symbol("rest") }),
                            nil);
    lisp_val_t defun_form = os_make_cons(os_make_symbol("defun"),
                                os_make_cons(name, os_make_cons(params, body)));
    os_set_function(os_make_symbol("cons"), os_make_native_function((lisp_addr_t)(void *)primitive_cons), env);
    os_eval(defun_form, env);

    lisp_val_t call_args[3] = { os_make_fixnum(1), os_make_fixnum(2), os_make_fixnum(3) };
    lisp_val_t v = os_eval(make_call("first-and-rest", 3, call_args), env);
    assert(cc_car(v) == os_make_fixnum(1), "(first-and-rest 1 2 3)のfirstは1");
    assert(cc_car(cc_cdr(v)) == os_make_fixnum(2), "(first-and-rest 1 2 3)のrestの1番目は2");
    assert(cc_car(cc_cdr(cc_cdr(v))) == os_make_fixnum(3), "(first-and-rest 1 2 3)のrestの2番目は3");
}

void test_os_eval_defmacro_expands_and_evaluates() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);

    // (defmacro my-if (test then else) `(if ,test ,then ,else))
    lisp_val_t name = os_make_symbol("my-if");
    lisp_val_t params = os_make_cons(os_make_symbol("test"),
                            os_make_cons(os_make_symbol("then"),
                                os_make_cons(os_make_symbol("else"), nil)));

    lisp_val_t unquote_test = os_make_cons(g_sym_unquote, os_make_cons(os_make_symbol("test"), nil));
    lisp_val_t unquote_then = os_make_cons(g_sym_unquote, os_make_cons(os_make_symbol("then"), nil));
    lisp_val_t unquote_else = os_make_cons(g_sym_unquote, os_make_cons(os_make_symbol("else"), nil));
    lisp_val_t if_template = os_make_cons(os_make_symbol("if"),
                                os_make_cons(unquote_test,
                                    os_make_cons(unquote_then, os_make_cons(unquote_else, nil))));
    lisp_val_t body = os_make_cons(
                            os_make_cons(g_sym_quasiquote, os_make_cons(if_template, nil)), nil);

    lisp_val_t defmacro_form = os_make_cons(os_make_symbol("defmacro"),
                                    os_make_cons(name, os_make_cons(params, body)));

    lisp_val_t defined = os_eval(defmacro_form, env);
    assert(defined == name, "defmacroの戻り値はマクロ名my-if");

    // (my-if 1 10 20) はマクロ展開後 (if 1 10 20) として評価され10になる
    lisp_val_t call_args[3] = { os_make_fixnum(1), os_make_fixnum(10), os_make_fixnum(20) };
    lisp_val_t v = os_eval(make_call("my-if", 3, call_args), env);
    assert(v == os_make_fixnum(10), "(my-if 1 10 20)はマクロ展開され(if 1 10 20)として評価され10になる");
}

void test_os_eval_defmacro_args_are_not_evaluated_before_expansion() {
    lisp_val_t env = make_arith_env();

    // (defmacro my-quote (x) `(quote ,x))
    lisp_val_t name = os_make_symbol("my-quote");
    lisp_val_t params = os_make_cons(os_make_symbol("x"), nil);
    lisp_val_t unquote_x = os_make_cons(g_sym_unquote, os_make_cons(os_make_symbol("x"), nil));
    lisp_val_t quote_template = os_make_cons(os_make_symbol("quote"), os_make_cons(unquote_x, nil));
    lisp_val_t body = os_make_cons(
                            os_make_cons(g_sym_quasiquote, os_make_cons(quote_template, nil)), nil);
    lisp_val_t defmacro_form = os_make_cons(os_make_symbol("defmacro"),
                                    os_make_cons(name, os_make_cons(params, body)));
    os_eval(defmacro_form, env);

    // (my-quote (+ 1 2)) : マクロの引数(+ 1 2)は展開前に評価されず、
    // 展開結果(quote (+ 1 2))を評価してリスト(+ 1 2)がそのまま返る
    lisp_val_t unevaluated_arg = make_call("+", 2, (lisp_val_t[]){ os_make_fixnum(1), os_make_fixnum(2) });
    lisp_val_t call_args[1] = { unevaluated_arg };
    lisp_val_t v = os_eval(make_call("my-quote", 1, call_args), env);

    assert((v & TAG_MASK) == TAG_CONS, "(my-quote (+ 1 2))の結果はconsのまま(評価されていない)");
    assert(cc_car(v) == os_make_symbol("+"), "マクロ引数は展開前に評価されないので先頭はsymbol +のまま");
}

void test_primitive_macroexpand_1_expands_macro_call() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);

    // (defmacro my-if (test then else) `(if ,test ,then ,else))
    lisp_val_t name = os_make_symbol("my-if");
    lisp_val_t params = os_make_cons(os_make_symbol("test"),
                            os_make_cons(os_make_symbol("then"),
                                os_make_cons(os_make_symbol("else"), nil)));
    lisp_val_t unquote_test = os_make_cons(g_sym_unquote, os_make_cons(os_make_symbol("test"), nil));
    lisp_val_t unquote_then = os_make_cons(g_sym_unquote, os_make_cons(os_make_symbol("then"), nil));
    lisp_val_t unquote_else = os_make_cons(g_sym_unquote, os_make_cons(os_make_symbol("else"), nil));
    lisp_val_t if_template = os_make_cons(os_make_symbol("if"),
                                os_make_cons(unquote_test,
                                    os_make_cons(unquote_then, os_make_cons(unquote_else, nil))));
    lisp_val_t body = os_make_cons(
                            os_make_cons(g_sym_quasiquote, os_make_cons(if_template, nil)), nil);
    lisp_val_t defmacro_form = os_make_cons(os_make_symbol("defmacro"),
                                    os_make_cons(name, os_make_cons(params, body)));
    os_eval(defmacro_form, env);

    // (macroexpand-1 '(my-if 1 10 20)) は展開結果(if 1 10 20)を1段だけ返す(評価はしない)
    lisp_val_t call_args[3] = { os_make_fixnum(1), os_make_fixnum(10), os_make_fixnum(20) };
    lisp_val_t call_form = make_call("my-if", 3, call_args);
    lisp_val_t expanded = primitive_macroexpand_1(os_make_cons(call_form, nil), env);

    assert(cc_car(expanded) == os_make_symbol("if"), "(macroexpand-1 '(my-if 1 10 20))の先頭はsymbol if");
    assert(cc_car(cc_cdr(expanded)) == os_make_fixnum(1), "展開結果の2番目は1");
    assert(cc_car(cc_cdr(cc_cdr(expanded))) == os_make_fixnum(10), "展開結果の3番目は10");
    assert(cc_car(cc_cdr(cc_cdr(cc_cdr(expanded)))) == os_make_fixnum(20), "展開結果の4番目は20");
}

void test_primitive_macroexpand_1_returns_form_unchanged_when_not_macro() {
    lisp_val_t env = make_arith_env();

    // マクロではない通常の関数呼び出しはそのまま返る(評価はしない)
    lisp_val_t form = make_call("+", 2, (lisp_val_t[]){ os_make_fixnum(1), os_make_fixnum(2) });
    lisp_val_t expanded = primitive_macroexpand_1(os_make_cons(form, nil), env);
    assert(expanded == form, "マクロでないformはmacroexpand-1で変化せずそのまま返る");

    lisp_val_t atom = os_make_fixnum(42);
    lisp_val_t expanded_atom = primitive_macroexpand_1(os_make_cons(atom, nil), env);
    assert(expanded_atom == atom, "atomはmacroexpand-1でそのまま返る");
}

void test_primitive_funcall_calls_native_function() {
    lisp_val_t env = make_arith_env();
    lisp_val_t plus_fn = os_get_function(os_make_symbol("+"), env);
    lisp_val_t args = os_make_cons(plus_fn,
                          os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(2), nil)));
    lisp_val_t r = primitive_funcall(args, env);
    assert(r == os_make_fixnum(3), "(funcall #'+ 1 2)は3");
}

void test_primitive_funcall_calls_interpreted_function() {
    lisp_val_t env = make_arith_env();
    lisp_val_t lambda_params = os_make_cons(os_make_symbol("x"), os_make_cons(os_make_symbol("y"), nil));
    lisp_val_t lambda_body = make_call("+", 2, (lisp_val_t[]){ os_make_symbol("x"), os_make_symbol("y") });
    lisp_val_t lambda_form = make_call("lambda", 2, (lisp_val_t[]){ lambda_params, lambda_body });
    lisp_val_t fn = os_eval(lambda_form, env);

    lisp_val_t args = os_make_cons(fn,
                          os_make_cons(os_make_fixnum(3), os_make_cons(os_make_fixnum(4), nil)));
    lisp_val_t r = primitive_funcall(args, env);
    assert(r == os_make_fixnum(7), "(funcall (lambda (x y) (+ x y)) 3 4)は7");
}

void test_os_eval_quasiquote_plain_list() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);

    // `(1 2 3) : unquoteが無いのでquoteと同様、そのまま(1 2 3)を返す
    lisp_val_t inner = os_make_cons(os_make_fixnum(1),
                            os_make_cons(os_make_fixnum(2), os_make_cons(os_make_fixnum(3), nil)));
    lisp_val_t form = os_make_cons(g_sym_quasiquote, os_make_cons(inner, nil));

    lisp_val_t v = os_eval(form, env);
    assert(cc_car(v) == os_make_fixnum(1), "`(1 2 3)の1番目は1");
    assert(cc_car(cc_cdr(v)) == os_make_fixnum(2), "`(1 2 3)の2番目は2");
    assert(cc_car(cc_cdr(cc_cdr(v))) == os_make_fixnum(3), "`(1 2 3)の3番目は3");
    assert(cc_cdr(cc_cdr(cc_cdr(v))) == nil, "`(1 2 3)の末尾はnil");
}

void test_os_eval_quasiquote_unquote() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    os_set_variable(os_make_symbol("x"), os_make_fixnum(10), env);

    // `(a ,x c) : xの位置だけ評価された10に置き換わる
    lisp_val_t unquote_x = os_make_cons(g_sym_unquote, os_make_cons(os_make_symbol("x"), nil));
    lisp_val_t inner = os_make_cons(os_make_symbol("a"),
                            os_make_cons(unquote_x, os_make_cons(os_make_symbol("c"), nil)));
    lisp_val_t form = os_make_cons(g_sym_quasiquote, os_make_cons(inner, nil));

    lisp_val_t v = os_eval(form, env);
    assert(cc_car(v) == os_make_symbol("a"), "`(a ,x c)の1番目はsymbol a(未評価)");
    assert(cc_car(cc_cdr(v)) == os_make_fixnum(10), "`(a ,x c)の2番目はxが評価された10");
    assert(cc_car(cc_cdr(cc_cdr(v))) == os_make_symbol("c"), "`(a ,x c)の3番目はsymbol c(未評価)");
}

void test_os_eval_quasiquote_unquote_splicing() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t lst = os_make_cons(os_make_fixnum(1),
                          os_make_cons(os_make_fixnum(2), os_make_cons(os_make_fixnum(3), nil)));
    os_set_variable(os_make_symbol("lst"), lst, env);

    // `(a ,@lst c) : lstの評価結果(1 2 3)がその場に展開(splice)される
    lisp_val_t splicing_lst = os_make_cons(g_sym_unquote_splicing, os_make_cons(os_make_symbol("lst"), nil));
    lisp_val_t inner = os_make_cons(os_make_symbol("a"),
                            os_make_cons(splicing_lst, os_make_cons(os_make_symbol("c"), nil)));
    lisp_val_t form = os_make_cons(g_sym_quasiquote, os_make_cons(inner, nil));

    lisp_val_t v = os_eval(form, env);
    assert(cc_car(v) == os_make_symbol("a"), "`(a ,@lst c)の1番目はsymbol a");
    assert(cc_car(cc_cdr(v)) == os_make_fixnum(1), "`(a ,@lst c)の2番目はlstの1番目(1)");
    assert(cc_car(cc_cdr(cc_cdr(v))) == os_make_fixnum(2), "`(a ,@lst c)の3番目はlstの2番目(2)");
    assert(cc_car(cc_cdr(cc_cdr(cc_cdr(v)))) == os_make_fixnum(3), "`(a ,@lst c)の4番目はlstの3番目(3)");
    assert(cc_car(cc_cdr(cc_cdr(cc_cdr(cc_cdr(v))))) == os_make_symbol("c"), "`(a ,@lst c)の5番目はsymbol c");
    assert(cc_cdr(cc_cdr(cc_cdr(cc_cdr(cc_cdr(v))))) == nil, "`(a ,@lst c)の末尾はnil");
}

void test_os_eval_cons() {
    lisp_val_t args[2] = { os_make_fixnum(1), os_make_fixnum(2) };
    lisp_val_t v = os_eval(make_call("cons", 2, args), global_environment);
    assert((v & TAG_MASK) == TAG_CONS, "(cons 1 2)はTAG_CONSを持つ");
    assert(cc_car(v) == os_make_fixnum(1), "(cons 1 2)のcarは1");
    assert(cc_cdr(v) == os_make_fixnum(2), "(cons 1 2)のcdrは2");
}

void test_os_eval_eq() {
    lisp_val_t same_args[2] = { os_make_fixnum(1), os_make_fixnum(1) };
    lisp_val_t v1 = os_eval(make_call("eq", 2, same_args), global_environment);
    assert(v1 == g_sym_t, "(eq 1 1)はTになる");

    lisp_val_t diff_args[2] = { os_make_fixnum(1), os_make_fixnum(2) };
    lisp_val_t v2 = os_eval(make_call("eq", 2, diff_args), global_environment);
    assert(v2 == nil, "(eq 1 2)はnilになる");
}

void test_os_eval_null() {
    lisp_val_t nil_args[1] = { nil };
    lisp_val_t v1 = os_eval(make_call("null", 1, nil_args), global_environment);
    assert(v1 == g_sym_t, "(null nil)はTになる");

    lisp_val_t non_nil_args[1] = { os_make_fixnum(1) };
    lisp_val_t v2 = os_eval(make_call("null", 1, non_nil_args), global_environment);
    assert(v2 == nil, "(null 1)はnilになる");
}

void test_os_eval_block_return_from_immediate_exit() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t name = os_make_symbol("done");

    // (block done 1 (return-from done 2) 3) : return-fromで即脱出し、3は評価されず結果は2
    lisp_val_t return_form = os_make_cons(os_make_symbol("return-from"),
                                  os_make_cons(name, os_make_cons(os_make_fixnum(2), nil)));
    lisp_val_t body = os_make_cons(os_make_fixnum(1),
                            os_make_cons(return_form, os_make_cons(os_make_fixnum(3), nil)));
    lisp_val_t form = os_make_cons(os_make_symbol("block"), os_make_cons(name, body));

    lisp_val_t v = os_eval(form, env);
    assert(v == os_make_fixnum(2), "(block done 1 (return-from done 2) 3)はreturn-fromで即脱出し2になる");
}

void test_os_eval_return_from_crosses_function_call() {
    lisp_val_t env = make_arith_env();
    lisp_val_t outer = os_make_symbol("outer");

    // (defun inner () (return-from outer 42))
    lisp_val_t return_form = os_make_cons(os_make_symbol("return-from"),
                                  os_make_cons(outer, os_make_cons(os_make_fixnum(42), nil)));
    lisp_val_t inner_body = os_make_cons(return_form, nil);
    lisp_val_t defun_form = os_make_cons(os_make_symbol("defun"),
                                os_make_cons(os_make_symbol("inner"), os_make_cons(nil, inner_body)));
    os_eval(defun_form, env);

    // (block outer (inner) 99) : innerの中のreturn-fromがouterまで関数呼び出しを飛び越えて届く
    lisp_val_t block_body = os_make_cons(make_call("inner", 0, NULL), os_make_cons(os_make_fixnum(99), nil));
    lisp_val_t block_form = os_make_cons(os_make_symbol("block"), os_make_cons(outer, block_body));

    lisp_val_t v = os_eval(block_form, env);
    assert(v == os_make_fixnum(42), "innerで呼んだreturn-fromが関数呼び出しをまたいでouterまで届き42になる");
}

void test_os_eval_nested_block_return_from_only_exits_inner() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t inner_name = os_make_symbol("inner");
    lisp_val_t outer_name = os_make_symbol("outer2");

    // (block outer2 (block inner (return-from inner 1) 2) 3)
    lisp_val_t return_form = os_make_cons(os_make_symbol("return-from"),
                                  os_make_cons(inner_name, os_make_cons(os_make_fixnum(1), nil)));
    lisp_val_t inner_body = os_make_cons(return_form, os_make_cons(os_make_fixnum(2), nil));
    lisp_val_t inner_block = os_make_cons(os_make_symbol("block"), os_make_cons(inner_name, inner_body));

    lisp_val_t outer_body = os_make_cons(inner_block, os_make_cons(os_make_fixnum(3), nil));
    lisp_val_t outer_block = os_make_cons(os_make_symbol("block"), os_make_cons(outer_name, outer_body));

    lisp_val_t v = os_eval(outer_block, env);
    assert(v == os_make_fixnum(3), "innerへのreturn-fromはinnerだけを抜け、outerは残りの3まで評価される");
}

void test_os_eval_unwind_protect_runs_cleanup_on_normal_return() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t flag = os_make_symbol("flag");
    os_set_variable(flag, os_make_fixnum(0), env);

    // (unwind-protect 1 (setq flag 99))
    lisp_val_t setq_flag = os_make_cons(os_make_symbol("setq"),
                                os_make_cons(flag, os_make_cons(os_make_fixnum(99), nil)));
    lisp_val_t form = os_make_cons(os_make_symbol("unwind-protect"),
                            os_make_cons(os_make_fixnum(1), os_make_cons(setq_flag, nil)));

    lisp_val_t v = os_eval(form, env);
    assert(v == os_make_fixnum(1), "(unwind-protect 1 (setq flag 99))は保護対象の結果1を返す");
    assert(os_get_variable(flag, env) == os_make_fixnum(99), "cleanupのsetqは通常時も実行される");
}

void test_os_eval_unwind_protect_runs_cleanup_on_non_local_exit() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t flag = os_make_symbol("flag");
    os_set_variable(flag, os_make_fixnum(0), env);
    lisp_val_t done = os_make_symbol("done");

    // (block done (unwind-protect (return-from done 1) (setq flag 100)) 2)
    lisp_val_t return_form = os_make_cons(os_make_symbol("return-from"),
                                  os_make_cons(done, os_make_cons(os_make_fixnum(1), nil)));
    lisp_val_t setq_flag = os_make_cons(os_make_symbol("setq"),
                                os_make_cons(flag, os_make_cons(os_make_fixnum(100), nil)));
    lisp_val_t unwind_form = os_make_cons(os_make_symbol("unwind-protect"),
                                os_make_cons(return_form, os_make_cons(setq_flag, nil)));
    lisp_val_t block_body = os_make_cons(unwind_form, os_make_cons(os_make_fixnum(2), nil));
    lisp_val_t block_form = os_make_cons(os_make_symbol("block"), os_make_cons(done, block_body));

    lisp_val_t v = os_eval(block_form, env);
    assert(v == os_make_fixnum(1), "return-fromでの脱出時もunwind-protectの結果はblockまで正しく伝播し1になる");
    assert(os_get_variable(flag, env) == os_make_fixnum(100), "return-fromで脱出する際もcleanupは必ず実行される");
}

void test_os_eval_catch_throw_basic() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t quoted_tag = os_make_cons(g_sym_quote, os_make_cons(os_make_symbol("tag"), nil));

    // (catch 'tag (throw 'tag 42)) : throwで渡した42をcatchが捕捉して返す
    lisp_val_t throw_form = os_make_cons(os_make_symbol("throw"),
                                os_make_cons(quoted_tag, os_make_cons(os_make_fixnum(42), nil)));
    lisp_val_t catch_form = os_make_cons(os_make_symbol("catch"),
                                os_make_cons(quoted_tag, os_make_cons(throw_form, nil)));

    lisp_val_t v = os_eval(catch_form, env);
    assert(v == os_make_fixnum(42), "(catch 'tag (throw 'tag 42))はthrowの42を捕捉して返す");
}

void test_os_eval_catch_throw_mismatched_tag_propagates_to_outer_catch() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t quoted_outer = os_make_cons(g_sym_quote, os_make_cons(os_make_symbol("outer"), nil));
    lisp_val_t quoted_inner = os_make_cons(g_sym_quote, os_make_cons(os_make_symbol("inner"), nil));

    // (catch 'outer (catch 'inner (throw 'outer 99))) :
    // innerのタグと不一致のthrowはinnerを素通りしてouterまで伝播し99になる
    lisp_val_t throw_outer = os_make_cons(os_make_symbol("throw"),
                                  os_make_cons(quoted_outer, os_make_cons(os_make_fixnum(99), nil)));
    lisp_val_t inner_catch = os_make_cons(os_make_symbol("catch"),
                                  os_make_cons(quoted_inner, os_make_cons(throw_outer, nil)));
    lisp_val_t outer_catch = os_make_cons(os_make_symbol("catch"),
                                  os_make_cons(quoted_outer, os_make_cons(inner_catch, nil)));

    lisp_val_t v = os_eval(outer_catch, env);
    assert(v == os_make_fixnum(99), "タグが一致しないthrowはinner catchを素通りしouter catchが99を捕捉する");
}

void test_os_eval_tagbody_go_forward_jump_skips_form() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t flag = os_make_symbol("flag");
    os_set_variable(flag, os_make_fixnum(0), env);

    // (tagbody (go skip) (setq flag 99) skip) : goで先へ飛び、setqは実行されずflagは0のまま
    lisp_val_t skip_tag = os_make_symbol("skip");
    lisp_val_t go_skip = os_make_cons(os_make_symbol("go"), os_make_cons(skip_tag, nil));
    lisp_val_t setq_flag = os_make_cons(os_make_symbol("setq"),
                                os_make_cons(flag, os_make_cons(os_make_fixnum(99), nil)));
    lisp_val_t body = os_make_cons(go_skip, os_make_cons(setq_flag, os_make_cons(skip_tag, nil)));
    lisp_val_t form = os_make_cons(os_make_symbol("tagbody"), body);

    lisp_val_t v = os_eval(form, env);
    assert(v == nil, "tagbodyは常にnilを返す");
    assert(os_get_variable(flag, env) == os_make_fixnum(0), "goで前方ジャンプしたためsetqは実行されずflagは0のまま");
}

void test_os_eval_tagbody_go_backward_jump_loops_until_end_tag() {
    lisp_val_t env = make_arith_env();
    os_set_function(os_make_symbol("eq"), os_make_native_function((lisp_addr_t)(void *)primitive_eq), env);
    lisp_val_t sum = os_make_symbol("sum");
    lisp_val_t i = os_make_symbol("i");
    os_set_variable(sum, os_make_fixnum(0), env);
    os_set_variable(i, os_make_fixnum(1), env);

    // (tagbody
    //   loop
    //   (if (eq i 6) (go end) nil)
    //   (setq sum (+ sum i))
    //   (setq i (+ i 1))
    //   (go loop)
    //   end)
    // : i=1..5の間backward-goでloopし、sum=1+2+3+4+5=15になる。
    // endはbody末尾のタグ(直後に何も無い)で、goで到達したときに正常終了することも確認する。
    lisp_val_t loop_tag = os_make_symbol("loop");
    lisp_val_t end_tag = os_make_symbol("end");

    lisp_val_t eq_test = make_call("eq", 2, (lisp_val_t[]){ i, os_make_fixnum(6) });
    lisp_val_t go_end = os_make_cons(os_make_symbol("go"), os_make_cons(end_tag, nil));
    lisp_val_t if_form = make_call("if", 3, (lisp_val_t[]){ eq_test, go_end, nil });

    lisp_val_t setq_sum = os_make_cons(os_make_symbol("setq"),
                              os_make_cons(sum, os_make_cons(make_call("+", 2, (lisp_val_t[]){ sum, i }), nil)));
    lisp_val_t setq_i = os_make_cons(os_make_symbol("setq"),
                              os_make_cons(i, os_make_cons(make_call("+", 2, (lisp_val_t[]){ i, os_make_fixnum(1) }), nil)));
    lisp_val_t go_loop = os_make_cons(os_make_symbol("go"), os_make_cons(loop_tag, nil));

    lisp_val_t body = os_make_cons(loop_tag,
                          os_make_cons(if_form,
                              os_make_cons(setq_sum,
                                  os_make_cons(setq_i,
                                      os_make_cons(go_loop,
                                          os_make_cons(end_tag, nil))))));
    lisp_val_t form = os_make_cons(os_make_symbol("tagbody"), body);

    lisp_val_t v = os_eval(form, env);
    assert(v == nil, "tagbodyは常にnilを返す");
    assert(os_get_variable(sum, env) == os_make_fixnum(15), "backward-goによるループでsumは1+2+3+4+5=15になる");
}

void test_os_eval_function_on_symbol_returns_function_object() {
    lisp_val_t env = make_arith_env();

    // (function +) は環境中の関数+のオブジェクトそのものを返す
    lisp_val_t plus_fn = os_get_function(os_make_symbol("+"), env);
    lisp_val_t form = os_make_cons(os_make_symbol("function"), os_make_cons(os_make_symbol("+"), nil));
    lisp_val_t v = os_eval(form, env);
    assert(v == plus_fn, "(function +)は+の関数オブジェクトと一致する");
}

void test_os_eval_function_on_lambda_returns_callable_closure() {
    lisp_val_t env = make_arith_env();

    // (function (lambda (x) (+ x 1))) はクロージャとして呼び出せる
    lisp_val_t params = os_make_cons(os_make_symbol("x"), nil);
    lisp_val_t plus_args[2] = { os_make_symbol("x"), os_make_fixnum(1) };
    lisp_val_t body = os_make_cons(make_call("+", 2, plus_args), nil);
    lisp_val_t lambda_form = os_make_cons(os_make_symbol("lambda"), os_make_cons(params, body));
    lisp_val_t form = os_make_cons(os_make_symbol("function"), os_make_cons(lambda_form, nil));

    lisp_val_t fn = os_eval(form, env);

    lisp_val_t caller_env = os_make_environment(os_make_symbol("CALLER-ENV"), nil);
    os_set_function(os_make_symbol("f"), fn, caller_env);
    lisp_val_t v = os_eval(make_call("f", 1, (lisp_val_t[]){ os_make_fixnum(5) }), caller_env);
    assert(v == os_make_fixnum(6), "(function (lambda (x) (+ x 1)))は呼び出せるクロージャになり6を返す");
}

void test_os_eval_function_on_undefined_symbol_returns_eval_error() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t form = os_make_cons(os_make_symbol("function"), os_make_cons(os_make_symbol("undefined-fn"), nil));
    lisp_val_t v = os_eval(form, env);
    assert(v == g_sym_eval_error, "(function undefined-fn)は未定義なのでeval-errorになる");
}

static lisp_val_t make_flet_binding(const char *name, lisp_val_t params, lisp_val_t body) {
    return os_make_cons(os_make_symbol(name), os_make_cons(params, body));
}

void test_os_eval_flet_siblings_cannot_call_each_other() {
    lisp_val_t env = make_arith_env();

    // (flet ((f (x) (+ x 1)) (g (x) (f x))) (g 5)) : gの本体からfを呼ぼうとするが
    // fletの兄弟関数は互いに見えないため未定義関数エラーになる
    lisp_val_t f_params = os_make_cons(os_make_symbol("x"), nil);
    lisp_val_t f_body = os_make_cons(make_call("+", 2, (lisp_val_t[]){ os_make_symbol("x"), os_make_fixnum(1) }), nil);
    lisp_val_t f_binding = make_flet_binding("f", f_params, f_body);

    lisp_val_t g_params = os_make_cons(os_make_symbol("x"), nil);
    lisp_val_t g_body = os_make_cons(make_call("f", 1, (lisp_val_t[]){ os_make_symbol("x") }), nil);
    lisp_val_t g_binding = make_flet_binding("g", g_params, g_body);

    lisp_val_t bindings = os_make_cons(f_binding, os_make_cons(g_binding, nil));
    lisp_val_t flet_body = os_make_cons(make_call("g", 1, (lisp_val_t[]){ os_make_fixnum(5) }), nil);
    lisp_val_t flet_form = os_make_cons(os_make_symbol("flet"), os_make_cons(bindings, flet_body));

    lisp_val_t v = os_eval(flet_form, env);
    assert(v == g_sym_eval_error, "fletの兄弟関数は互いに見えないのでgからfの呼び出しはeval-errorになる");
}

void test_os_eval_flet_outer_scope_still_visible() {
    lisp_val_t env = make_arith_env();
    os_set_variable(os_make_symbol("y"), os_make_fixnum(100), env);

    // (flet ((f (x) (+ x y))) (f 1)) : fletの本体(f)から外側の変数yと関数+が見える
    lisp_val_t f_params = os_make_cons(os_make_symbol("x"), nil);
    lisp_val_t f_body = os_make_cons(make_call("+", 2, (lisp_val_t[]){ os_make_symbol("x"), os_make_symbol("y") }), nil);
    lisp_val_t f_binding = make_flet_binding("f", f_params, f_body);
    lisp_val_t bindings = os_make_cons(f_binding, nil);
    lisp_val_t flet_body = os_make_cons(make_call("f", 1, (lisp_val_t[]){ os_make_fixnum(1) }), nil);
    lisp_val_t flet_form = os_make_cons(os_make_symbol("flet"), os_make_cons(bindings, flet_body));

    lisp_val_t v = os_eval(flet_form, env);
    assert(v == os_make_fixnum(101), "fletのクロージャは外側の変数yを見えるので1+100=101になる");
}

void test_os_eval_flet_body_can_call_both_bound_functions() {
    lisp_val_t env = make_arith_env();

    // (flet ((f (x) (+ x 1)) (g (x) (+ x 2))) (+ (f 1) (g 1))) : bodyからは両方呼べる
    lisp_val_t f_params = os_make_cons(os_make_symbol("x"), nil);
    lisp_val_t f_body = os_make_cons(make_call("+", 2, (lisp_val_t[]){ os_make_symbol("x"), os_make_fixnum(1) }), nil);
    lisp_val_t f_binding = make_flet_binding("f", f_params, f_body);

    lisp_val_t g_params = os_make_cons(os_make_symbol("x"), nil);
    lisp_val_t g_body = os_make_cons(make_call("+", 2, (lisp_val_t[]){ os_make_symbol("x"), os_make_fixnum(2) }), nil);
    lisp_val_t g_binding = make_flet_binding("g", g_params, g_body);

    lisp_val_t bindings = os_make_cons(f_binding, os_make_cons(g_binding, nil));
    lisp_val_t call_f = make_call("f", 1, (lisp_val_t[]){ os_make_fixnum(1) });
    lisp_val_t call_g = make_call("g", 1, (lisp_val_t[]){ os_make_fixnum(1) });
    lisp_val_t flet_body = os_make_cons(make_call("+", 2, (lisp_val_t[]){ call_f, call_g }), nil);
    lisp_val_t flet_form = os_make_cons(os_make_symbol("flet"), os_make_cons(bindings, flet_body));

    lisp_val_t v = os_eval(flet_form, env);
    assert(v == os_make_fixnum(5), "flet本体からは両方の関数f,gが呼べて(1+1)+(1+2)=5になる");
}

void test_os_eval_labels_mutual_recursion() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    os_set_function(os_make_symbol("-"), os_make_native_function((lisp_addr_t)(void *)primitive_subtract), env);
    os_set_function(os_make_symbol("eq"), os_make_native_function((lisp_addr_t)(void *)primitive_eq), env);

    // (labels ((my-even (n) (if (eq n 0) t (my-odd (- n 1))))
    //          (my-odd (n) (if (eq n 0) nil (my-even (- n 1)))))
    //   (my-even 10))
    lisp_val_t even_params = os_make_cons(os_make_symbol("n"), nil);
    lisp_val_t even_test = make_call("eq", 2, (lisp_val_t[]){ os_make_symbol("n"), os_make_fixnum(0) });
    lisp_val_t even_rec = make_call("my-odd", 1, (lisp_val_t[]){ make_call("-", 2, (lisp_val_t[]){ os_make_symbol("n"), os_make_fixnum(1) }) });
    lisp_val_t even_then = os_make_cons(g_sym_quote, os_make_cons(g_sym_t, nil));
    lisp_val_t even_if = make_call("if", 3, (lisp_val_t[]){ even_test, even_then, even_rec });
    lisp_val_t even_binding = make_flet_binding("my-even", even_params, os_make_cons(even_if, nil));

    lisp_val_t odd_params = os_make_cons(os_make_symbol("n"), nil);
    lisp_val_t odd_test = make_call("eq", 2, (lisp_val_t[]){ os_make_symbol("n"), os_make_fixnum(0) });
    lisp_val_t odd_rec = make_call("my-even", 1, (lisp_val_t[]){ make_call("-", 2, (lisp_val_t[]){ os_make_symbol("n"), os_make_fixnum(1) }) });
    lisp_val_t odd_if = make_call("if", 3, (lisp_val_t[]){ odd_test, nil, odd_rec });
    lisp_val_t odd_binding = make_flet_binding("my-odd", odd_params, os_make_cons(odd_if, nil));

    lisp_val_t bindings = os_make_cons(even_binding, os_make_cons(odd_binding, nil));
    lisp_val_t labels_body = os_make_cons(make_call("my-even", 1, (lisp_val_t[]){ os_make_fixnum(10) }), nil);
    lisp_val_t labels_form = os_make_cons(os_make_symbol("labels"), os_make_cons(bindings, labels_body));

    lisp_val_t v = os_eval(labels_form, env);
    assert(v == g_sym_t, "labelsの相互再帰でmy-even 10はTになる");
}

void test_os_eval_defvar_second_call_is_ignored() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t name = os_make_symbol("x");

    lisp_val_t defvar_form1 = os_make_cons(os_make_symbol("defvar"), os_make_cons(name, os_make_cons(os_make_fixnum(1), nil)));
    lisp_val_t v1 = os_eval(defvar_form1, env);
    assert(v1 == name, "defvarの戻り値は変数名x");
    assert(os_get_variable(name, env) == os_make_fixnum(1), "初回のdefvarで値1がセットされる");

    lisp_val_t defvar_form2 = os_make_cons(os_make_symbol("defvar"), os_make_cons(name, os_make_cons(os_make_fixnum(2), nil)));
    os_eval(defvar_form2, env);
    assert(os_get_variable(name, env) == os_make_fixnum(1), "既に束縛済みの場合、2回目のdefvarは無視され元の値1が残る");
}

void test_os_eval_defconstant_blocks_setq_but_only_in_its_own_env() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t name = os_make_symbol("x");

    lisp_val_t defconstant_form = os_make_cons(os_make_symbol("defconstant"), os_make_cons(name, os_make_cons(os_make_fixnum(1), nil)));
    lisp_val_t defined = os_eval(defconstant_form, env);
    assert(defined == name, "defconstantの戻り値は定数名x");
    assert(os_get_variable(name, env) == os_make_fixnum(1), "defconstantで値1がセットされる");

    lisp_val_t setq_form = os_make_cons(os_make_symbol("setq"), os_make_cons(name, os_make_cons(os_make_fixnum(2), nil)));
    lisp_val_t v = os_eval(setq_form, env);
    assert(v == g_sym_eval_error, "定数へのsetqはeval-errorになる");
    assert(os_get_variable(name, env) == os_make_fixnum(1), "定数へのsetqは失敗するので値1のまま変わらない");

    // 別環境の同名変数は定数ではないので通常通りsetqできる
    lisp_val_t other_env = os_make_environment(os_make_symbol("OTHER-ENV"), nil);
    os_set_variable(name, os_make_fixnum(10), other_env);
    lisp_val_t other_v = os_eval(setq_form, other_env);
    assert(other_v == os_make_fixnum(2), "別環境の同名変数xは定数ではないので通常通りsetqできる");
    assert(os_get_variable(name, other_env) == os_make_fixnum(2), "別環境のxは2に書き換わる");
}

void test_os_eval_defdynamic_and_dynamic_bypass_lexical_env() {
    lisp_val_t name = os_make_symbol("*x*");
    lisp_val_t defdynamic_form = os_make_cons(os_make_symbol("defdynamic"), os_make_cons(name, os_make_cons(os_make_fixnum(10), nil)));

    lisp_val_t env1 = os_make_environment(os_make_symbol("ENV1"), nil);
    lisp_val_t defined = os_eval(defdynamic_form, env1);
    assert(defined == name, "defdynamicの戻り値は変数名*x*");

    // env1とは無関係のenv2から(dynamic *x*)しても同じ値10が見える
    lisp_val_t env2 = os_make_environment(os_make_symbol("ENV2"), nil);
    lisp_val_t dynamic_form = os_make_cons(os_make_symbol("dynamic"), os_make_cons(name, nil));
    lisp_val_t v = os_eval(dynamic_form, env2);
    assert(v == os_make_fixnum(10), "定義に使ったenv1と無関係なenv2からもdynamicで同じ値10が見える(レキシカルenvを経由しない)");

    // env2から(defdynamic *x* 20)で上書きすると、env1側からも新しい値が見える
    lisp_val_t redefine_form = os_make_cons(os_make_symbol("defdynamic"), os_make_cons(name, os_make_cons(os_make_fixnum(20), nil)));
    os_eval(redefine_form, env2);
    lisp_val_t v2 = os_eval(dynamic_form, env1);
    assert(v2 == os_make_fixnum(20), "動的変数はグローバルなので、どの環境から更新しても他の環境から新しい値20が見える");
}

void test_os_eval_defglobal_reevaluates_and_allows_setq() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);
    lisp_val_t name = os_make_symbol("x");

    lisp_val_t defglobal_form1 = os_make_cons(os_make_symbol("defglobal"), os_make_cons(name, os_make_cons(os_make_fixnum(1), nil)));
    lisp_val_t v1 = os_eval(defglobal_form1, env);
    assert(v1 == name, "defglobalの戻り値は変数名x");
    assert(os_get_variable(name, env) == os_make_fixnum(1), "初回のdefglobalで値1がセットされる");

    // defvarと異なり、既に束縛済みでも2回目のdefglobalはvalue-formを再評価して上書きする
    lisp_val_t defglobal_form2 = os_make_cons(os_make_symbol("defglobal"), os_make_cons(name, os_make_cons(os_make_fixnum(2), nil)));
    os_eval(defglobal_form2, env);
    assert(os_get_variable(name, env) == os_make_fixnum(2), "2回目のdefglobalで値2に上書きされる");

    // defconstantと異なり、defglobalで定義した変数はsetqで書き換えられる
    lisp_val_t setq_form = os_make_cons(os_make_symbol("setq"), os_make_cons(name, os_make_cons(os_make_fixnum(3), nil)));
    lisp_val_t v = os_eval(setq_form, env);
    assert(v == os_make_fixnum(3), "defglobalの変数はsetqで書き換えられる");
    assert(os_get_variable(name, env) == os_make_fixnum(3), "setq後の値は3になる");
}

void test_os_eval_top_level_catches_return_from_to_top_level_block() {
    lisp_val_t env = os_make_environment(os_make_symbol("TEST-ENV"), nil);

    // (return-from %TOP-LEVEL 99) : os_eval_top_levelが張るblock %TOP-LEVELにcatchされ99になる
    lisp_val_t return_form = os_make_cons(os_make_symbol("return-from"),
                                  os_make_cons(g_sym_top_level_block, os_make_cons(os_make_fixnum(99), nil)));

    lisp_val_t v = os_eval_top_level(return_form, env);
    assert(v == os_make_fixnum(99), "os_eval_top_levelはトップレベルへのreturn-fromをcatchして99を返す(生のMAGIC_BLOCK_EXITが漏れない)");
}

void test_os_eval_top_level_passes_through_normal_form() {
    lisp_val_t env = make_arith_env();

    // (+ 1 2) : 通常のフォームはそのまま3を返す
    lisp_val_t args[2] = { os_make_fixnum(1), os_make_fixnum(2) };
    lisp_val_t form = make_call("+", 2, args);

    lisp_val_t v = os_eval_top_level(form, env);
    assert(v == os_make_fixnum(3), "os_eval_top_levelは通常のフォームをそのまま評価して3を返す");
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
    test_os_eval_nil_self_evaluates();
    test_os_eval_quote();
    test_os_eval_if();
    test_os_eval_progn();
    test_os_eval_setq_writes_current_env_only();
    test_os_eval_defun();
    test_os_eval_defun_multi_form_body();
    test_os_eval_defun_recursion();
    test_os_eval_lambda_immediate_invocation();
    test_os_eval_lambda_closes_over_defining_env();
    test_os_eval_defun_rest_param_collects_remaining_args();
    test_os_eval_defun_rest_param_with_leading_fixed_params();
    test_os_eval_defmacro_expands_and_evaluates();
    test_os_eval_defmacro_args_are_not_evaluated_before_expansion();
    test_os_eval_cons();
    test_os_eval_eq();
    test_os_eval_null();
    test_os_eval_quasiquote_plain_list();
    test_os_eval_quasiquote_unquote();
    test_os_eval_quasiquote_unquote_splicing();
    test_os_eval_block_return_from_immediate_exit();
    test_os_eval_return_from_crosses_function_call();
    test_os_eval_nested_block_return_from_only_exits_inner();
    test_os_eval_unwind_protect_runs_cleanup_on_normal_return();
    test_os_eval_unwind_protect_runs_cleanup_on_non_local_exit();
    test_os_eval_catch_throw_basic();
    test_os_eval_catch_throw_mismatched_tag_propagates_to_outer_catch();
    test_os_eval_tagbody_go_forward_jump_skips_form();
    test_os_eval_tagbody_go_backward_jump_loops_until_end_tag();
    test_primitive_macroexpand_1_expands_macro_call();
    test_primitive_macroexpand_1_returns_form_unchanged_when_not_macro();
    test_primitive_funcall_calls_native_function();
    test_primitive_funcall_calls_interpreted_function();
    test_os_eval_function_on_symbol_returns_function_object();
    test_os_eval_function_on_lambda_returns_callable_closure();
    test_os_eval_function_on_undefined_symbol_returns_eval_error();
    test_os_eval_flet_siblings_cannot_call_each_other();
    test_os_eval_flet_outer_scope_still_visible();
    test_os_eval_flet_body_can_call_both_bound_functions();
    test_os_eval_labels_mutual_recursion();
    test_os_eval_defvar_second_call_is_ignored();
    test_os_eval_defconstant_blocks_setq_but_only_in_its_own_env();
    test_os_eval_defdynamic_and_dynamic_bypass_lexical_env();
    test_os_eval_defglobal_reevaluates_and_allows_setq();
    test_os_eval_top_level_catches_return_from_to_top_level_block();
    test_os_eval_top_level_passes_through_normal_form();

    return g_test_failed ? 1 : 0;
}
