#ifndef _DISASM_H_
#define _DISASM_H_

#include "types.h"

/**
 * x86-64 逆アセンブラ(JIT生成コード用)。
 *
 * 対象は src/c/za.c の jit_* ヘルパーが出力しうるエンコーディングで、選定の経緯と
 * サポート範囲は documents/disasm-backend-decision.md に書いてある。
 * libc も動的ライブラリも無いフリースタンディング環境で動かすため、確保を一切行わず
 * 「呼び出し側が用意した構造体へ1項目ぶん書き込む」形のAPIにしてある。
 */

/** x86-64の命令長の上限(プレフィックス込み) */
#define OS_DISASM_MAX_INSN_BYTES 15
/** os_disasm_insn_t.text に入る文字列の上限(NUL含む) */
#define OS_DISASM_TEXT_SIZE 96
/** 埋め込み文字列(.asciz)として1項目にまとめる最大バイト数 */
#define OS_DISASM_MAX_DATA_BYTES 256

/** 1項目の種別 */
typedef enum {
    /** 命令としてデコードできた */
    OS_DISASM_INSN = 0,
    /** 命令ではなく、コード中に埋め込まれたNUL終端文字列(za_emit_symbol_name) */
    OS_DISASM_DATA = 1,
    /** デコードできなかった。length は必ず1で、呼び出し側は1バイト進めて再開できる */
    OS_DISASM_BAD  = 2
} os_disasm_kind_t;

/**
 * 逆アセンブルした1項目。すべて生データ(lisp_val_tを含まない)なので、
 * GCの保護対象にはならない(documents/pitfalls.md 原則7/原則8)。
 */
typedef struct {
    /** OS_DISASM_INSN / OS_DISASM_DATA / OS_DISASM_BAD */
    os_disasm_kind_t kind;
    /** コード先頭からのバイトオフセット */
    UINT64 offset;
    /** この項目のバイト数(1〜15。OS_DISASM_DATAのみOS_DISASM_MAX_DATA_BYTESまで) */
    UINT64 length;
    /** 命令バイト列。OS_DISASM_DATAでlengthが15を超える場合は先頭15バイトのみ */
    UINT8 bytes[OS_DISASM_MAX_INSN_BYTES];
    /** ニモニック("mov"、"jmp"、".asciz"、"(bad)") */
    char mnemonic[16];
    /** オペランド(Intel記法、dst, srcの順)。無い場合は空文字列 */
    char operands[OS_DISASM_TEXT_SIZE];
    /**
     * この命令が指す絶対アドレスを持つなら1。対象は
     *  - movabs の imm64
     *  - コードブロックの外へ飛ぶ call/jmp/jcc rel32 の解決先
     *  - RIP相対のアドレッシングの実効アドレス
     * の3つ。imm8/disp8は値域が狭く誤ヒットするので対象にしない。
     *
     * ブロック**内**へ飛ぶ分岐は対象外である。飛び先は定義上この関数自身と同じ
     * 領域にあり、注釈しても情報が増えないまま全行が伸びるだけになる。
     * 飛び先はoperandsにオフセットとして出ているので、そちらで足りる。
     */
    int has_target_addr;
    /** has_target_addrが1のときの絶対アドレス */
    UINT64 target_addr;
    /**
     * 表示用の注釈("<immobilized>"等)。空文字列なら何も表示しない。
     *
     * デコーダはここを**常に空文字列にする**。アドレスの所属領域を知るには
     * ランタイム側の境界(os_classify_addr)が要り、デコーダをランタイムから
     * 独立に保つためにここでは埋めない。埋めるのは src/c/disasm_lisp.c 側で、
     * os_disasm_format_line は埋まっていれば行末へ付ける。
     *
     * operandsへ連結しないのは、disassemble-to-list の利用側が
     * オペランドと注釈を分離するためにパースする羽目になるため。
     */
    char comment[64];
} os_disasm_insn_t;

/**
 * code[offset] から1項目(命令または埋め込み文字列)をデコードする。
 *
 * 埋め込み文字列の判定はステートレスで、直前5バイトが `jmp rel32` でその飛び先が
 * 「表示可能ASCII列 + NUL」の直後に一致する場合にのみ OS_DISASM_DATA を返す
 * (判定条件の詳細は documents/disasm-backend-decision.md)。
 *
 * @param code コード先頭
 * @param code_len コードのバイト長
 * @param offset デコード開始位置(code先頭からのオフセット)
 * @param out デコード結果の書き込み先(NULL不可)
 * @return この項目のバイト数。offsetがcode_len以上なら0(その場合outは未変更)
 */
UINT64 os_disasm_item(const UINT8 *code, UINT64 code_len, UINT64 offset, os_disasm_insn_t *out);

/**
 * 1項目を1行のテキストへ整形する(末尾に改行は付けない)。
 * 書式: `OFFSET  BYTES...  MNEMONIC OPERANDS`
 *   0028  48 8b 4c 24 48        mov rcx, [rsp+0x48]
 * @param insn os_disasm_itemが埋めた項目
 * @param buf 書き込み先
 * @param buf_size bufのバイト数(NUL終端を含む)
 * @return 書き込んだ文字数(NULを含まない)。bufには常にNUL終端が入る
 */
UINT64 os_disasm_format_line(const os_disasm_insn_t *insn, char *buf, UINT64 buf_size);

/**
 * 1項目のバイト列を "48 89 c1" 形式の16進テキストへ整形する。
 * os_disasm_format_lineが作る行にも同じ列が含まれるが、Lisp側で
 * バイト列だけを別に扱えるよう単独でも取り出せるようにしてある。
 * 15バイトを超える項目(埋め込み文字列)は先頭15バイトまでで " .." を付ける。
 * @param insn os_disasm_itemが埋めた項目
 * @param buf 書き込み先
 * @param buf_size bufのバイト数(NUL終端を含む)
 * @return 書き込んだ文字数(NULを含まない)
 */
UINT64 os_disasm_format_bytes(const os_disasm_insn_t *insn, char *buf, UINT64 buf_size);

#endif /* _DISASM_H_ */
