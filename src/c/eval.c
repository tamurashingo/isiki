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
 * body(未評価のフォーム列)を先頭から順にos_evalし、最後の評価結果を返す。
 * defun/lambdaの本体評価とprogn特殊形式の両方から使う。
 * @param body 未評価のフォーム列
 * @param env 評価に使う環境
 * @return 最後のフォームの評価結果。bodyが空ならnil
 */
static lisp_val_t eval_progn(lisp_val_t body, lisp_val_t env) {
    lisp_val_t result = nil;
    for (lisp_val_t rest = body; rest != nil; rest = cc_cdr(rest)) {
        result = os_eval(cc_car(rest), env);
    }
    return result;
}

/**
 * (params . body)と定義時のenvから、Lisp(defun/lambda)で定義された関数オブジェクトを作る。
 * @param params 仮引数リスト(未評価のシンボルリスト)
 * @param body 本体(未評価のフォーム列)
 * @param env 定義時の環境
 * @return MAGIC_FUNCTION_INTERPRETEDのINSTANCE
 */
static lisp_val_t make_interpreted_function(lisp_val_t params, lisp_val_t body, lisp_val_t env) {
    return os_make_instance(MAGIC_FUNCTION_INTERPRETED, params, body, env);
}

/**
 * 仮引数リストと評価済み実引数リストを先頭から順に対応させ、call_envにos_set_variableで束縛する。
 * 仮引数が多い場合の余りはnilに束縛され、実引数が多い場合の余りは無視される。
 * 仮引数リストに&restが現れた場合、その次の仮引数にその時点で残っている実引数を
 * リストのまま束縛し、それ以降の仮引数は処理しない(&rest は仮引数リストの末尾に置く前提)。
 * @param params 仮引数リスト(未評価のシンボルリスト。&restを含みうる)
 * @param evaluated_args 評価済みの実引数リスト
 * @param call_env 束縛先の環境
 */
static void bind_params(lisp_val_t params, lisp_val_t evaluated_args, lisp_val_t call_env) {
    lisp_val_t p = params;
    lisp_val_t a = evaluated_args;
    while (p != nil) {
        lisp_val_t param = cc_car(p);
        if (param == g_sym_rest) {
            lisp_val_t rest_param = cc_car(cc_cdr(p));
            os_set_variable(rest_param, a, call_env);
            return;
        }
        lisp_val_t val = (a != nil) ? cc_car(a) : nil;
        os_set_variable(param, val, call_env);
        p = cc_cdr(p);
        a = (a != nil) ? cc_cdr(a) : nil;
    }
}

/**
 * ネイティブ関数(TAG_INSTANCE, MAGIC_FUNCTION_NATIVE)を評価済み引数で呼び出す。
 * @param fn 呼び出す関数オブジェクト
 * @param evaluated_args 評価済みの引数リスト
 * @param env 呼び出し時の環境
 * @return 関数呼び出しの結果。関数オブジェクトでない場合はg_sym_eval_error
 */
static lisp_val_t apply_function(lisp_val_t fn, lisp_val_t evaluated_args, lisp_val_t env) {
    lisp_addr_t addr = fn & ~TAG_MASK;
    UINT64 *obj = (UINT64 *)addr;
    if (obj[0] == MAGIC_FUNCTION_NATIVE) {
        lisp_val_t (*fnptr)(lisp_val_t, lisp_val_t) = (lisp_val_t (*)(lisp_val_t, lisp_val_t))obj[1];
        return fnptr(evaluated_args, env);
    }
    if (obj[0] == MAGIC_FUNCTION_INTERPRETED) {
        lisp_val_t params = obj[1];
        lisp_val_t body = obj[2];
        lisp_val_t closure_env = obj[3];
        lisp_val_t call_env = os_make_environment(os_make_symbol("CALL-ENV"), closure_env);
        bind_params(params, evaluated_args, call_env);
        return eval_progn(body, call_env);
    }
    return g_sym_eval_error; // 関数オブジェクトではない
}

/**
 * (op . args)形式のS式を評価する。opがsymbolならenvから関数として解決し、
 * それ以外(即時呼び出しされる(lambda ...)など)ならopそのものを評価して関数オブジェクトを得る。
 * argsを評価してから呼び出す。
 * @param op 関数を表すシンボル、または関数オブジェクトへ評価される式
 * @param args 未評価の引数リスト
 * @param env 評価に使う環境
 * @return 関数呼び出しの結果。opが未定義の場合はg_sym_eval_error
 */
static lisp_val_t eval_form(lisp_val_t op, lisp_val_t args, lisp_val_t env) {
    lisp_val_t fn = ((op & TAG_MASK) == TAG_SYMBOL) ? os_get_function(op, env) : os_eval(op, env);
    if (fn == nil) {
        return g_sym_eval_error; // 未定義の関数
    }
    lisp_val_t evaluated_args = eval_args(args, env);
    return apply_function(fn, evaluated_args, env);
}

/**
 * quote特殊形式。(quote x)のxをそのまま(未評価で)返す。
 * @param args (x)
 * @param env 未使用
 * @return x(未評価)
 */
