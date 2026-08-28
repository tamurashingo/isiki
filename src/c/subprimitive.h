#ifndef _SUBPRIMITIVE_H_
#define _SUBPRIMITIVE_H_

#include "types.h"

/**
 * ハードウェア層の組み込み関数%%IN-8。第一引数のポート番号から1バイト読み込む。
 * @param args (port) portはFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return 読み込んだ値のFIXNUM
 */
lisp_val_t cc_in_8(lisp_val_t args, lisp_val_t env);

/**
 * ハードウェア層の組み込み関数%%OUT-8。第一引数のポート番号へ第二引数の1バイトを出力する。
 * @param args (port value) port/valueはFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return 出力した値のFIXNUM
 */
lisp_val_t cc_out_8(lisp_val_t args, lisp_val_t env);

/**
 * ハードウェア層の組み込み関数%%PEEK。第一引数のアドレスから1バイト読み込む。
 * @param args (addr) addrはFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return 読み込んだ値のFIXNUM
 */
lisp_val_t cc_peek(lisp_val_t args, lisp_val_t env);

/**
 * ハードウェア層の組み込み関数%%POKE。第一引数のアドレスへ第二引数の1バイトを書き込む。
 * @param args (addr value) addr/valueはFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return 書き込んだ値のFIXNUM
 */
lisp_val_t cc_poke(lisp_val_t args, lisp_val_t env);

/**
 * ハードウェア層の組み込み関数%%IN-16。第一引数のポート番号から2バイト読み込む。
 * @param args (port) portはFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return 読み込んだ値のFIXNUM
 */
lisp_val_t cc_in_16(lisp_val_t args, lisp_val_t env);

/**
 * ハードウェア層の組み込み関数%%OUT-16。第一引数のポート番号へ第二引数の2バイトを出力する。
 * @param args (port value) port/valueはFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return 出力した値のFIXNUM
 */
lisp_val_t cc_out_16(lisp_val_t args, lisp_val_t env);

/**
 * ビット演算の組み込み関数%%LOGAND。第一引数と第二引数のビットごとのANDを取る。
 * @param args (a b) a/bは非負のFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return a AND bのFIXNUM
 */
lisp_val_t cc_logand(lisp_val_t args, lisp_val_t env);

/**
 * ビット演算の組み込み関数%%LOGIOR。第一引数と第二引数のビットごとのORを取る。
 * @param args (a b) a/bは非負のFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return a OR bのFIXNUM
 */
lisp_val_t cc_logior(lisp_val_t args, lisp_val_t env);

/**
 * ビット演算の組み込み関数%%LOGXOR。第一引数と第二引数のビットごとのXORを取る。
 * @param args (a b) a/bは非負のFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return a XOR bのFIXNUM
 */
lisp_val_t cc_logxor(lisp_val_t args, lisp_val_t env);

/**
 * ビット演算の組み込み関数%%ASH。第一引数を第二引数の分だけシフトする。
 * @param args (a count) aは非負のFIXNUM、countは正なら左シフト・負なら右シフトの符号付きFIXNUM
 * @param env 呼び出し時の環境(未使用)
 * @return シフト後の値のFIXNUM
 */
lisp_val_t cc_ash(lisp_val_t args, lisp_val_t env);

/** %%IN-8/%%OUT-8/%%PEEK/%%POKE/%%IN-16/%%OUT-16/%%LOGAND/%%LOGIOR/%%LOGXOR/%%ASHをglobal_environmentに関数として登録する */
void os_register_subprimitives(void);

#endif /* _SUBPRIMITIVE_H_ */
