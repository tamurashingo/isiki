#ifndef _ZA_H_
#define _ZA_H_

#include "types.h"

/**
 * defunの仮引数paramsと本体bodyが「(+ operand operand)」ぴったり1フォームで、
 * 各operandがparamsのいずれか(重複参照可)またはfixnumリテラルである場合に限り、
 * x86-64機械語へコンパイルしてMAGIC_FUNCTION_NATIVEのINSTANCEを返す。
 * 対応できない形であれば何も書き込まずnilを返す(呼び出し側はインタプリタにフォールバックする)。
 * @param params 仮引数リスト(未評価のシンボルリスト)
 * @param body 関数本体(未評価のフォームのリスト)
 * @return コンパイル済み関数のMAGIC_FUNCTION_NATIVE INSTANCE、失敗時はnil
 */
lisp_val_t za_try_compile_defun(lisp_val_t params, lisp_val_t body);

#endif /* _ZA_H_ */