static lisp_val_t eval_quote(lisp_val_t args, lisp_val_t env) {
    (void)env;
    return cc_car(args);
}

/**
 * if特殊形式。(if test then else...)のtestを評価し、非nilならthen、
 * nilならelse(省略時はnil)を評価して返す。
 * @param args (test then . else-rest)
 * @param env 評価に使う環境
 * @return thenまたはelseの評価結果
 */
static lisp_val_t eval_if(lisp_val_t args, lisp_val_t env) {
    lisp_val_t test = cc_car(args);
    lisp_val_t then_form = cc_car(cc_cdr(args));
    lisp_val_t else_rest = cc_cdr(cc_cdr(args));

    if (os_eval(test, env) != nil) {
        return os_eval(then_form, env);
    }
    if (else_rest != nil) {
        return os_eval(cc_car(else_rest), env);
    }
    return nil;
}

/**
 * setq特殊形式。(setq sym val-form)のval-formを評価し、current environmentのvariablesスロットにのみ書き込む。
 * @param args (sym val-form)
 * @param env 評価・書き込み対象の環境
 * @return 書き込んだ値
 */
static lisp_val_t eval_setq(lisp_val_t args, lisp_val_t env) {
    lisp_val_t sym = cc_car(args);
    lisp_val_t val_form = cc_car(cc_cdr(args));
    lisp_val_t val = os_eval(val_form, env);
    return os_set_variable(sym, val, env);
}

/**
 * defun特殊形式。(defun name (params...) body...)から関数オブジェクトを作り、
 * current environmentのfunctionsスロットに登録する。
 * @param args (name (params...) . body)
 * @param env 登録先の環境。かつ関数の定義時環境(クロージャ)にもなる
 * @return name(ISLispのdefunの戻り値規約)
 */
static lisp_val_t eval_defun(lisp_val_t args, lisp_val_t env) {
    lisp_val_t name = cc_car(args);
    lisp_val_t params = cc_car(cc_cdr(args));
    lisp_val_t body = cc_cdr(cc_cdr(args));

    lisp_val_t fn = make_interpreted_function(params, body, env);
    os_set_function(name, fn, env);
    return name;
}

/**
 * lambda特殊形式。(lambda (params...) body...)から、登録はせずに関数オブジェクトだけを作って返す。
 * @param args ((params...) . body)
 * @param env 関数の定義時環境(クロージャ)
 * @return 作成した関数オブジェクト
 */
static lisp_val_t eval_lambda(lisp_val_t args, lisp_val_t env) {
    lisp_val_t params = cc_car(args);
    lisp_val_t body = cc_cdr(args);
    return make_interpreted_function(params, body, env);
}

/**
 * (params . body)と定義時のenvから、defmacroで定義されたマクロオブジェクトを作る。
 * MAGIC_FUNCTION_INTERPRETEDと同じ(params, body, env)のレイアウトだが、
 * MAGIC_MACROの色を付けることで通常の関数と区別する。
 * @param params 仮引数リスト(未評価のシンボルリスト)
 * @param body 本体(未評価のフォーム列。評価結果は展開後のコード)
 * @param env 定義時の環境
 * @return MAGIC_MACROのINSTANCE
 */
static lisp_val_t make_macro(lisp_val_t params, lisp_val_t body, lisp_val_t env) {
    return os_make_instance(MAGIC_MACRO, params, body, env);
}

/**
 * defmacro特殊形式。(defmacro name (params...) body...)からマクロオブジェクトを作り、
 * current environmentのfunctionsスロットに登録する。
 * @param args (name (params...) . body)
 * @param env 登録先の環境。かつマクロの定義時環境(クロージャ)にもなる
 * @return name(defunと同じ戻り値規約)
 */
static lisp_val_t eval_defmacro(lisp_val_t args, lisp_val_t env) {
    lisp_val_t name = cc_car(args);
    lisp_val_t params = cc_car(cc_cdr(args));
    lisp_val_t body = cc_cdr(cc_cdr(args));

    lisp_val_t macro = make_macro(params, body, env);
    os_set_function(name, macro, env);
    return name;
}

/**
 * fnがdefmacroで定義されたマクロ(TAG_INSTANCE, MAGIC_MACRO)かどうかを判定する。
 * @param fn 判定対象の値
 * @return マクロならnon-zero
 */
static int is_macro(lisp_val_t fn) {
    if ((fn & TAG_MASK) != TAG_INSTANCE) {
        return 0;
    }
    UINT64 *obj = (UINT64 *)(fn & ~TAG_MASK);
    return obj[0] == MAGIC_MACRO;
}

/**
 * マクロを未評価の実引数argsで展開する(argsはevalせずそのままパラメータに束縛する)。
 * @param macro 展開するマクロオブジェクト(MAGIC_MACRO)
 * @param args 未評価の実引数リスト
 * @return 展開結果として得られた、これから評価すべきコード
 */
