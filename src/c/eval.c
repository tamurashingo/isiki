#include "eval.h"
#include "lisp.h"

/**
 * args(未評価のリスト)を先頭から順にos_evalし、評価済みの値のリストを作る。
 * @param args 未評価の引数リスト
 * @param env 評価に使う環境
 * @return 評価済みの値のリスト
 */
static lisp_val_t eval_args(lisp_val_t args, lisp_val_t env) {
    if (args == nil) {
        return nil;
    }
    lisp_val_t head = os_eval(cc_car(args), env);
    lisp_val_t tail = eval_args(cc_cdr(args), env);
    return os_make_cons(head, tail);
}

/**
 * ネイティブ関数(TAG_INSTANCE, MAGIC_FUNCTION_NATIVE)を評価済み引数で呼び出す。
 * @param fn 呼び出す関数オブジェクト
 * @param evaluated_args 評価済みの引数リスト
 * @param env 呼び出し時の環境
 * @return 関数呼び出しの結果。ネイティブ関数でない場合はg_sym_eval_error
 */
static lisp_val_t apply_function(lisp_val_t fn, lisp_val_t evaluated_args, lisp_val_t env) {
    lisp_addr_t addr = fn & ~TAG_MASK;
    UINT64 *obj = (UINT64 *)addr;
    if (obj[0] != MAGIC_FUNCTION_NATIVE) {
        return g_sym_eval_error; // interpreted function(defun)は今回のスコープ外
    }
    lisp_val_t (*fnptr)(lisp_val_t, lisp_val_t) = (lisp_val_t (*)(lisp_val_t, lisp_val_t))obj[1];
    return fnptr(evaluated_args, env);
}

/**
 * (op . args)形式のS式を評価する。opをenvから関数として解決し、argsを評価してから呼び出す。
 * @param op 関数を表すシンボル
 * @param args 未評価の引数リスト
 * @param env 評価に使う環境
 * @return 関数呼び出しの結果。opが未定義の場合はg_sym_eval_error
 */
static lisp_val_t eval_form(lisp_val_t op, lisp_val_t args, lisp_val_t env) {
    lisp_val_t fn = os_get_function(op, env);
    if (fn == nil) {
        return g_sym_eval_error; // 未定義の関数
    }
    lisp_val_t evaluated_args = eval_args(args, env);
    return apply_function(fn, evaluated_args, env);
}

/**
 * exp を env のもとで評価する。
 * SYMBOLはenvから値をlookupし、CONSはcarを関数、cdrを引数として評価する。
 * それ以外(FIXNUM/STRING/CHAR/INSTANCEなど)は自己評価する。
 * @param exp 評価対象のS式
 * @param env 評価に使う環境
 * @return 評価結果
 */
lisp_val_t os_eval(lisp_val_t exp, lisp_val_t env) {
    UINT64 tag = exp & TAG_MASK;
    if (tag == TAG_SYMBOL) {
        return os_get_variable(exp, env);
    }
    if (tag == TAG_CONS) {
        return eval_form(cc_car(exp), cc_cdr(exp), env);
    }
    return exp; // FIXNUM/STRING/CHAR/INSTANCE は自己評価
}
