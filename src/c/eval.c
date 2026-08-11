#include "eval.h"
#include "lisp.h"

/**
 * vがblock/return-from/unwind-protect/catch/throw/tagbody/goの非局所脱出シグナル
 * (TAG_INSTANCE, MAGIC_BLOCK_EXIT/MAGIC_CATCH_EXIT/MAGIC_GO_EXIT)かどうかを判定する。
 * setjmp/longjmpが使えないfreestanding環境のため、脱出はこのシグナル値を評価器の各段で
 * 伝播させることで実現する(捕捉されるまでos_evalの呼び出しをすべて即座に巻き戻す)。
 * @param v 判定対象の値
 * @return 非局所脱出シグナルならnon-zero
 */
static int is_control_transfer(lisp_val_t v) {
    if ((v & TAG_MASK) != TAG_INSTANCE) {
        return 0;
    }
    UINT64 *obj = (UINT64 *)(v & ~TAG_MASK);
    return obj[0] == MAGIC_BLOCK_EXIT || obj[0] == MAGIC_CATCH_EXIT || obj[0] == MAGIC_GO_EXIT;
}

/**
 * args(未評価のリスト)を先頭から順にos_evalし、評価済みの値のリストを作る。
 * 途中で非局所脱出シグナルが現れた場合、残りの引数は評価せずそのシグナルをそのまま返す。
 * @param args 未評価の引数リスト
 * @param env 評価に使う環境
 * @return 評価済みの値のリスト。非局所脱出が起きた場合はその脱出シグナル
 */
static lisp_val_t eval_args(lisp_val_t args, lisp_val_t env) {
    if (args == nil) {
        return nil;
    }
    lisp_val_t head = os_eval(cc_car(args), env);
    if (is_control_transfer(head)) {
        return head;
    }
    lisp_val_t tail = eval_args(cc_cdr(args), env);
    if (is_control_transfer(tail)) {
        return tail;
    }
    return os_make_cons(head, tail);
}

/**
 * body(未評価のフォーム列)を先頭から順にos_evalし、最後の評価結果を返す。
 * defun/lambdaの本体評価とprogn特殊形式の両方から使う。
 * 途中で非局所脱出シグナルが現れた場合、残りのフォームは評価せずそのシグナルをそのまま返す。
 * @param body 未評価のフォーム列
 * @param env 評価に使う環境
 * @return 最後のフォームの評価結果。bodyが空ならnil。非局所脱出が起きた場合はその脱出シグナル
 */
