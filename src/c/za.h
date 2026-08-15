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
 * @param params 仮引数リスト(未評価のシンボルリスト。末尾に&rest 1個を許容)
 * @param body 関数本体(未評価のフォームのリスト)
 * @param env マクロ展開時にマクロ定義を解決する環境(defunの定義時環境)
 * @return コンパイル済み関数のMAGIC_FUNCTION_NATIVE INSTANCE、失敗時はnil
 */
lisp_val_t za_try_compile_defun(lisp_val_t params, lisp_val_t body, lisp_val_t env);

#endif /* _ZA_H_ */