static lisp_val_t apply_macro(lisp_val_t macro, lisp_val_t args) {
    lisp_addr_t addr = macro & ~TAG_MASK;
    UINT64 *obj = (UINT64 *)addr;
    lisp_val_t params = obj[1];
    lisp_val_t body = obj[2];
    lisp_val_t closure_env = obj[3];
    lisp_val_t call_env = os_make_environment(os_make_symbol("MACRO-ENV"), closure_env);
    bind_params(params, args, call_env);
    return eval_progn(body, call_env);
}

static lisp_val_t qq_expand(lisp_val_t form, lisp_val_t env);

/**
 * list(評価済み、unquote-splicingで得られたリスト)の要素をtailの手前に非破壊的に継ぎ足す。
 * @param list 継ぎ足す要素のリスト
 * @param tail listの末尾に続ける残りのリスト
 * @return listの要素 . tail
 */
static lisp_val_t qq_append(lisp_val_t list, lisp_val_t tail) {
    if (list == nil) {
        return tail;
    }
    return os_make_cons(cc_car(list), qq_append(cc_cdr(list), tail));
}

/**
 * quasiquoteの本体(未評価のフォーム)を、unquote/unquote-splicingだけを評価しながら組み立てる。
 * nested quasiquote(入れ子のquasiquote)の深さは追跡しない簡易実装。
 * @param form 展開対象のフォーム(quasiquoteの直下、またはその再帰呼び出し)
 * @param env unquoteの評価に使う環境
 * @return 組み立てた値
 */
static lisp_val_t qq_expand(lisp_val_t form, lisp_val_t env) {
    if (form == nil || (form & TAG_MASK) != TAG_CONS) {
        return form; // atom/nilは評価せずそのまま
    }

    lisp_val_t elem = cc_car(form);

    if (elem == g_sym_unquote) {
        // (unquote x): xを評価してその場に差し込む
        return os_eval(cc_car(cc_cdr(form)), env);
    }
    if (elem == g_sym_unquote_splicing) {
        // リストの要素位置以外(先頭やドット対の末尾)でのunquote-splicingはunquoteと同様に扱う
        return os_eval(cc_car(cc_cdr(form)), env);
    }
    if ((elem & TAG_MASK) == TAG_CONS && cc_car(elem) == g_sym_unquote_splicing) {
        // リストの要素が(unquote-splicing x): xを評価し、その要素を残りに継ぎ足す
        lisp_val_t spliced = os_eval(cc_car(cc_cdr(elem)), env);
        lisp_val_t rest = qq_expand(cc_cdr(form), env);
        return qq_append(spliced, rest);
    }

    lisp_val_t head = qq_expand(elem, env);
    lisp_val_t tail = qq_expand(cc_cdr(form), env);
    return os_make_cons(head, tail);
}

/**
 * quasiquote特殊形式。(quasiquote form)のformをunquote/unquote-splicing以外は未評価のまま組み立てて返す。
 * @param args (form)
 * @param env unquoteの評価に使う環境
 * @return 組み立てた値
 */
static lisp_val_t eval_quasiquote(lisp_val_t args, lisp_val_t env) {
    return qq_expand(cc_car(args), env);
}

/**
 * exp を env のもとで評価する。
 * SYMBOLはenvから値をlookupし、CONSはcarが特殊形式シンボルならその処理を、
 * そうでなければcarを関数、cdrを引数として評価する。
 * それ以外(FIXNUM/STRING/CHAR/INSTANCEなど)は自己評価する。
 * @param exp 評価対象のS式
 * @param env 評価に使う環境
 * @return 評価結果
 */
lisp_val_t os_eval(lisp_val_t exp, lisp_val_t env) {
    if (exp == nil) {
        return nil;
    }
    UINT64 tag = exp & TAG_MASK;
    if (tag == TAG_SYMBOL) {
        return os_get_variable(exp, env);
    }
    if (tag == TAG_CONS) {
        lisp_val_t op = cc_car(exp);
        lisp_val_t args = cc_cdr(exp);
        if (op == g_sym_quote) {
            return eval_quote(args, env);
        }
        if (op == g_sym_if) {
            return eval_if(args, env);
        }
        if (op == g_sym_progn) {
            return eval_progn(args, env);
        }
        if (op == g_sym_setq) {
            return eval_setq(args, env);
        }
        if (op == g_sym_defun) {
            return eval_defun(args, env);
        }
        if (op == g_sym_lambda) {
            return eval_lambda(args, env);
        }
        if (op == g_sym_defmacro) {
            return eval_defmacro(args, env);
        }
        if (op == g_sym_quasiquote) {
            return eval_quasiquote(args, env);
        }
        if ((op & TAG_MASK) == TAG_SYMBOL) {
            lisp_val_t fn = os_get_function(op, env);
            if (fn != nil && is_macro(fn)) {
                lisp_val_t expanded = apply_macro(fn, args);
                return os_eval(expanded, env);
            }
        }
        return eval_form(op, args, env);
    }
    return exp; // FIXNUM/STRING/CHAR/INSTANCE は自己評価
}
