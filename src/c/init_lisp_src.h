#ifndef _INIT_LISP_SRC_H_
#define _INIT_LISP_SRC_H_

#include "types.h"

/**
 * カーネルへ同梱した Lisp ソース(バイト配列)。
 *
 * 実体は **生成物** `src/c/init_lisp_src.c`(git 管理対象外)で、
 * `tools/gen_embedded_lisp.py` が `src/lisp/*.lisp` から作る。
 * 同梱対象の一覧はその生成器が唯一の真実源で、Makefile は
 * `--print-inputs` で依存を受け取る。
 *
 * **この手書きヘッダを生成物が #include している。** 名前や型が食い違えば
 * コンパイルが落ちるので、宣言と定義がずれたまま通ることはない。
 *
 * 長さは配列の `sizeof` から導出される(規則 13)。
 * 「配列の要素数 == 入力の wc -c」はコンパイル時の `_Static_assert` と
 * ホスト側の `make check-embedded-lisp-src` の両方で突き合わせる
 * (前者は生成物の内部矛盾、後者は生成物の古さを捕まえる)。
 *
 * **const なのは、文字列入力ストリームが配列を読むだけだからである**
 * (`stream.c:427-432` が `str_buf` を読む。書くのは文字列**出力**の
 * `stream.c:516-517` だけ)。`os_stream_open_string_input` の第 2 引数が
 * `UINT8 *` なので、渡すときは `(UINT8 *)` のキャストが要る。
 */

/** 基盤ライブラリ src/lisp/init.lisp のソース(NUL 終端しない) */
extern const UINT8 g_init_lisp_src[];
/** g_init_lisp_src のバイト数 */
extern const UINT32 g_init_lisp_src_len;

/** 補助ライブラリ src/lisp/disassemble.lisp のソース(NUL 終端しない)。
 *  設計判断 L2 で同梱と決めた。**読むのは M1-T3 以降** */
extern const UINT8 g_disassemble_lisp_src[];
/** g_disassemble_lisp_src のバイト数 */
extern const UINT32 g_disassemble_lisp_src_len;

#endif /* _INIT_LISP_SRC_H_ */
