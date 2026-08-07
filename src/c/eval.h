#ifndef _EVAL_H_
#define _EVAL_H_

#include "types.h"
#include "runtime.h"

/**
 * exp を env のもとで評価する。
 * SYMBOLはenvから値をlookupし、CONSはcarを関数、cdrを引数として評価する。
 * それ以外(FIXNUM/STRING/CHAR/INSTANCEなど)は自己評価する。
 * @param exp 評価対象のS式
 * @param env 評価に使う環境
 * @return 評価結果
 */
lisp_val_t os_eval(lisp_val_t exp, lisp_val_t env);

/**
 * 組み込み関数MACROEXPAND-1。formの先頭がマクロとして定義されたsymbolなら1段だけ展開して返し、
 * そうでなければformをそのまま返す。
 * @param args 評価済みの引数リスト(第一引数がform)
 * @param env マクロ定義を解決する環境
 * @return 1段展開した結果、またはマクロでなければform自身
 */
lisp_val_t primitive_macroexpand_1(lisp_val_t args, lisp_val_t env);

/**
 * eval.cで実装した組み込み関数(macroexpand-1)をglobal_environmentに登録する。
 */
void os_register_eval_primitives(void);

#endif /* _EVAL_H_ */
