// lisp の primitive な関数を定義する

#include "types.h"
#include "runtime.h"
#include "lisp.h"


/**
 * cons cell の car を返す。
 * @param obj cons cell
 * @return car の値
 */
lisp_val_t cc_car(lisp_val_t obj) {
    // TODO: TAG_CONS であることのチェックを入れる
    return ((lisp_val_t *)(obj & ~TAG_MASK))[0];
}

/**
 * cons cell の cdr を返す。
 * @param obj cons cell
 * @return cdr の値
 */
lisp_val_t cc_cdr(lisp_val_t obj) {
    // TODO: TAG_CONS であることのチェックを入れる
    return ((lisp_val_t *)(obj & ~TAG_MASK))[1];
}

/**
 * cons cell の cdr を破壊的に書き換える。
 * @param obj 書き換え対象の cons cell
 * @param val 新しい cdr の値
 */
void cc_set_cdr(lisp_val_t obj, lisp_val_t val) {
    // TODO: TAG_CONS であることのチェックを入れる
    ((lisp_val_t *)(obj & ~TAG_MASK))[1] = val;
}

/**
 * alist((key . val)のconsを次々つないだリスト)からkに一致するペアを探す。
 * @param k 検索するキー(symbolまたはfixnum)
 * @param alist 検索対象のalist
 * @return 見つかった(key . val)のペア。見つからなければnil
 */
lisp_val_t cc_assoc_eq(lisp_val_t k, lisp_val_t alist) {
    // TODO: alist の TAG_CONS チェック
    lisp_val_t current = alist;
    while (current != nil) {
        // current は ((key . val) . next)
        lisp_val_t pair = cc_car(current); // car -> (key . val)
        lisp_val_t key = cc_car(pair); // car of pair -> key

        // symbol
        if (k == key) {
            return pair;
        }
        // fixnum
        else if (k & TAG_FIXNUM && (k >> 3) == (key >> 3)) {
            return pair;
        }
        current = cc_cdr(current); // cdr -> next
    }
    return nil;
}

