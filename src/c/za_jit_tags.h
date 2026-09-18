#ifndef _ZA_JIT_TAGS_H_
#define _ZA_JIT_TAGS_H_

#include "types.h"
#include "runtime.h"

/* ============================================================================
 * JITが機械語へ埋め込む「タグに由来する即値」を、C側の定義から導出する。
 *
 * documents/jit-tag-constants.md。
 *
 * なぜ要るか: JITは jit_emit8/32/64 で生のバイト列を組み立てる。生バイト列の中では
 * 0x07 や 0xF8 は ModRM・REX・レジスタ番号としても普通に現れるので、grep では
 * 「タグの定数」と「命令のエンコーディング」を見分けられない。実際 ~TAG_MASK を
 * 表す 0xF8 が9箇所へ独立に直書きされており、TAG_MASK を変えても追随しない状態
 * だった(documents/alignment-survey-report.md 7.1 が同じ形の危険として記録している)。
 *
 * ここを唯一の導出元にすれば、C側の TAG_MASK を変えたときにJITも一緒に動く。
 *
 * **値が同じでも概念が違うものは、同じマクロにまとめないこと。**
 * 例えば fixnum のシフト量(FIXNUM_VALUE_SHIFT)は現在たまたまタグ幅と同じ3だが、
 * ポインタのタグ幅だけを広げる案がありうる以上、別の定数として扱う。
 * ========================================================================= */

/**
 * `and r/m64, imm8`(opcode 0x83 /4)でタグビットだけを残すためのマスク。
 * 「タグの値を取り出す」用途。
 */
#define JIT_IMM8_TAG_MASK    ((UINT8)(TAG_MASK))

/**
 * `and r/m64, imm8` でタグビットを落として実アドレスにするためのマスク。
 *
 * この命令の imm8 は**64bitへ符号拡張される**ので、0xF8 を書くと
 * 0xFFFFFFFFFFFFFFF8 として効く。つまり ~TAG_MASK の下位8bitを置けばよい。
 * 下の _Static_assert が、符号拡張した結果が本当に ~TAG_MASK と一致することを
 * 確かめている(タグ幅を変えたときに黙って壊れないようにするため)。
 */
#define JIT_IMM8_UNTAG_MASK  ((UINT8)(~(UINT64)(TAG_MASK) & 0xFFULL))

/* imm8 は符号付き8bit。タグマスクがこの幅に収まらなくなったら、
   and r/m64, imm8 では表現できないので命令形式から変える必要がある */
_Static_assert(TAG_MASK <= 0x7F,
               "TAG_MASK does not fit in a signed imm8: 'and r/m64, imm8' can no longer encode it");

/* 符号拡張した結果が ~TAG_MASK と一致すること。
   ここが崩れると、生成コードは「タグを落としたつもりで別のビットを落とす」 */
_Static_assert((INT64)(INT8)JIT_IMM8_UNTAG_MASK == (INT64)(~(UINT64)(TAG_MASK)),
               "JIT_IMM8_UNTAG_MASK does not sign-extend to ~TAG_MASK");

/* タグマスク側も imm8 として素直に(符号拡張しても値が変わらずに)置けること */
_Static_assert((INT64)(INT8)JIT_IMM8_TAG_MASK == (INT64)(UINT64)(TAG_MASK),
               "JIT_IMM8_TAG_MASK does not sign-extend to TAG_MASK");

#endif /* _ZA_JIT_TAGS_H_ */
