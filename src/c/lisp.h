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
/* [性能測定] Phase5 第2部: cc_car/cc_cdr(生成コード中2,784箇所)のinline化は
   **見送る**。static inline化・実体を1つだけ残すinline化のいずれでも、JITが
   labels本体内にletを持つ関数をコンパイルしなくなる回帰が出る
   (test/lisp/za_test_ext17.lispの11番目、%%za-compiled-pがnil)。
   %%DIAG-ZA-BAIL-LINEで追跡したところ、断念箇所はza_rewrite_body_listの
   「formsがTAG_CONSでない」判定であり、コンパイル時に組み直している
   bodyリストが壊れていることを示す。同関数はos_make_cons(first, rest)を
   呼ぶがrestを保護しておらず(firstは保護済み)、documents/pitfalls.md原則4と
   同じクラスの漏れに見える。ただしGC_PROTECT(rest)を足すと別の形で壊れる
   (EVAL-ERROR)ため、za.cのコンパイル時アロケーション経路全体の監査が要る。
   その調査を経るまでinline化は行わない */
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
