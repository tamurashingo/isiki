#ifndef DISASM_SYMTAB_H
#define DISASM_SYMTAB_H

#include "runtime.h"

/* 逆アセンブルのシンボル解決テーブル(documents/disasm-symbols.md)。
   実体は tools/gen_disasm_symtab.sh が生成する src/c/disasm_symtab.c。
   **生成物は .gitignore に入れる。** */

/** リンク時の ImageBase。実行時アドレスへの補正に使う */
extern const UINT64 g_disasm_link_image_base;
/** シンボル数 */
extern const UINT64 g_disasm_sym_count;
/** リンク時アドレス(昇順) */
extern const UINT64 g_disasm_sym_addr[];
/** 名前プールへのオフセット */
extern const UINT32 g_disasm_sym_name_off[];
/** 名前プール(NUL 区切り) */
extern const char g_disasm_sym_names[];

/**
 * カーネル .text のアドレスをシンボル名 + オフセットへ解決する。
 * **範囲マッチ**で、シンボルの先頭でなくても `name+0x1c` の形で引ける。
 * @param runtime_addr 実行時アドレス
 * @param out_offset シンボル先頭からのバイト数(引けたときだけ書く)
 * @return シンボル名。引けなければ 0
 */
const char *os_disasm_lookup_kernel_symbol(UINT64 runtime_addr, UINT64 *out_offset);

#endif
