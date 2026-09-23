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
 * os_eval_top_levelの、「打ち切られたかどうか」を呼び出し元へ返す版。
 *
 * **戻り値だけでは区別が付かない。** `(error "x")` が打ち切られた場合も、
 * フォームがたまたまconditionオブジェクトを値として返した場合も、
 * os_eval_top_levelの戻り値はどちらも<error>のインスタンスになる。
 * environmentの巻き戻し(打ち切られたときだけ戻す)と表示の切り替え
 * (report-conditionを使うかどうか)は、この区別が要る。
 *
 * 打ち切りとみなすのは**%abort-top-levelによる脱出だけ**である。すなわち
 * MAGIC_BLOCK_EXIT かつ宛先が %TOP-LEVEL のもの。eval_blockと同じく
 * 宛先名だけを見る経路(throw/goのタグがたまたま%TOP-LEVELだった場合)は
 * 値としては従来どおり捕捉するが、打ち切りとしては数えない。
 *
 * @param form 評価対象のトップレベルフォーム
 * @param env 評価に使う環境
 * @param out_aborted %abort-top-levelで打ち切られた場合に1を格納する先(NULL可)
 * @return formの評価結果。abortされた場合はabortに渡されたcondition
 */
lisp_val_t os_eval_top_level_ex(lisp_val_t form, lisp_val_t env, int *out_aborted);

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
/* [性能測定] Phase4: 生成コード中に7,210箇所ある最頻の呼び出し。中身はタグ判定と
   magic比較だけで、実測19.5命令のうち大半が呼び出しオーバーヘッドだった
   (get_current_process/os_make_fixnumと同じ理由)。static inlineへ移す。
   判定内容はeval.cのis_control_transferと同一で、意味論は変えていない */
static inline int os_is_control_transfer(lisp_val_t v) {
    if ((v & TAG_MASK) != TAG_INSTANCE) {
        return 0;
    }
    UINT64 *obj = (UINT64 *)(v & ~TAG_MASK);
    return obj[0] == MAGIC_BLOCK_EXIT || obj[0] == MAGIC_CATCH_EXIT || obj[0] == MAGIC_GO_EXIT;
}

/**
 * 非局所脱出シグナルのmagic(MAGIC_BLOCK_EXIT等、runtime.h)を取り出す。
 * os_is_control_transferがnon-zeroを返す値にのみ呼んでよい。
 */
UINT64 os_control_transfer_magic(lisp_val_t v);

/**
 * 非局所脱出シグナルのword2(block/return-fromのname、またはgo/tagbodyのtag)を
 * 取り出す。os_is_control_transferがnon-zeroを返す値にのみ呼んでよい。
 */
lisp_val_t os_control_transfer_name(lisp_val_t v);

/**
 * 非局所脱出シグナルのword3(MAGIC_BLOCK_EXITのvalue)を取り出す。
 * os_is_control_transferがnon-zeroを返す値にのみ呼んでよい。
 */
lisp_val_t os_control_transfer_value(lisp_val_t v);

#endif /* _EVAL_H_ */
