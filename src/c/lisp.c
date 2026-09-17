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
    GC_DEBUG_ASSERT_LIVE(obj, "cc_car");
    // [GC監査] **読んだ結果**がトラップなら、objは塗り潰し済み領域のcons cellを
    // 指していた。最初にstaleを読んだ場所を押さえられるのはこの形だけである。
    // ポインタ側を判定しても、staleポインタ自体は「ふつうのアドレス」に見えるので
    // 素通りし、トラップは1段あとの読み出しで初めて現れる(実測でeval_argsが
    // 最初の検出箇所に見えていたのはこのため。真の起点はos_eval_top_levelだった)。
    // **例外ハンドラでは代替できない。** 塗り潰した領域は読んでもフォルトせず、
    // ただトラップの値が返るだけだからである(アンマップ方式にすれば不要になる)
    lisp_val_t v = ((lisp_val_t *)(obj & ~TAG_MASK))[0];
    GC_DEBUG_TRAP_RESULT(v, "cc_car");
    return v;
}

lisp_val_t cc_cdr(lisp_val_t obj) {
    // TODO: TAG_CONS であることのチェックを入れる
    GC_DEBUG_ASSERT_LIVE(obj, "cc_cdr");
    lisp_val_t v = ((lisp_val_t *)(obj & ~TAG_MASK))[1];
    GC_DEBUG_TRAP_RESULT(v, "cc_cdr");
    return v;
}

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
void cc_set_car(lisp_val_t obj, lisp_val_t val) {
    // TODO: TAG_CONS であることのチェックを入れる
    ((lisp_val_t *)(obj & ~TAG_MASK))[0] = val;
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
        else if (k & TAG_FIXNUM && (k >> FIXNUM_VALUE_SHIFT) == (key >> FIXNUM_VALUE_SHIFT)) {
            return pair;
        }
        current = cc_cdr(current); // cdr -> next
    }
    return nil;
}

