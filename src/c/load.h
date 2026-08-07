#ifndef _LOAD_H_
#define _LOAD_H_

#include "types.h"

/**
 * 組み込み関数LOAD。第一引数のパスの9Pファイルをstreamで開き、
 * S式をos_read_streamで読めなくなるまで順にos_evalする。
 * 構文エラー・I/Oエラー・open失敗時は呼び出し元プロセスのstdout_bufferへ
 * エラー内容を表示し、g_sym_eval_errorを返す。
 * @param args (path) pathはSTRING(9Pエクスポートルートから見た相対パス)
 * @param env 評価に使う環境(読み込んだ各S式もこのenvでos_evalされる)
 * @return 成功時g_sym_t、失敗時g_sym_eval_error
 */
lisp_val_t cc_load(lisp_val_t args, lisp_val_t env);

/** LOADをglobal_environmentに関数として登録する */
void os_register_load(void);

#endif /* _LOAD_H_ */
