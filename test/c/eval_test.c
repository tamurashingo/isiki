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
    test_os_eval_cons();
    test_os_eval_eq();
    test_os_eval_null();
    test_os_eval_quasiquote_plain_list();
    test_os_eval_quasiquote_unquote();
    test_os_eval_quasiquote_unquote_splicing();

    return g_test_failed ? 1 : 0;
}
