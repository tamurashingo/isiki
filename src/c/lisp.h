#ifndef _LISP_H_
#define _LISP_H_

#include "types.h"
/* [性能測定] Phase4: cc_car/cc_cdrのstatic inline化にTAG_MASKが要る。
   runtime.hはtypes.h/process.hしか取り込まず、process.hもlisp.hを取り込まないため
   循環にはならない */
#include "runtime.h"

/**
 * cons cell の car を返す。
 * @param obj cons cell
 * @return car の値
 */
/* [性能測定] Phase4: os_make_fixnum/os_is_control_transferと同様にstatic inline化を
   試みたが、za.cのJITがiz17-labels-with-let(labels本体内でletマクロを使う関数)を
   コンパイルしなくなる回帰が出たため据え置く(%%za-compiled-pがnilを返す)。
   za.cはcc_car/cc_cdrのアドレスをJITコードへ埋め込むが、同一性比較には使って
   おらず、機構は未解明のまま。再挑戦する場合はまずこの回帰の原因を特定すること
   (test/lisp/za_test_ext17.lispの11番目のケースで再現する) */
lisp_val_t cc_car(lisp_val_t obj);

/**
 * cons cell の cdr を返す。
 * @param obj cons cell
 * @return cdr の値
 */
lisp_val_t cc_cdr(lisp_val_t obj);

/**
 * cons cell の car を破壊的に書き換える。
 * @param obj 書き換え対象の cons cell
 * @param val 新しい car の値
 */
void cc_set_car(lisp_val_t obj, lisp_val_t val);

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
