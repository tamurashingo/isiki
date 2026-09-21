/* 逆アセンブルのシンボル解決(documents/disasm-symbols.md)。
   テーブルの実体は生成物(disasm_symtab.c)で、こちらは引く側。 */
#include "disasm_symtab.h"

/* ユニットテストのビルドには生成物をリンクしないため、空のテーブルを置く。
   引けないだけで、逆アセンブラ自体は動く。 */
#ifdef ISIKIOS_UNIT_TEST
const UINT64 g_disasm_link_image_base = 0;
const UINT64 g_disasm_sym_count = 0;
const UINT64 g_disasm_sym_addr[1] = { 0 };
const UINT32 g_disasm_sym_name_off[1] = { 0 };
const char g_disasm_sym_names[1] = { 0 };
#endif

extern char __ImageBase[];

/**
 * リンク時アドレスを実行時アドレスへ補正する差分。
 *
 * **UEFI はリンク時とは別の場所へロードする。** Phase 1 の実測では
 * QUEM_MEM=256M で 199MB、4096M で 3015MB と、起動ごとに変わる。
 * __ImageBase は実行時の実アドレスを指すので、リンク時 ImageBase との差が
 * そのまま全シンボルへの補正量になる。
 */
static UINT64 disasm_runtime_image_base(void) {
#ifdef ISIKIOS_UNIT_TEST
    return 0;
#else
    return (UINT64)(lisp_addr_t)(void *)__ImageBase;
#endif
}

const char *os_disasm_lookup_kernel_symbol(UINT64 runtime_addr, UINT64 *out_offset) {
    if (g_disasm_sym_count == 0) {
        return 0;
    }
    /* [補正] 実行時アドレスをリンク時アドレスへ戻す。
       **UEFI のロード先はリンク時より前にも後にもなりうる。** 実測では
       リンク時 ImageBase が 0x2446c0000、実行時は 0x2750000 台で、
       `実行時 - リンク時` を符号なしで計算すると巨大な値に化けた。
       ImageBase からの**オフセット**で考えれば符号に依存しない。 */
    UINT64 rt_base = disasm_runtime_image_base();
    if (runtime_addr < rt_base) {
        return 0;       /* イメージより手前 = 対象外 */
    }
    UINT64 link_addr = g_disasm_link_image_base + (runtime_addr - rt_base);

    /* [性能] 2691 件を線形に走査しない。アドレス昇順なので二分探索で、
       **link_addr 以下で最大のシンボル**を見つける(範囲マッチ)。 */
    if (link_addr < g_disasm_sym_addr[0]) {
        return 0;
    }
    UINT64 lo = 0;
    UINT64 hi = g_disasm_sym_count - 1;
    while (lo < hi) {
        UINT64 mid = lo + (hi - lo + 1) / 2;
        if (g_disasm_sym_addr[mid] <= link_addr) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }

    /* [誤ヒット対策] 最後のシンボルより後ろは、どこまでが関数か分からない。
       次のシンボルとの間に収まっているものだけを引く。最後の1つは
       **.text の終端が分からない**ので、オフセット 0 のときだけ認める。 */
    if (lo + 1 < g_disasm_sym_count) {
        if (link_addr >= g_disasm_sym_addr[lo + 1]) {
            return 0;
        }
    } else if (link_addr != g_disasm_sym_addr[lo]) {
        return 0;
    }

    if (out_offset != 0) {
        *out_offset = link_addr - g_disasm_sym_addr[lo];
    }
    return &g_disasm_sym_names[g_disasm_sym_name_off[lo]];
}
