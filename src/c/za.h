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
 *         | (if expr expr expr?)  ※ test/then/elseは再帰的にexprを許容、elseは省略可
 *
 * 「+」「-」「*」のオペランドはleaf限定のままで、これらの中に直接ifを書くことはできない
 * (`(+ x (if y 1 2))`はコンパイル失敗、フォールバックする)。一方ifのtest/then/elseの
 * 位置には`(+ ...)`「-」「*」「<」「=」や入れ子のifを自由に書ける。exprを評価する
 * 位置(bodyそのもの、およびifのtest/then/else)では、その場でマクロ展開(fixpointまで)
 * を試みるため、`cond`/`and`等のdefmacroマクロも展開結果がこのグラマーに収まる限り
 * コンパイルできる(例: `and`はif木へ完全に展開されるためコンパイル可能。一方`let`/`or`/
 * `cond`はlambda即時呼び出しやprognへ展開されるため、現時点では未対応としてフォール
 * バックする)。paramsは末尾に「&rest 任意のシンボル」を伴っても良いが、そのrest引数名を
 * expr中で参照することはできない(参照した場合はコンパイル失敗)。
 * 対応できない形であれば何も書き込まずnilを返す(呼び出し側はインタプリタにフォールバックする)。
 * @param params 仮引数リスト(未評価のシンボルリスト。末尾に&rest 1個を許容)
 * @param body 関数本体(未評価のフォームのリスト)
 * @param env マクロ展開時にマクロ定義を解決する環境(defunの定義時環境)
 * @return コンパイル済み関数のMAGIC_FUNCTION_NATIVE INSTANCE、失敗時はnil
 */
lisp_val_t za_try_compile_defun(lisp_val_t params, lisp_val_t body, lisp_val_t env);

#endif /* _ZA_H_ */