static lisp_val_t eval_progn(lisp_val_t body, lisp_val_t env) {
    lisp_val_t result = nil;
    for (lisp_val_t rest = body; rest != nil; rest = cc_cdr(rest)) {
        result = os_eval(cc_car(rest), env);
        if (is_control_transfer(result)) {
            return result;
        }
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
    if (is_control_transfer(evaluated_args)) {
        return evaluated_args;
    }
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

    lisp_val_t test_result = os_eval(test, env);
    if (is_control_transfer(test_result)) {
        return test_result;
    }
    if (test_result != nil) {
        return os_eval(then_form, env);
    }
    if (else_rest != nil) {
        return os_eval(cc_car(else_rest), env);
    }
    return nil;
}

/**
 * setq特殊形式。(setq sym val-form)のval-formを評価し、envから親を辿って見つかった
 * 既存のsym束縛を上書きする(os_setq_variable)。クロージャ経由で外側のスコープの
 * 変数を書き換えられるようにするため、current environmentのvariablesスロットだけを
 * 見るos_set_variableとは異なる。
 * @param args (sym val-form)
 * @param env 評価・書き込み対象の環境
 * @return 書き込んだ値
 */
static lisp_val_t eval_setq(lisp_val_t args, lisp_val_t env) {
    lisp_val_t sym = cc_car(args);
    if (os_is_constant(sym, env)) {
        return g_sym_eval_error; // defconstantで定義された定数はsetqで上書きできない
    }
    lisp_val_t val_form = cc_car(cc_cdr(args));
    lisp_val_t val = os_eval(val_form, env);
    if (is_control_transfer(val)) {
        return val;
    }
    return os_setq_variable(sym, val, env);
}

/**
 * defvar特殊形式。(defvar name value-form)。current environmentの変数slotに
 * すでにnameが束縛されていれば何もしない(value-formも評価しない)。
 * 未束縛の場合のみvalue-formを評価してos_set_variableで登録する。
 * @param args (name . value-form-rest) value-form-restが空ならvalueはnil
 * @param env 登録先の環境
 * @return name(ISLispのdefvarの戻り値規約)
 */
static lisp_val_t eval_defvar(lisp_val_t args, lisp_val_t env) {
    lisp_val_t name = cc_car(args);
    lisp_val_t value_rest = cc_cdr(args);

    lisp_val_t var_slot = cc_car(cc_cdr(env)); // (variables . alist)
    lisp_val_t existing = cc_assoc_eq(name, cc_cdr(var_slot));
    if (existing == nil) {
        lisp_val_t val = (value_rest != nil) ? os_eval(cc_car(value_rest), env) : nil;
        if (is_control_transfer(val)) {
            return val;
        }
        os_set_variable(name, val, env);
    }
    return name;
}

/**
 * defconstant特殊形式。(defconstant name value-form)から、value-formを評価してcurrent
 * environmentの変数slotに登録し、さらにconstantsスロットにも登録してsetqでの上書きを禁止する。
 * @param args (name value-form)
 * @param env 登録先の環境
 * @return name(defvarと同じ戻り値規約)
 */
static lisp_val_t eval_defconstant(lisp_val_t args, lisp_val_t env) {
    lisp_val_t name = cc_car(args);
    lisp_val_t value_form = cc_car(cc_cdr(args));
    lisp_val_t val = os_eval(value_form, env);
    if (is_control_transfer(val)) {
        return val;
    }
    os_set_variable(name, val, env);
    os_mark_constant(name, env);
    return name;
}

/**
 * defdynamic特殊形式。(defdynamic name value-form)のvalue-formを評価し、レキシカルなenvの
 * 親子関係とは無関係なグローバルなg_dynamic_bindingsにnameの値として登録する。
 * @param args (name value-form)
 * @param env value-formの評価に使う環境
 * @return name(defvarと同じ戻り値規約)
 */
static lisp_val_t eval_defdynamic(lisp_val_t args, lisp_val_t env) {
    lisp_val_t name = cc_car(args);
    lisp_val_t value_form = cc_car(cc_cdr(args));
    lisp_val_t val = os_eval(value_form, env);
    if (is_control_transfer(val)) {
        return val;
    }
    os_set_dynamic(name, val);
    return name;
}

/**
 * defglobal特殊形式。(defglobal name value-form)のvalue-formを評価してcurrent environmentの
 * 変数slotに登録する。defconstantと異なりos_mark_constantは呼ばず、setqでの再代入を許す
 * (仕様上「変数を定義するためだけに使い、更新には使わない」のはdefglobal自身の再実行についての
 * 注意であり、変数自体は可変)。defvarと異なり既存束縛の有無を確認せず、常にvalue-formを
 * 評価してos_set_variableで(再)登録する。
 * @param args (name value-form)
 * @param env 登録先の環境
 * @return name(defvarと同じ戻り値規約)
 */
static lisp_val_t eval_defglobal(lisp_val_t args, lisp_val_t env) {
    lisp_val_t name = cc_car(args);
    lisp_val_t value_form = cc_car(cc_cdr(args));
    lisp_val_t val = os_eval(value_form, env);
    if (is_control_transfer(val)) {
        return val;
    }
    os_set_variable(name, val, env);
    return name;
}

/**
 * dynamic特殊形式。(dynamic name)のnameは未評価のシンボルとして扱い(quoteと同様)、
 * g_dynamic_bindingsからその動的変数の値を取得して返す。
 * @param args (name)
 * @param env 未使用
 * @return nameの動的変数の値。未定義の場合はnil
 */
static lisp_val_t eval_dynamic(lisp_val_t args, lisp_val_t env) {
    (void)env;
    return os_get_dynamic(cc_car(args));
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
 * function特殊形式。(function name)ならnameをenvから関数として解決して返し、
 * (function (lambda ...))のような式ならその式自体をos_evalして得た関数オブジェクトを返す。
 * @param args (name-or-lambda-expr)
 * @param env 解決・評価に使う環境
 * @return 関数オブジェクト。nameが未定義の場合はg_sym_eval_error
 */
static lisp_val_t eval_function(lisp_val_t args, lisp_val_t env) {
    lisp_val_t form = cc_car(args);
    if ((form & TAG_MASK) == TAG_SYMBOL) {
        lisp_val_t fn = os_get_function(form, env);
        if (fn == nil) {
            return g_sym_eval_error; // 未定義の関数
        }
        return fn;
    }
    return os_eval(form, env);
}

/**
 * flet特殊形式。(flet ((name (params...) body...) ...) body...)。bindingsで作られる各関数の
 * クロージャ環境は外側のenv(new_envではない)にするため、bindings同士は互いを見えない。
 * @param args (bindings . body)
 * @param env 外側の環境。かつbindingsで作る各関数のクロージャ環境
 * @return bodyの最後の評価結果
 */
static lisp_val_t eval_flet(lisp_val_t args, lisp_val_t env) {
    lisp_val_t bindings = cc_car(args);
    lisp_val_t body = cc_cdr(args);
    lisp_val_t new_env = os_make_environment(os_make_symbol("FLET-ENV"), env);

    for (lisp_val_t b = bindings; b != nil; b = cc_cdr(b)) {
        lisp_val_t binding = cc_car(b);
        lisp_val_t name = cc_car(binding);
        lisp_val_t params = cc_car(cc_cdr(binding));
        lisp_val_t fn_body = cc_cdr(cc_cdr(binding));
        lisp_val_t fn = make_interpreted_function(params, fn_body, env);
        os_set_function(name, fn, new_env);
    }

    return eval_progn(body, new_env);
}

/**
 * labels特殊形式。(labels ((name (params...) body...) ...) body...)。bindingsで作られる各関数の
 * クロージャ環境はnew_env自身にするため、呼び出し時には全員登録済みで相互再帰できる。
 * @param args (bindings . body)
 * @param env 外側の環境
 * @return bodyの最後の評価結果
 */
static lisp_val_t eval_labels(lisp_val_t args, lisp_val_t env) {
    lisp_val_t bindings = cc_car(args);
    lisp_val_t body = cc_cdr(args);
    lisp_val_t new_env = os_make_environment(os_make_symbol("LABELS-ENV"), env);

    for (lisp_val_t b = bindings; b != nil; b = cc_cdr(b)) {
        lisp_val_t binding = cc_car(b);
        lisp_val_t name = cc_car(binding);
        lisp_val_t params = cc_car(cc_cdr(binding));
        lisp_val_t fn_body = cc_cdr(cc_cdr(binding));
        lisp_val_t fn = make_interpreted_function(params, fn_body, new_env);
        os_set_function(name, fn, new_env);
    }

    return eval_progn(body, new_env);
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
 * block特殊形式。(block name body...)のbodyを順に評価する。
 * bodyの評価中に(return-from name value)による脱出シグナルが起きた場合、
 * それがこのblockのnameと一致するならvalueを返して捕捉し、一致しなければそのまま上位へ伝播する。
 * @param args (name . body)
 * @param env 評価に使う環境
 * @return bodyの最後の評価結果、またはreturn-fromで渡された値
 */
static lisp_val_t eval_block(lisp_val_t args, lisp_val_t env) {
    lisp_val_t name = cc_car(args);
    lisp_val_t body = cc_cdr(args);
    lisp_val_t result = eval_progn(body, env);
    if (is_control_transfer(result)) {
        UINT64 *obj = (UINT64 *)(result & ~TAG_MASK);
        if (obj[1] == name) {
            return obj[2];
        }
    }
    return result;
}

/**
 * return-from特殊形式。(return-from name value-form)のvalue-formを評価し、
 * nameを宛先とする非局所脱出シグナル(MAGIC_BLOCK_EXIT)を作って返す。
 * value-formの評価中に別の脱出シグナルが起きた場合は、それをそのまま返す(自分では包まない)。
 * @param args (name . value-form-rest) value-form-restが空ならvalueはnil
 * @param env 評価に使う環境
 * @return nameを宛先とする脱出シグナル
 */
static lisp_val_t eval_return_from(lisp_val_t args, lisp_val_t env) {
    lisp_val_t name = cc_car(args);
    lisp_val_t value_rest = cc_cdr(args);
    lisp_val_t val = (value_rest != nil) ? os_eval(cc_car(value_rest), env) : nil;
    if (is_control_transfer(val)) {
        return val;
    }
    return os_make_instance(MAGIC_BLOCK_EXIT, name, val, nil);
}

/**
 * unwind-protect特殊形式。(unwind-protect protected-form cleanup-form...)のprotected-formを評価し、
 * その結果(通常値・脱出シグナルのいずれでも)に関わらずcleanup-formを必ず評価してから、
 * protected-formの評価結果を返す。
 * 既知の簡略化: cleanup-form内で新たな脱出が起きた場合、その脱出は無視してprotected-formの結果を返す。
 * @param args (protected-form . cleanup-form-rest)
 * @param env 評価に使う環境
 * @return protected-formの評価結果(通常値または脱出シグナル)
 */
static lisp_val_t eval_unwind_protect(lisp_val_t args, lisp_val_t env) {
    lisp_val_t protected_form = cc_car(args);
    lisp_val_t cleanup_forms = cc_cdr(args);
    lisp_val_t result = os_eval(protected_form, env);
    eval_progn(cleanup_forms, env);
    return result;
}

/**
 * catch特殊形式。(catch tag-form form...)のtag-formを評価してcatch tagを得たうえで、
 * bodyをblockと同様に順に評価する。bodyの評価中に(throw tag-form result-form)による
 * 脱出シグナル(MAGIC_CATCH_EXIT)が起き、そのtagがeqで一致するならresultを返して捕捉し、
 * 一致しなければそのまま上位へ伝播する(block/return-fromと同じ形)。
 * @param args (tag-form . body)
 * @param env 評価に使う環境
 * @return bodyの最後の評価結果、またはthrowで渡された値
 */
static lisp_val_t eval_catch(lisp_val_t args, lisp_val_t env) {
    lisp_val_t tag_form = cc_car(args);
    lisp_val_t body = cc_cdr(args);
    lisp_val_t tag = os_eval(tag_form, env);
    if (is_control_transfer(tag)) {
        return tag;
    }
    lisp_val_t result = eval_progn(body, env);
    if ((result & TAG_MASK) == TAG_INSTANCE) {
        UINT64 *obj = (UINT64 *)(result & ~TAG_MASK);
        if (obj[0] == MAGIC_CATCH_EXIT && obj[1] == tag) {
            return obj[2];
        }
    }
    return result;
}

/**
 * throw特殊形式。(throw tag-form result-form)のtag-formとresult-formを評価し、
 * tagを宛先とする非局所脱出シグナル(MAGIC_CATCH_EXIT)を作って返す。
 * 対応するcatchが動的に外側に無い場合の扱いは未実装(仕様上はcontrol-error、
 * 本実装ではシグナルがそのまま最上位まで伝播する)。
 * @param args (tag-form result-form)
 * @param env 評価に使う環境
 * @return tagを宛先とする脱出シグナル
 */
static lisp_val_t eval_throw(lisp_val_t args, lisp_val_t env) {
    lisp_val_t tag_form = cc_car(args);
    lisp_val_t result_form = cc_car(cc_cdr(args));
    lisp_val_t tag = os_eval(tag_form, env);
    if (is_control_transfer(tag)) {
        return tag;
    }
    lisp_val_t result = os_eval(result_form, env);
    if (is_control_transfer(result)) {
        return result;
    }
    return os_make_instance(MAGIC_CATCH_EXIT, tag, result, nil);
}

/**
 * elemがtagbodyのbody中でtagbody-tag(識別子)として扱われる要素かどうかを判定する。
 * ISLisp仕様(§14.7.1)ではtagbody-tagは識別子(symbol)のみで、整数タグは扱わない。
 * nilはtagbody上ではsymbolだが、body要素としては(空リストと表記上区別できないため)
 * タグではなくformとして扱う。
 * @param elem body中の1要素
 * @return tagbody-tagならnon-zero
 */
static int is_tagbody_tag(lisp_val_t elem) {
    return elem != nil && (elem & TAG_MASK) == TAG_SYMBOL;
}

/**
 * tagbody特殊形式。(tagbody {tagbody-tag | form}*)のbodyを先頭から順に評価する
 * (symbolの要素はタグとして読み飛ばし、それ以外はformとして評価して値を捨てる)。
 * formの評価結果が(go tag)による脱出シグナル(MAGIC_GO_EXIT)で、そのtagが自分自身の
 * body中のタグに一致する場合はそのタグの直後にジャンプして続行する(前方・後方どちらも可)。
 * 一致しない場合はそのまま上位へ伝播する(外側のtagbodyが拾う可能性がある)。
 * 最後まで正常終了した場合、常にnilを返す(仕様通り、formの値はすべて捨てる)。
 * @param args body(tagbody-tagとformが混在するリスト)
 * @param env 評価に使う環境
 * @return 常にnil。捕捉されないgoが起きた場合はその脱出シグナル
 */
static lisp_val_t eval_tagbody(lisp_val_t args, lisp_val_t env) {
    lisp_val_t pos = args;
    while (pos != nil) {
        lisp_val_t elem = cc_car(pos);
        if (is_tagbody_tag(elem)) {
            pos = cc_cdr(pos);
            continue;
        }
        lisp_val_t result = os_eval(elem, env);
        if ((result & TAG_MASK) == TAG_INSTANCE) {
            UINT64 *obj = (UINT64 *)(result & ~TAG_MASK);
            if (obj[0] == MAGIC_GO_EXIT) {
                lisp_val_t dest = nil;
                int found = 0;
                for (lisp_val_t p = args; p != nil; p = cc_cdr(p)) {
                    if (is_tagbody_tag(cc_car(p)) && cc_car(p) == obj[1]) {
                        dest = cc_cdr(p);
                        found = 1;
                        break;
                    }
                }
                if (found) {
                    pos = dest;
                    continue;
                }
            }
            return result; // 自分のタグではない脱出シグナル(catch/block/他のgo)。そのまま伝播
        }
        pos = cc_cdr(pos);
    }
    return nil;
}

/**
 * go特殊形式。(go tag)のtagは評価しない(quoteと同様)。tagを宛先とする
 * 非局所脱出シグナル(MAGIC_GO_EXIT)を作って返すのみで、実際のジャンプはtagbody側が行う。
 * @param args (tag)
 * @param env 未使用
 * @return tagを宛先とする脱出シグナル
 */
static lisp_val_t eval_go(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t tag = cc_car(args);
    return os_make_instance(MAGIC_GO_EXIT, tag, nil, nil);
}

/**
 * 組み込み関数MACROEXPAND-1。formの先頭がマクロとして定義されたsymbolなら1段だけ展開して返し、
 * そうでなければformをそのまま返す(このインタプリタには多値機構が無いため、
 * ISLisp仕様上の「展開したか否か」の真偽値は返さない)。
 * @param args 評価済みの引数リスト(第一引数がformでなければならない)
 * @param env マクロ定義を解決する環境
 * @return 1段展開した結果、またはマクロでなければform自身
 */
lisp_val_t primitive_macroexpand_1(lisp_val_t args, lisp_val_t env) {
    lisp_val_t form = cc_car(args);
    if ((form & TAG_MASK) != TAG_CONS) {
        return form;
    }
    lisp_val_t op = cc_car(form);
    if ((op & TAG_MASK) != TAG_SYMBOL) {
        return form;
    }
    lisp_val_t fn = os_get_function(op, env);
    if (fn == nil || !is_macro(fn)) {
        return form;
    }
    return apply_macro(fn, cc_cdr(form));
}

/**
 * 組み込み関数FUNCALL。第一引数の関数オブジェクトを、残りの評価済み引数で呼び出す。
 * mapcar等、関数を値として受け取り呼び出す高階関数がLisp側から呼ぶために使う
 * (Lisp2スコープのため、変数に束縛された関数オブジェクトは(f x)のようには呼べない)。
 * @param args 評価済みの引数リスト(第一引数は関数オブジェクト、残りはその実引数)
 * @param env 呼び出し時の環境
 * @return 関数呼び出しの結果
 */
lisp_val_t primitive_funcall(lisp_val_t args, lisp_val_t env) {
    lisp_val_t fn = cc_car(args);
    lisp_val_t fn_args = cc_cdr(args);
    return apply_function(fn, fn_args, env);
}

/**
 * 組み込み関数%%APPLY。primitive_funcallとほぼ同じだが、実引数は構文上並べる
 * のではなく第二引数として渡された(評価済みの)リストをそのまま展開する。
 * @param args 評価済みの引数リスト((fn arg-list)の2要素)
 * @param env 呼び出し時の環境
 * @return 関数呼び出しの結果
 */
lisp_val_t primitive_apply(lisp_val_t args, lisp_val_t env) {
    lisp_val_t fn = cc_car(args);
    lisp_val_t fn_args = cc_car(cc_cdr(args));
    return apply_function(fn, fn_args, env);
}

/**
 * eval.cで実装した組み込み関数(macroexpand-1, funcall, %%apply)をglobal_environmentに登録する。
 */
void os_register_eval_primitives(void) {
    os_set_function(os_make_symbol("MACROEXPAND-1"),
                     os_make_native_function((lisp_addr_t)(void *)primitive_macroexpand_1),
                     global_environment);
    os_set_function(os_make_symbol("FUNCALL"),
                     os_make_native_function((lisp_addr_t)(void *)primitive_funcall),
                     global_environment);
    os_set_function(os_make_symbol("%%APPLY"),
                     os_make_native_function((lisp_addr_t)(void *)primitive_apply),
                     global_environment);
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
        if (op == g_sym_block) {
            return eval_block(args, env);
        }
        if (op == g_sym_return_from) {
            return eval_return_from(args, env);
        }
        if (op == g_sym_unwind_protect) {
            return eval_unwind_protect(args, env);
        }
        if (op == g_sym_function) {
            return eval_function(args, env);
        }
        if (op == g_sym_flet) {
            return eval_flet(args, env);
        }
        if (op == g_sym_labels) {
            return eval_labels(args, env);
        }
        if (op == g_sym_defvar) {
            return eval_defvar(args, env);
        }
        if (op == g_sym_defconstant) {
            return eval_defconstant(args, env);
        }
        if (op == g_sym_defdynamic) {
            return eval_defdynamic(args, env);
        }
        if (op == g_sym_defglobal) {
            return eval_defglobal(args, env);
        }
        if (op == g_sym_dynamic) {
            return eval_dynamic(args, env);
        }
        if (op == g_sym_catch) {
            return eval_catch(args, env);
        }
        if (op == g_sym_throw) {
            return eval_throw(args, env);
        }
        if (op == g_sym_tagbody) {
            return eval_tagbody(args, env);
        }
        if (op == g_sym_go) {
            return eval_go(args, env);
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

/**
 * formを(block %TOP-LEVEL form)相当としてenvのもとで評価する。トップレベルの
 * ドライバ(REPL/load)がこの関数を通すことで、途中でcatchされなかった
 * (%abort-top-level経由の)非局所脱出をこの1フォームの評価だけに閉じ込め、
 * 生の脱出シグナル(TAG_INSTANCE)がドライバやprintまで漏れるのを防ぐ。
 * @param form 評価対象のトップレベルフォーム
 * @param env 評価に使う環境
 * @return formの評価結果。abortされた場合はabortに渡されたcondition
 */
lisp_val_t os_eval_top_level(lisp_val_t form, lisp_val_t env) {
    lisp_val_t wrapped = os_make_cons(g_sym_block,
        os_make_cons(g_sym_top_level_block, os_make_cons(form, nil)));
    return os_eval(wrapped, env);
}

/**
 * apply_functionをruntime.c/reader.cのCプリミティブから呼べるように公開するラッパー。
 * @param fn 呼び出す関数オブジェクト
 * @param evaluated_args 評価済みの引数リスト
 * @param env 呼び出し時の環境
 * @return 関数呼び出しの結果。関数オブジェクトでない場合はg_sym_eval_error
 */
lisp_val_t os_apply_function(lisp_val_t fn, lisp_val_t evaluated_args, lisp_val_t env) {
    return apply_function(fn, evaluated_args, env);
}

/**
 * is_control_transferをruntime.c/reader.cのCプリミティブから呼べるように公開するラッパー。
 * @param v 判定対象の値
 * @return 非局所脱出シグナルならnon-zero
 */
int os_is_control_transfer(lisp_val_t v) {
    return is_control_transfer(v);
}
