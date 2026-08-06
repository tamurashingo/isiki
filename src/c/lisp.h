#ifndef _LISP_H_
#define _LISP_H_

#include "types.h"

/**
 * cons cell の car を返す。
 * @param obj cons cell
 * @return car の値
 */
lisp_val_t cc_car(lisp_val_t obj);

/**
 * cons cell の cdr を返す。
 * @param obj cons cell
 * @return cdr の値
 */
lisp_val_t cc_cdr(lisp_val_t obj);

/**
 * cons cell の cdr を破壊的に書き換える。
 * @param obj 書き換え対象の cons cell
 * @param val 新しい cdr の値
 */
void cc_set_cdr(lisp_val_t obj, lisp_val_t val);

/**
 * alist((key . val)のconsを次々つないだリスト)からsymに一致するペアを探す。
 * @param sym 検索するキー(symbolまたはfixnum)
 * @param alist 検索対象のalist
 * @return 見つかった(key . val)のペア。見つからなければnil
 */
lisp_val_t cc_assoc_eq(lisp_val_t sym, lisp_val_t alist);

#endif /* _LISP_H_ */
