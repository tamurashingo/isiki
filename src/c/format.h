#ifndef _FORMAT_H_
#define _FORMAT_H_

#include "types.h"

/**
 * 組み込み関数FORMAT。format-stringの内容を解釈しながらoutput-streamへ出力する。
 * 対応する指示子: ~A ~B ~C ~D ~G ~O ~nR ~S ~nT ~X ~% ~& ~~
 * @param args (output-stream format-string . objs)
 * @param env 呼び出し時の環境(未使用)
 * @return nil
 */
lisp_val_t cc_format(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数FORMAT-CHAR。charをそのままoutput-streamへ書き込む。
 * @param args (output-stream char)
 * @param env 呼び出し時の環境(未使用)
 * @return nil
 */
lisp_val_t cc_format_char(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数FORMAT-FLOAT。floatをoutput-streamへ書き込む。
 * floatは未実装のため、fixnumの値をそのまま「整数+".0"」として出力する近似実装。
 * @param args (output-stream float)
 * @param env 呼び出し時の環境(未使用)
 * @return nil
 */
lisp_val_t cc_format_float(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数FORMAT-FRESH-LINE。output-streamが行頭でなければ改行を1つ出力する
 * (os_stream_t.columnで行頭判定する)。
 * @param args (output-stream)
 * @param env 呼び出し時の環境(未使用)
 * @return nil
 */
lisp_val_t cc_format_fresh_line(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数FORMAT-INTEGER。integerをradix進数でoutput-streamへ書き込む。
 * bignum(MAGIC_BIGNUM)はradix=10のみ対応(既存のprint機構を再利用)、
 * それ以外のradixでのbignum出力は未対応。
 * @param args (output-stream integer radix)
 * @param env 呼び出し時の環境(未使用)
 * @return nil
 */
lisp_val_t cc_format_integer(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数FORMAT-OBJECT。objをoutput-streamへ書き込む。
 * escape-pが真ならprin1相当(~S)、偽ならprinc相当(~A)。
 * @param args (output-stream obj escape-p)
 * @param env 呼び出し時の環境(未使用)
 * @return nil
 */
lisp_val_t cc_format_object(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数FORMAT-TAB。output-streamの現在の桁位置(column)がcolumn未満なら
 * columnに達するまで空白を出力し、既にcolumn以上なら空白を1つだけ出力する。
 * @param args (output-stream column)
 * @param env 呼び出し時の環境(未使用)
 * @return nil
 */
lisp_val_t cc_format_tab(lisp_val_t args, lisp_val_t env);

/**
 * FORMAT/FORMAT-CHAR/FORMAT-FLOAT/FORMAT-FRESH-LINE/FORMAT-INTEGER/FORMAT-OBJECT/
 * FORMAT-TABをglobal_environmentに登録する
 */
void os_register_format(void);

#endif /* _FORMAT_H_ */
