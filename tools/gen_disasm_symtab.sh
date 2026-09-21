#!/bin/sh
# 逆アセンブルのシンボル解決テーブルを生成する(documents/disasm-symbols.md)。
#
#   gen_disasm_symtab.sh <nm出力> <ImageBase(16進)> > src/c/disasm_symtab.c
#
# **鶏と卵**: テーブルを含めて再リンクするとアドレスがずれうる。実測では
# .rdata が 80KB 増えても .text のシンボルは 1 つも動かなかった(.text が
# 最初のセクションで、テーブルは .rdata へ載るため)。それでも将来リンカの
# 挙動が変われば崩れるので、Makefile が**再リンク後に再抽出して照合**する。
set -eu
NM_OUT="$1"
IMAGE_BASE="$2"

# 関数シンボル(T/t)だけを採る。名前が ".text" のものは static 関数が節名で
# 出てしまったもので、区別できないため捨てる。.refptr.* のような補助シンボルも
# 関数ではないので落ちる(T/t でない)。
grep -E ' [Tt] ' "$NM_OUT" \
  | grep -v ' \.text$' \
  | sort -k1,1 \
  | awk '{ print $1, $3 }' > /tmp/_symtab_sorted.$$

COUNT=$(wc -l < /tmp/_symtab_sorted.$$)

printf '/* 自動生成。編集しない(tools/gen_disasm_symtab.sh が作る)。\n'
printf '   documents/disasm-symbols.md 参照。 */\n'
printf '#include "runtime.h"\n'
printf '#include "disasm_symtab.h"\n\n'

printf '/** リンク時の ImageBase。実行時は __ImageBase との差で補正する */\n'
printf 'const UINT64 g_disasm_link_image_base = 0x%sULL;\n\n' "$IMAGE_BASE"

printf '/** シンボル数 */\n'
printf 'const UINT64 g_disasm_sym_count = %s;\n\n' "$COUNT"

printf '/** リンク時アドレス(昇順)。二分探索で引く */\n'
printf 'const UINT64 g_disasm_sym_addr[] = {\n'
awk '{ printf "    0x%sULL,\n", $1 }' /tmp/_symtab_sorted.$$
printf '};\n\n'

printf '/** 名前プールへのオフセット(g_disasm_sym_addr と同じ順) */\n'
printf 'const UINT32 g_disasm_sym_name_off[] = {\n'
awk 'BEGIN { off = 0 }
     { printf "    %d,\n", off; off += length($2) + 1 }' /tmp/_symtab_sorted.$$
printf '};\n\n'

printf '/** 名前プール(NUL 区切り) */\n'
printf 'const char g_disasm_sym_names[] =\n'
awk '{ printf "    \"%s\\0\"\n", $2 }' /tmp/_symtab_sorted.$$
printf ';\n'

rm -f /tmp/_symtab_sorted.$$
