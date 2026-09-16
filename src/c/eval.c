#include "eval.h"
#include "lisp.h"
#include "za.h"

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
    // argsはこの関数のCスタックフレーム独自のローカル変数(値渡し)であり、
    // 呼び出し元(os_eval等)がGC_PROTECTしていても、それはあくまで呼び出し元自身の
    // スタックスロットの保護に過ぎずここには及ばない。os_eval(cc_car(args), env)は
    // 任意の深さの評価(GCを誘発しうる)を行うため、その後のcc_cdr(args)より前に
    // 自分自身でargsを保護しておく必要がある
    GC_PROTECT(args);
    GC_PROTECT(env);
    lisp_val_t head = os_eval(cc_car(args), env);
    GC_PROTECT(head);
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
    lisp_val_t rest = body;
    GC_PROTECT(rest);
    GC_PROTECT(env);
    for (; rest != nil; rest = cc_cdr(rest)) {
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
    GC_PROTECT(params);
    GC_PROTECT(body);
    GC_PROTECT(env);
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
    // os_set_variableはcall_envへの新規束縛時にos_make_consで確保を行いGCを誘発しうる。
    // ループが2回以上回る(仮引数が2個以上)場合、p/a/call_envはこの関数自身のCスタック
    // フレーム独自のローカル変数なので、呼び出し元の保護とは無関係にここで保護しないと
    // 次のパラメータを束縛する時点で全て古いアドレスを指したままになる
    GC_PROTECT(p);
    GC_PROTECT(a);
    GC_PROTECT(call_env);
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
        // ABI-M4: word1はza_fn_meta_tへの生ポインタ。meta->cons_entryが従来通りの
        // consリストABI fn(evaluated_args, env)の実体を指す(fixed_entryはABI-M5まで未使用)。
        za_fn_meta_t *meta = (za_fn_meta_t *)obj[1];
        lisp_val_t (*fnptr)(lisp_val_t, lisp_val_t) = (lisp_val_t (*)(lisp_val_t, lisp_val_t))meta->cons_entry;
        // word2がfixnum 2(トランスパイラがリフトしたlambdaのクロージャ)の場合、
        // word3(定義時に捕捉した自由変数を保持する環境)をenv引数として渡す。
        // 呼び出し元のenvではなく、リフトされた関数本体が自由変数を解決できる
        // 環境を渡す必要があるため
        // word3(定義時環境/捕捉環境)がNILでなければそれを渡す(JITコンパイル済みdefunも
        // os_make_jit_functionで定義時環境を持つ。レキシカルスコープのため)
        lisp_val_t call_env = (obj[3] != nil) ? obj[3] : env;
        return fnptr(evaluated_args, call_env);
    }
    if (obj[0] == MAGIC_FUNCTION_INTERPRETED) {
        lisp_val_t params = obj[1];
        lisp_val_t body = obj[2];
        lisp_val_t closure_env = obj[3];
        GC_PROTECT(params);
        GC_PROTECT(evaluated_args);
        GC_PROTECT(closure_env);
        GC_PROTECT(body);
        lisp_val_t call_env = os_make_frame(os_make_symbol("CALL-ENV"), closure_env);
        // bind_params内のGC_PROTECTはbind_params自身のスタックフレーム限りで、
        // ここ(呼び出し元)のcall_envローカルは別のスタックスロットなので追随しない。
        // bind_params完了後もcall_envをeval_prognに渡すため、ここでも保護する
        GC_PROTECT(call_env);
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
    // [原則4の系] opの解決(os_get_functionの確保、opが(lambda ...)等ならos_evalの
    // 任意深さの評価)はGCを誘発しうる。args/env/opの保護はそれより**前**でなければ
    // ならない。あとからGC_PROTECTしても、入ってきた時点で古い値は直せない。
    //
    // 実測(塗り潰し監査、environment_pages_test): argsは一度も保護されておらず、
    // envの保護もos_evalの後だったため、eval_argsに最初から塗り潰し済みの
    // args/envが渡り、cc_car+0x1dで #GP になった(rcx=rbx=0xDEADDEADDEADDEA0)。
    // eval_args側のGC_PROTECTは自分のフレームのスロットを守るだけで、
    // 呼び出し元が古い値を渡してくる場合には効かない。
    GC_PROTECT(op);
    GC_PROTECT(args);
    GC_PROTECT(env);
    lisp_val_t fn = ((op & TAG_MASK) == TAG_SYMBOL) ? os_get_function(op, env) : os_eval(op, env);
    GC_PROTECT(fn);
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
    GC_PROTECT(then_form);
    GC_PROTECT(else_rest);
    GC_PROTECT(env);

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
    GC_PROTECT(sym);
    GC_PROTECT(env);
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
    GC_PROTECT(name);
    GC_PROTECT(env);

    // 定義の書き込み先は呼び出し時のenvではなく、そこから最も近い「捨てられない環境」。
    // 既存束縛の検査も同じ環境に対して行う(検査と書き込みが別の環境だと、
    // frame側に無いからと評価したのにowner側では既に束縛済み、という食い違いになる)
    lisp_val_t owner = os_definition_env(env);
    GC_PROTECT(owner);

    lisp_val_t var_slot = cc_car(cc_cdr(owner)); // (variables . alist)
    lisp_val_t existing = cc_assoc_eq(name, cc_cdr(var_slot));
    if (existing == nil) {
        lisp_val_t val = (value_rest != nil) ? os_eval(cc_car(value_rest), env) : nil;
        if (is_control_transfer(val)) {
            return val;
        }
        os_set_variable(name, val, owner);
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
    GC_PROTECT(name);
    GC_PROTECT(env);
    // 値と定数フラグは**必ず同じ環境へ**書くこと。片方だけownerにすると、
    // 値はowner・フラグはframeに散らばり、setqでの上書き禁止が効かなくなる
    lisp_val_t owner = os_definition_env(env);
    GC_PROTECT(owner);
    lisp_val_t val = os_eval(value_form, env);
    if (is_control_transfer(val)) {
        return val;
    }
    os_set_variable(name, val, owner);
    os_mark_constant(name, owner);
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
    GC_PROTECT(name);
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
    GC_PROTECT(name);
    GC_PROTECT(env);
    lisp_val_t owner = os_definition_env(env);
    GC_PROTECT(owner);
    lisp_val_t val = os_eval(value_form, env);
    if (is_control_transfer(val)) {
        return val;
    }
    os_set_variable(name, val, owner);
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
    GC_PROTECT(name);
    GC_PROTECT(env);
    // params/bodyはza_try_compile_defun(JITコンパイラ本体。大量に確保する)を跨いで
    // 生存し、失敗時はその後のmake_interpreted_functionへ渡される。保護していないと、
    // コンパイル中にGCが1回でも走った時点で両方staleになる。
    // za_try_compile_defun側の保護は「向こうのコピー」を守るだけで、こちらの
    // ローカルには及ばない(documents/pitfalls.md 原則4)
    GC_PROTECT(params);
    GC_PROTECT(body);

    // [重要] envとownerは役割が違う。
    //   env   = 捕捉環境。関数オブジェクトのword3に入り、本体の自由変数を
    //           実行時にos_get_variableが辿る起点になる。**frameでよい**
    //   owner = 定義の登録先。letやflet等の捨てられる環境に書くと到達不能になる
    // ここを取り違えると、テストは通るのに自由変数だけが静かに壊れる
    lisp_val_t owner = os_definition_env(env);
    GC_PROTECT(owner);

    /* [declaim] optimizeはownerから読む。**新たに環境を探索しない**
       (ownerは既にos_definition_envで求まっている。documents/declaim-design.md)。
       本Phaseではza_try_compile_defunは受け取った値をmetaへ記録するだけで、
       コード生成には使わない(Phase3以降) */
    UINT64 optimize = os_env_optimize(owner);
    lisp_val_t fn = za_try_compile_defun(params, body, env, owner, optimize);
    if (fn == nil) {
        fn = make_interpreted_function(params, body, env);
    }
    os_set_function(name, fn, owner);
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
/**
 * declaim特殊形式。(declaim (optimize (speed 3) (safety 0)) ...)。
 *
 * **CommonLispとは意味論が異なる。** 本実装はenvironment単位で作用し、親からは
 * 引き継がない(documents/declaim-design.md)。評価された環境から
 * os_definition_envでenvironmentを求めてそこへ記録するので、let/flet等のframeの
 * 中でdeclaimしても読み飛ばされて囲みのenvironmentへ届く。
 *
 * 本PhaseはoptimizeのみでtypeやinlineはPhase3以降。**未知の指定子はエラーにせず
 * 無視する**(後のPhaseで実装予定のものを先に書いたコードが動かなくなるのを避ける)。
 * 指定しなかった項目は現在値のまま変更しない。値域は0〜3で、外れたらエラー。
 *
 * @param args 宣言指定子のリスト
 * @param env 評価時の環境(frameでよい)
 * @return nil
 */
static lisp_val_t eval_declaim(lisp_val_t args, lisp_val_t env) {
    GC_PROTECT(args);
    GC_PROTECT(env);
    UINT64 packed = os_env_optimize(env);

    lisp_val_t rest = args;
    while (rest != nil) {
        lisp_val_t spec = cc_car(rest);
        rest = cc_cdr(rest);
        /* optimize以外の指定子(type/inline/ftype等)は黙って読み飛ばす */
        if ((spec & TAG_MASK) != TAG_CONS || cc_car(spec) != g_sym_optimize) {
            continue;
        }
        lisp_val_t items = cc_cdr(spec);
        while (items != nil) {
            lisp_val_t item = cc_car(items);
            items = cc_cdr(items);
            /* (speed 3) の形のみ受け付ける。裸の speed(= 3扱い)は本Phaseでは非対応 */
            if ((item & TAG_MASK) != TAG_CONS) {
                continue;
            }
            lisp_val_t qual = cc_car(item);
            lisp_val_t vcell = cc_cdr(item);
            if ((vcell & TAG_MASK) != TAG_CONS) {
                continue;
            }
            lisp_val_t vval = cc_car(vcell);
            /* 値域は0〜3。整数でない・負・4以上はいずれも<domain-error>にする。
               負のfixnumはFIXNUM_SIGN_BITが立つのでマグニチュードだけ見ると-1が1に
               化ける。符号ビットの有無をos_make_fixnumで作り直して比較することで弾く */
            if ((vval & TAG_MASK) != TAG_FIXNUM ||
                vval != os_make_fixnum(os_fixnum_magnitude(vval)) ||
                os_fixnum_magnitude(vval) > 3) {
                return os_signal_condition(g_sym_class_domain_error, nil, env);
            }
            UINT64 v = os_fixnum_magnitude(vval);
            if (qual == g_sym_speed) {
                packed = OPTIMIZE_PACK(v, OPTIMIZE_SAFETY(packed), OPTIMIZE_SPACE(packed));
            } else if (qual == g_sym_safety) {
                packed = OPTIMIZE_PACK(OPTIMIZE_SPEED(packed), v, OPTIMIZE_SPACE(packed));
            } else if (qual == g_sym_space) {
                packed = OPTIMIZE_PACK(OPTIMIZE_SPEED(packed), OPTIMIZE_SAFETY(packed), v);
            }
            /* 未知のqualityも無視する */
        }
    }

    os_env_set_optimize(env, packed);
    return nil;
}

static lisp_val_t eval_flet(lisp_val_t args, lisp_val_t env) {
    // [原則4] envはos_make_symbol("FLET-ENV")の確保より**前**に保護すること。
    // 引数の評価順は未規定なので、シンボル確保でGCが走った時点でenvが古いまま
    // os_make_environmentへ渡りうる。
    // 実測(塗り潰し監査、environment_literal_slots_test): ここが漏れていたため
    // make_interpreted_function(eval.c:83)のGC_PROTECT(env)に既にstaleな値が入り、
    // 最終的にos_get_function内のcc_cdrが塗り潰し済みポインタを参照して #GP になった。
    GC_PROTECT(env);
    lisp_val_t bindings = cc_car(args);
    lisp_val_t body = cc_cdr(args);
    GC_PROTECT(bindings);
    GC_PROTECT(body);
    lisp_val_t new_env = os_make_frame(os_make_symbol("FLET-ENV"), env);
    GC_PROTECT(new_env);

    for (lisp_val_t b = bindings; b != nil; b = cc_cdr(b)) {
        // b/nameはmake_interpreted_functionとos_set_functionの確保を跨ぐ。
        // 保護はループ**本体の中**で行う(forの初期化子から巻き上げるとスコープが
        // 変わり、過去に別件で沈黙ハングを起こしている)
        GC_PROTECT(b);
        lisp_val_t binding = cc_car(b);
        lisp_val_t name = cc_car(binding);
        GC_PROTECT(name);
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
    // [原則4] envはos_make_symbol("LABELS-ENV")の確保より**前**に保護すること。
    // 引数の評価順は未規定なので、シンボル確保でGCが走った時点でenvが古いまま
    // os_make_environmentへ渡りうる。
    // 実測(塗り潰し監査、environment_literal_slots_test): ここが漏れていたため
    // make_interpreted_function(eval.c:83)のGC_PROTECT(env)に既にstaleな値が入り、
    // 最終的にos_get_function内のcc_cdrが塗り潰し済みポインタを参照して #GP になった。
    GC_PROTECT(env);
    lisp_val_t bindings = cc_car(args);
    lisp_val_t body = cc_cdr(args);
    GC_PROTECT(bindings);
    GC_PROTECT(body);
    lisp_val_t new_env = os_make_frame(os_make_symbol("LABELS-ENV"), env);
    GC_PROTECT(new_env);

    for (lisp_val_t b = bindings; b != nil; b = cc_cdr(b)) {
        // b/nameはmake_interpreted_functionとos_set_functionの確保を跨ぐ。
        // 保護はループ**本体の中**で行う(forの初期化子から巻き上げるとスコープが
        // 変わり、過去に別件で沈黙ハングを起こしている)
        GC_PROTECT(b);
        lisp_val_t binding = cc_car(b);
        lisp_val_t name = cc_car(binding);
        GC_PROTECT(name);
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
    GC_PROTECT(params);
    GC_PROTECT(body);
    GC_PROTECT(env);
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
    GC_PROTECT(name);
    GC_PROTECT(env);

    // defunと同じく、捕捉環境(env)と登録先(owner)を分ける
    lisp_val_t owner = os_definition_env(env);
    GC_PROTECT(owner);
    lisp_val_t macro = make_macro(params, body, env);
    os_set_function(name, macro, owner);
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
    GC_PROTECT(params);
    GC_PROTECT(args);
    GC_PROTECT(body);
    lisp_val_t call_env = os_make_frame(os_make_symbol("MACRO-ENV"), closure_env);
    GC_PROTECT(call_env);
    bind_params(params, args, call_env);
    return eval_progn(body, call_env);
}

static lisp_val_t qq_expand(lisp_val_t form, lisp_val_t env, UINT64 depth);

/**
 * list(評価済み、unquote-splicingで得られたリスト)の要素をtailの手前に非破壊的に継ぎ足す。
 * za.c(JIT)がquasiquoteコンパイル時、list-position unquote-splicingの実行時fold処理で
 * os_make_consと同じ呼び出し規約(引数2個、両方GCリンク済みスロットから渡す)で呼ぶため
 * 非staticにしている。
 * @param list 継ぎ足す要素のリスト
 * @param tail listの末尾に続ける残りのリスト
 * @return listの要素 . tail
 */
lisp_val_t qq_append(lisp_val_t list, lisp_val_t tail) {
    if (list == nil) {
        return tail;
    }
    GC_PROTECT(list);
    GC_PROTECT(tail);
    return os_make_cons(cc_car(list), qq_append(cc_cdr(list), tail));
}

/** (sym x) の2要素リストを組み立てる(qq_expandがネストしたquasiquote/unquoteの
 * 形を保ったまま内側だけ展開して再構成するために使う)。symはg_sym_*(GCルート)。 */
static lisp_val_t qq_wrap(lisp_val_t sym, lisp_val_t inner) {
    GC_PROTECT(inner);
    lisp_val_t tail = os_make_cons(inner, nil);
    GC_PROTECT(tail);
    return os_make_cons(sym, tail);
}

/**
 * quasiquoteの本体(未評価のフォーム)を、unquote/unquote-splicingだけを評価しながら組み立てる。
 * ISLisp仕様§16.1: quasiquoteはネストでき、置換はネストレベルが同じunquoteだけに対して行う
 * (内側のquasiquoteで+1、unquoteで-1)。depth>0のunquote/unquote-splicingは評価せず、
 * その形のまま内側をdepth-1で展開して残す。
 * @param form 展開対象のフォーム(quasiquoteの直下、またはその再帰呼び出し)
 * @param env unquoteの評価に使う環境
 * @param depth 現在のネストレベル(最外のquasiquote直下が0)
 * @return 組み立てた値
 */
static lisp_val_t qq_expand(lisp_val_t form, lisp_val_t env, UINT64 depth) {
    if (form == nil || (form & TAG_MASK) != TAG_CONS) {
        return form; // atom/nilは評価せずそのまま
    }
    GC_PROTECT(form);
    GC_PROTECT(env);

    lisp_val_t elem = cc_car(form);
    GC_PROTECT(elem);

    if (elem == g_sym_quasiquote) {
        // ネストしたquasiquote: 形を保ったまま内側をdepth+1で展開する
        lisp_val_t inner = qq_expand(cc_car(cc_cdr(form)), env, depth + 1);
        return qq_wrap(g_sym_quasiquote, inner);
    }
    if (elem == g_sym_unquote || elem == g_sym_unquote_splicing) {
        // (unquote x): depth 0ならxを評価してその場に差し込む。リストの要素位置以外
        // (先頭やドット対の末尾)でのunquote-splicingはunquoteと同様に扱う。
        // depth>0なら形を保ち、内側をdepth-1で展開する
        if (depth == 0) {
            return os_eval(cc_car(cc_cdr(form)), env);
        }
        lisp_val_t inner = qq_expand(cc_car(cc_cdr(form)), env, depth - 1);
        return qq_wrap(elem, inner);
    }
    if ((elem & TAG_MASK) == TAG_CONS && cc_car(elem) == g_sym_unquote_splicing) {
        if (depth == 0) {
            // リストの要素が(unquote-splicing x): xを評価し、その要素を残りに継ぎ足す
            lisp_val_t spliced = os_eval(cc_car(cc_cdr(elem)), env);
            GC_PROTECT(spliced);
            lisp_val_t rest = qq_expand(cc_cdr(form), env, depth);
            GC_PROTECT(rest);
            return qq_append(spliced, rest);
        }
        lisp_val_t inner = qq_expand(cc_car(cc_cdr(elem)), env, depth - 1);
        GC_PROTECT(inner);
        lisp_val_t head = qq_wrap(g_sym_unquote_splicing, inner);
        GC_PROTECT(head);
        lisp_val_t tail = qq_expand(cc_cdr(form), env, depth);
        return os_make_cons(head, tail);
    }

    lisp_val_t head = qq_expand(elem, env, depth);
    GC_PROTECT(head);
    lisp_val_t tail = qq_expand(cc_cdr(form), env, depth);
    return os_make_cons(head, tail);
}

/**
 * quasiquote特殊形式。(quasiquote form)のformをunquote/unquote-splicing以外は未評価のまま組み立てて返す。
 * @param args (form)
 * @param env unquoteの評価に使う環境
 * @return 組み立てた値
 */
static lisp_val_t eval_quasiquote(lisp_val_t args, lisp_val_t env) {
    return qq_expand(cc_car(args), env, 0);
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
    GC_PROTECT(name);
    lisp_val_t body = cc_cdr(args);
    GC_PROTECT(body);
    GC_PROTECT(env);
    // ISLisp仕様§14.7: blockの動的extent(bodyの評価中)だけ、このblockの名前を
    // プロセスの「生きているblock」リストに積む。eval_return_fromが、既に抜けた
    // blockへの脱出(クロージャ越しに後から呼ぶ等)を<control-error>にするために見る
    lisp_val_t saved = os_live_block_push(name);
    GC_PROTECT(saved);
    lisp_val_t result = eval_progn(body, env);
    os_live_block_restore(saved);
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
    GC_PROTECT(name);
    lisp_val_t value_rest = cc_cdr(args);
    GC_PROTECT(env);
    lisp_val_t val = (value_rest != nil) ? os_eval(cc_car(value_rest), env) : nil;
    GC_PROTECT(val);
    if (is_control_transfer(val)) {
        return val;
    }
    // 宛先のblockが動的に生きていなければ<control-error>(ISLisp仕様§14.7の
    // (bar nil t)の例)。JITコンパイル済み関数内のblock(za_compile_block)も
    // os_live_block_pushで同じリストに登録するので、JIT関数内で作られたクロージャ
    // (インタプリタ実行)からのreturn-fromも正しく判定できる
    if (!os_live_block_p(name)) {
        return os_signal_control_error(env);
    }
    return os_make_instance(MAGIC_BLOCK_EXIT, name, val, nil);
}

/**
 * unwind-protect特殊形式。(unwind-protect protected-form cleanup-form...)のprotected-formを評価し、
 * その結果(通常値・脱出シグナルのいずれでも)に関わらずcleanup-formを必ず評価してから、
 * protected-formの評価結果を返す。
 * ISLisp仕様§14.7.2: cleanup-form内で新たな非局所脱出が起きた場合、protected-formが
 * 正常終了していればその脱出を伝播し、protected-form自身も脱出の途中(既に脱出先へ向かって
 * いる)なら<control-error>をsignalする。
 * @param args (protected-form . cleanup-form-rest)
 * @param env 評価に使う環境
 * @return protected-formの評価結果(通常値または脱出シグナル)、またはcleanupの脱出シグナル
 */
static lisp_val_t eval_unwind_protect(lisp_val_t args, lisp_val_t env) {
    lisp_val_t protected_form = cc_car(args);
    lisp_val_t cleanup_forms = cc_cdr(args);
    GC_PROTECT(cleanup_forms);
    GC_PROTECT(env);
    lisp_val_t result = os_eval(protected_form, env);
    GC_PROTECT(result);
    lisp_val_t cleanup_result = eval_progn(cleanup_forms, env);
    if (is_control_transfer(cleanup_result)) {
        if (is_control_transfer(result)) {
            return os_signal_control_error(env);
        }
        return cleanup_result;
    }
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
    GC_PROTECT(body);
    GC_PROTECT(env);
    lisp_val_t tag = os_eval(tag_form, env);
    if (is_control_transfer(tag)) {
        return tag;
    }
    GC_PROTECT(tag);
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
    GC_PROTECT(tag);
    lisp_val_t result = os_eval(result_form, env);
    GC_PROTECT(result);
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
    GC_PROTECT(args);
    GC_PROTECT(env);
    lisp_val_t pos = args;
    GC_PROTECT(pos);
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
    GC_PROTECT(tag);
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
        GC_PROTECT(args);
        GC_PROTECT(env);
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
        if (op == g_sym_declaim) {
            return eval_declaim(args, env);
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
                GC_PROTECT(expanded);
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
    // envはos_make_consの引数ではないため、os_make_consの内部保護の対象外である。
    // wrappedを組み立てる3回の確保はいずれもGCを誘発しうるので、envはここで
    // 自分で保護しなければならない。保護していないと、GCを跨いだ時点でenvが
    // 旧From空間を指したままos_evalへ渡り、os_eval側のGC_PROTECT(env)は
    // 「すでに古い値」を追跡するだけになって救えない
    // (documents/pitfalls.md 原則4。formは各os_make_consの引数なので内部保護される)
    GC_PROTECT(env);
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
/* [性能測定] Phase4: eval.hのstatic inlineへ移した(判定内容はis_control_transferと同一) */

/**
 * 非局所脱出シグナル(TAG_INSTANCE、word1=magic)のmagic(MAGIC_BLOCK_EXIT等)を
 * 取り出す。トランスパイラが生成するCコード(lisp_compiled.c)がblock/
 * return-from/tagbody/goを実行時に判定するために公開する。
 * @param v os_is_control_transferがnon-zeroを返す値
 * @return magic定数
 */
UINT64 os_control_transfer_magic(lisp_val_t v) {
    UINT64 *obj = (UINT64 *)(v & ~TAG_MASK);
    return obj[0];
}

/**
 * 非局所脱出シグナルのword2(block/return-fromのname、またはgo/tagbodyのtag)を
 * 取り出す。
 * @param v os_is_control_transferがnon-zeroを返す値
 * @return name/tagのシンボル
 */
lisp_val_t os_control_transfer_name(lisp_val_t v) {
    UINT64 *obj = (UINT64 *)(v & ~TAG_MASK);
    return obj[1];
}

/**
 * 非局所脱出シグナルのword3(MAGIC_BLOCK_EXITのvalue)を取り出す。
 * @param v os_is_control_transferがnon-zeroを返す値
 * @return return-fromが返そうとしている値
 */
lisp_val_t os_control_transfer_value(lisp_val_t v) {
    UINT64 *obj = (UINT64 *)(v & ~TAG_MASK);
    return obj[2];
}
