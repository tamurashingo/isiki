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
 * formを(block %TOP-LEVEL form)相当としてenvのもとで評価する。トップレベルの
 * ドライバ(REPL/load)がこの関数を通すことで、途中でcatchされなかった
 * (%abort-top-level経由の)非局所脱出をこの1フォームの評価だけに閉じ込め、
 * 生の脱出シグナル(TAG_INSTANCE)がドライバやprintまで漏れるのを防ぐ。
 * @param form 評価対象のトップレベルフォーム
 * @param env 評価に使う環境
 * @return formの評価結果。abortされた場合はabortに渡されたcondition
 */
lisp_val_t os_eval_top_level(lisp_val_t form, lisp_val_t env);

/**
 * 組み込み関数MACROEXPAND-1。formの先頭がマクロとして定義されたsymbolなら1段だけ展開して返し、
 * そうでなければformをそのまま返す。
 * @param args 評価済みの引数リスト(第一引数がform)
 * @param env マクロ定義を解決する環境
 * @return 1段展開した結果、またはマクロでなければform自身
 */
lisp_val_t primitive_macroexpand_1(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数FUNCALL。第一引数の関数オブジェクトを、残りの評価済み引数で呼び出す。
 * mapcar等、関数を値として受け取り呼び出す高階関数がLisp側から呼ぶために使う
 * (Lisp2スコープのため、変数に束縛された関数オブジェクトは(f x)のようには呼べない)。
 * @param args 評価済みの引数リスト(第一引数は関数オブジェクト、残りはその実引数)
 * @param env 呼び出し時の環境
 * @return 関数呼び出しの結果
 */
lisp_val_t primitive_funcall(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%APPLY。第一引数の関数オブジェクトを、第二引数(評価済みの引数の
 * リスト)を展開して呼び出す。FUNCALLは呼び出し側の構文上の引数個数が固定のため、
 * 実行時に長さが決まるリスト(総称関数dispatchでのメソッド呼び出しなど)を展開して
 * 渡すことができない。%%APPLYはその用途のための内部primitiveで、仕様上のapply
 * (先頭に固定引数を並べられる)とは異なり(fn arg-list)の2引数のみを受け付ける。
 * @param args 評価済みの引数リスト(第一引数は関数オブジェクト、第二引数は実引数のリスト)
 * @param env 呼び出し時の環境
 * @return 関数呼び出しの結果
 */
lisp_val_t primitive_apply(lisp_val_t args, lisp_val_t env);

/**
 * eval.cで実装した組み込み関数(macroexpand-1, funcall, %%apply)をglobal_environmentに登録する。
 */
void os_register_eval_primitives(void);

/**
 * ネイティブ関数/インタプリタ関数(TAG_INSTANCE, MAGIC_FUNCTION_NATIVE/MAGIC_FUNCTION_INTERPRETED)を
 * 評価済み引数で呼び出す。runtime.c/reader.cのCプリミティブがLisp側の関数(make-instance/
 * signal-condition等)を呼び戻すために使う。
 * @param fn 呼び出す関数オブジェクト
 * @param evaluated_args 評価済みの引数リスト
 * @param env 呼び出し時の環境
 * @return 関数呼び出しの結果。関数オブジェクトでない場合はg_sym_eval_error
 */
lisp_val_t os_apply_function(lisp_val_t fn, lisp_val_t evaluated_args, lisp_val_t env);

/**
 * list(評価済み、unquote-splicingで得られたリスト)の要素をtailの手前に非破壊的に継ぎ足す。
 * za.c(JIT)がquasiquoteコンパイル時、list-position unquote-splicingの実行時fold処理で
 * os_make_consと同じ呼び出し規約(引数2個、両方GCリンク済みスロットから渡す)で呼ぶ。
 * @param list 継ぎ足す要素のリスト
 * @param tail listの末尾に続ける残りのリスト
 * @return listの要素 . tail
 */
lisp_val_t qq_append(lisp_val_t list, lisp_val_t tail);

/**
 * vがblock/return-from/unwind-protect/catch/throw/tagbody/goの非局所脱出シグナルかどうかを判定する。
 * os_apply_function経由でLisp側の関数を呼んだ結果、非局所脱出やsignal-conditionの伝播が
 * 起きていないかをCプリミティブ側でチェックするために使う。
 * @param v 判定対象の値
 * @return 非局所脱出シグナルならnon-zero
 */
int os_is_control_transfer(lisp_val_t v);

#endif /* _EVAL_H_ */
