#ifndef _ZA_H_
#define _ZA_H_

#include "types.h"

/**
 * defunの仮引数paramsと本体bodyがぴったり1フォームで、そのフォームが次のグラマーに
 * マッチする場合に限り、x86-64機械語へコンパイルしてMAGIC_FUNCTION_NATIVEのINSTANCEを
 * 返す:
 *
 *   expr := fixnumリテラル
 *         | paramsの固定引数への参照(重複参照可)
 *         | (+ leaf leaf...)   ※ オペランド2個以上、各leafはfixnumリテラルか固定引数参照のみ
 *         | (- leaf)           ※ 単項マイナス(0-leaf)
 *         | (- leaf leaf...)   ※ オペランド2個以上、左からfold
 *         | (* leaf leaf...)   ※ オペランド2個以上
 *         | (< leaf leaf)      ※ ちょうど2オペランドのみ(3個以上の連鎖比較は非対応)
 *         | (= leaf leaf)      ※ 同上
 *         | (car leaf) | (cdr leaf)              ※ 非allocating(cc_car/cc_cdrを直接呼ぶ)
 *         | (atom leaf) | (null leaf)             ※ 非allocating
 *         | (eq leaf leaf)                        ※ 非allocating(ポインタ同一性比較)
 *         | (cons leaf leaf)                      ※ ヒープ確保を伴う(os_make_consを1回呼ぶ)
 *         | (lambda (params...) . body)  ※ ヒープ確保を伴う。単一レベルのクロージャのみ
 *           対応: 外側関数の固定引数(&restを除く)を呼ばれるたびに新規environmentへ
 *           コピーする。lambda本体自体はzaがコンパイルせず、常にインタプリタ経由で
 *           実行される
 *         | (if expr expr expr?)  ※ test/then/elseは再帰的にexprを許容、elseは省略可
 *         | (block name . body)  ※ nameはシンボルリテラル(未評価)。bodyはprogn同様に
 *           順に評価し最後の値を返す。bodyの評価中に(return-from name val?)相当の
 *           制御転送に遭遇し、そのタグがnameと一致すればvalを、しなければ制御転送を
 *           そのまま上位へ伝播する
 *         | (return-from name expr?)  ※ nameはシンボルリテラル(未評価)。対応するblockが
 *           同一JITコンパイル対象関数内に無くても機械語上は単純に制御転送値を返すだけ
 *           であり実行時にインタプリタ側のblockまで伝播する。ただしクロージャ
 *           ((lambda ...)本体)を越えて外側関数のblockへ戻るケースは非対応
 *         | (catch tag-expr . body)  ※ tagは実行時に評価する。bodyはprogn同様に順に
 *           評価し最後の値を返す。bodyの評価中に(throw tag2 val)相当の制御転送に
 *           遭遇し、tag2がtagとeqであればvalを、しなければ制御転送をそのまま上位へ
 *           伝播する
 *         | (throw tag-expr result-expr)  ※ tag/resultは実行時に評価する。対応する
 *           catchの有無に関わらず機械語上は制御転送値を作って返すだけであり、対応する
 *           catchが動的に見つからない場合の扱いはインタプリタに委ねる
 *         | (unwind-protect protected-expr . cleanup-body)  ※ protected-exprの結果が
 *           通常値・制御転送のいずれであってもcleanup-bodyを必ず実行し(その結果は
 *           捨てる)、protected-exprの結果をそのまま返す
 *         | (tagbody . body)  ※ bodyの各要素はシンボルリテラルのタグ、またはexpr。
 *           タグ以外の要素を順に評価し、(go tag)相当の制御転送でそのtagbody自身が
 *           持つタグへ飛んだ場合はコンパイル時に解決した位置へ直接ジャンプする
 *           (実行時に制御転送値は生成しない)。他のblock/catch/goの制御転送、または
 *           自身の持たないタグへのgoはそのまま上位へ伝播する。bodyを最後まで評価し
 *           終えたら常にnilを返す。1つのJITコンパイル対象関数内でtagbodyが入れ子に
 *           なるケース(ネストしたtagbody)は非対応でコンパイル全体を断念する
 *         | (go tag)  ※ tagはシンボルリテラル(未評価)。同一tagbody内の当該タグへ
 *           コンパイル時に直接ジャンプする。tagbodyの外、または対応するcatch/throw/
 *           unwind-protectのスパンを飛び越えてジャンプすることになる位置に書かれた
 *           場合は非対応でコンパイル断念する
 *         | (fn-sym callarg callarg...)  ※ 一般呼び出し。fn-symはquote・if・+・-・*・<・=や
 *           progn/setq/defun/lambda/defmacro/quasiquote/block/return-from/
 *           unwind-protect/function/flet/labels/defvar/defconstant/defdynamic/
 *           defglobal/dynamic/catch/throw/tagbody/go以外の任意のシンボル。callargは
 *           leaf/(+ ...)/(- ...)/(* ...)/(< ...)/(= ...)/ifのみ(呼び出しをネストして
 *           直接書くことはできない)。実行時にfn-symをos_get_functionで解決するため、
 *           自己再帰・相互再帰・定義順序に依存しない他関数呼び出しのいずれも対応する。
 *
 * 「+」「-」「*」のオペランド、および一般呼び出しの引数(callarg)はleaf/算術/比較/if
 * 限定のままで、これらの中に直接別の一般呼び出しを書くことはできない
 * (`(+ x (if y 1 2))`はコンパイル可能だが`(+ x (foo y))`や`(foo (bar y))`は
 * コンパイル失敗、フォールバックする)。一方ifのtest/then/elseの位置には`(+ ...)`
 * 「-」「*」「<」「=」・一般呼び出し・入れ子のifを自由に書ける。exprを評価する
 * 位置(bodyそのもの、およびifのtest/then/else)では、その場でマクロ展開(fixpointまで)
 * を試みるため、`cond`/`and`等のdefmacroマクロも展開結果がこのグラマーに収まる限り
 * コンパイルできる(例: `and`はif木へ完全に展開されるためコンパイル可能。一方`let`/`or`/
 * `cond`はlambda即時呼び出しやprognへ展開されるため、現時点では未対応としてフォール
 * バックする)。bodyそのもの、およびifのthen/else(ifが末尾位置にある場合)に書かれた
 * 一般呼び出しは末尾呼び出しとなり、呼び出し先が`MAGIC_FUNCTION_NATIVE`であれば
 * トランポリンを介した末尾呼び出し最適化(TCO)が働き、深い自己再帰・相互再帰でも
 * Cスタックを消費しない(呼び出し先がインタプリタ実行の関数の場合のみCスタックを
 * 1段消費するが、これは元々の呼び出しと同じで悪化はしない)。paramsは末尾に
 * 「&rest 任意のシンボル」を伴っても良いが、そのrest引数名をexpr中で参照することは
 * できない(参照した場合はコンパイル失敗)。
 * 対応できない形であれば何も書き込まずnilを返す(呼び出し側はインタプリタにフォールバックする)。
 *
 * [重要] envは2つに分かれている。取り違えるとテストは通るのに自由変数だけが
 * 静かに壊れるので注意すること。
 * @param params 仮引数リスト(未評価のシンボルリスト。末尾に&rest 1個を許容)
 * @param body 関数本体(未評価のフォームのリスト)
 * @param capture_env **捕捉環境**。コンパイル時のマクロ展開・呼び出し先の解決に使い、
 *   さらに生成した関数オブジェクトのword3に入る。実行時に本体の自由変数を
 *   os_get_variableが辿る起点になるので、**呼び出し時のレキシカルなenv
 *   (frameでよい)をそのまま渡すこと**。ここにowner_envを渡すと
 *   `(let ((x 1)) (defun f () x))` のfがxを見られなくなる
 * @param owner_env **定義の登録先**(os_definition_envが解決したenvironment)。
 *   確保したImmobilized Pageとリテラルスロットの所有者として登録する。
 *   frameを渡すとdestroy-environmentの回収対象にならず、ページが永久に残る
 * @return コンパイル済み関数のMAGIC_FUNCTION_NATIVE INSTANCE、失敗時はnil
 */
lisp_val_t za_try_compile_defun(lisp_val_t params, lisp_val_t body,
                                lisp_val_t capture_env, lisp_val_t owner_env,
                                UINT64 optimize);

/**
 * za.c実装のネイティブ関数(%%DESTROY-ENVIRONMENT-RECLAIM、documents/environment.md
 * Phase4)をglobal_environmentへ登録する。os_bootstrap()等の他のos_register_*関数と
 * 同じタイミングでkernel_mainのブート列から呼ぶ。
 */
void os_register_za_primitives(void);

/**
 * [境界] JITリテラルスロット4本の配列先頭がOS_HEAP_ALIGN境界にあることを確かめ、
 * 外れていればos_panicする。スロットのアドレスはTAG_RAW_POINTER付きのLisp値として
 * 流通するため、ここが崩れると「Lisp値はすべて16byte境界」の不変条件が破れる。
 * 要素サイズ側は za.c の _Static_assert が保証するので、先頭だけ見ればよい。
 * ブート時に1回呼ぶ(kernel_main)。
 */
void za_assert_slot_alignment(void);

#endif /* _ZA_H_ */
