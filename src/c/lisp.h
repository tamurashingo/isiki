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
/* [性能測定] Phase5: cc_car/cc_cdr(生成コード中2,784箇所)のinline化は引き続き
   見送る。za_rewrite_*系のenv未保護(本コミットで修正)は真の潜在バグだったが、
   それを直してもinline化すると別の箇所(za_compile_body_forms経由、za.c:4703で
   伝播)で断念しEVAL-ERRORになる。同型の保護漏れがza.cのコンパイル時経路に
   まだ残っていることを示す。%%DIAG-ZA-BAIL-LINEは失敗を表すreturn 0だけを
   捉えており、正常終了と区別できないreturn nilの経路は追えないため、
   za.cのコンパイル時アロケーション経路全体の監査が必要 */
lisp_val_t cc_car(lisp_val_t obj);
lisp_val_t cc_cdr(lisp_val_t obj);

/**
 * cons cell の cdr を返す。
 * @param obj cons cell
 * @return cdr の値
 */

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
