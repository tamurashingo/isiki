#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_assert.h"
#include "types.h"
#include "disasm.h"

/*
 * src/c/disasm.c のデコーダを、za.c が実際に出力するバイト列で検証する。
 *
 * 期待値はホストの objdump(-M intel)で確認した結果に合わせてある。
 * 「za.c のどのヘルパーが出すバイト列か」をテスト名に残しているので、
 * za.c 側のエンコーディングを変えたときにどのテストが対応するか辿れる。
 */

/** code[offset]を1項目デコードし、ニモニックとオペランドと長さをまとめて検査する */
static void expect_insn(const UINT8 *code, UINT64 code_len, UINT64 offset,
                        UINT64 want_len, const char *want_mnemonic, const char *want_operands,
                        const char *what) {
    os_disasm_insn_t insn;
    UINT64 n = os_disasm_item(code, code_len, offset, &insn);
    int ok = (n == want_len)
             && (insn.kind == OS_DISASM_INSN)
             && (strcmp(insn.mnemonic, want_mnemonic) == 0)
             && (strcmp(insn.operands, want_operands) == 0);
    if (!ok) {
        printf("  got: len=%llu kind=%d \"%s %s\" / want: len=%llu \"%s %s\"\n",
               (unsigned long long)n, (int)insn.kind, insn.mnemonic, insn.operands,
               (unsigned long long)want_len, want_mnemonic, want_operands);
    }
    assert(ok, what);
}

/* ---- Phase 1.2: 命令デコーディング ---- */

void test_decode_mov_reg_reg(void) {
    /* jit_mov_reg_reg / jit_mov_rcx_rax / jit_mov_r13_rax。
       ModRM.reg=src, ModRM.rm=dst なので Intel記法では "mov dst, src" になる */
    UINT8 mov_rcx_rax[] = { 0x48, 0x89, 0xC1 };
    expect_insn(mov_rcx_rax, sizeof(mov_rcx_rax), 0, 3, "mov", "rcx, rax", "48 89 c1 は mov rcx, rax");

    UINT8 mov_r13_rax[] = { 0x49, 0x89, 0xC5 };
    expect_insn(mov_r13_rax, sizeof(mov_r13_rax), 0, 3, "mov", "r13, rax", "REX.Bでdstがr13に拡張される");

    UINT8 mov_rdx_rax[] = { 0x48, 0x89, 0xC2 };
    expect_insn(mov_rdx_rax, sizeof(mov_rdx_rax), 0, 3, "mov", "rdx, rax", "48 89 c2 は mov rdx, rax");
}

void test_decode_alu_reg_reg(void) {
    /* jit_emit_reg_reg_op が出す6種(or/add/sub/cmp/test)。opcodeだけが違う */
    UINT8 or_r10_rdx[]  = { 0x49, 0x09, 0xD2 };
    UINT8 add_r10_rdx[] = { 0x49, 0x01, 0xD2 };
    UINT8 sub_r10_rdx[] = { 0x49, 0x29, 0xD2 };
    UINT8 cmp_rcx_rdx[] = { 0x48, 0x39, 0xD1 };
    UINT8 test_r10_r9[] = { 0x4D, 0x85, 0xCA };
    expect_insn(or_r10_rdx,  3, 0, 3, "or",   "r10, rdx", "opcode 0x09 は or");
    expect_insn(add_r10_rdx, 3, 0, 3, "add",  "r10, rdx", "opcode 0x01 は add");
    expect_insn(sub_r10_rdx, 3, 0, 3, "sub",  "r10, rdx", "opcode 0x29 は sub");
    expect_insn(cmp_rcx_rdx, 3, 0, 3, "cmp",  "rcx, rdx", "opcode 0x39 は cmp");
    expect_insn(test_r10_r9, 3, 0, 3, "test", "r10, r9",  "REX.R+REX.Bで両方が拡張レジスタになる");

    /* jit_cmp_rax_r11 (4C 39 D8) は「ModRM.reg=r11(src)、rm=rax(dst)」 */
    UINT8 cmp_rax_r11[] = { 0x4C, 0x39, 0xD8 };
    expect_insn(cmp_rax_r11, 3, 0, 3, "cmp", "rax, r11", "jit_cmp_rax_r11 は cmp rax, r11");
}

void test_decode_movabs(void) {
    /* jit_movabs_reg: REX.W + (B8+reg) + imm64 */
    UINT8 movabs_rax[] = { 0x48, 0xB8, 0xEF, 0xBE, 0xAD, 0xDE, 0x00, 0x00, 0x00, 0x00 };
    expect_insn(movabs_rax, sizeof(movabs_rax), 0, 10, "movabs", "rax, 0xdeadbeef",
                "jit_movabs_rax は10バイトでimm64を持つ");

    UINT8 movabs_r11[] = { 0x49, 0xBB, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    expect_insn(movabs_r11, sizeof(movabs_r11), 0, 10, "movabs", "r11, 0x1000",
                "jit_movabs_r11 はREX.Bでr11になる");

    /* 0埋めの即値も "0x0" として出る(即値が欠けていないことの確認) */
    UINT8 movabs_zero[] = { 0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0 };
    expect_insn(movabs_zero, sizeof(movabs_zero), 0, 10, "movabs", "r11, 0x0",
                "即値0でも10バイト消費する");
}

void test_decode_push_pop_ret(void) {
    UINT8 push_rbx[] = { 0x53 };
    UINT8 pop_rbx[]  = { 0x5B };
    UINT8 push_r13[] = { 0x41, 0x55 };
    UINT8 pop_r13[]  = { 0x41, 0x5D };
    UINT8 push_r14[] = { 0x41, 0x56 };
    UINT8 pop_r14[]  = { 0x41, 0x5E };
    UINT8 ret[]      = { 0xC3 };
    expect_insn(push_rbx, 1, 0, 1, "push", "rbx", "0x53 は push rbx");
    expect_insn(pop_rbx,  1, 0, 1, "pop",  "rbx", "0x5B は pop rbx");
    expect_insn(push_r13, 2, 0, 2, "push", "r13", "41 55 は push r13");
    expect_insn(pop_r13,  2, 0, 2, "pop",  "r13", "41 5D は pop r13");
    expect_insn(push_r14, 2, 0, 2, "push", "r14", "41 56 は push r14");
    expect_insn(pop_r14,  2, 0, 2, "pop",  "r14", "41 5E は pop r14");
    expect_insn(ret,      1, 0, 1, "ret",  "",    "0xC3 は ret");
}

void test_decode_rsp_frame_ops(void) {
    /* jit_sub_rsp_imm8 / jit_add_rsp_imm8 (call前後のshadow space) */
    UINT8 sub_rsp_20[] = { 0x48, 0x83, 0xEC, 0x20 };
    UINT8 add_rsp_20[] = { 0x48, 0x83, 0xC4, 0x20 };
    expect_insn(sub_rsp_20, 4, 0, 4, "sub", "rsp, 0x20", "48 83 EC ib は sub rsp, imm8");
    expect_insn(add_rsp_20, 4, 0, 4, "add", "rsp, 0x20", "48 83 C4 ib は add rsp, imm8");

    /* jit_sub_rsp_imm32 / jit_add_rsp_imm32 (フレーム確保) */
    UINT8 sub_rsp_1a8[] = { 0x48, 0x81, 0xEC, 0xA8, 0x01, 0x00, 0x00 };
    UINT8 add_rsp_1a8[] = { 0x48, 0x81, 0xC4, 0xA8, 0x01, 0x00, 0x00 };
    expect_insn(sub_rsp_1a8, 7, 0, 7, "sub", "rsp, 0x1a8", "48 81 EC id は sub rsp, imm32");
    expect_insn(add_rsp_1a8, 7, 0, 7, "add", "rsp, 0x1a8", "48 81 C4 id は add rsp, imm32");

    /* jit_and_reg_imm8: 0xF8 は符号拡張されて -8。~TAG_MASK のマスクである */
    UINT8 and_r10_f8[] = { 0x49, 0x83, 0xE2, 0xF8 };
    expect_insn(and_r10_f8, 4, 0, 4, "and", "r10, -0x8", "83 /4 ib の imm8 は符号拡張される");
}

void test_decode_rsp_disp32_slots(void) {
    /* jit_emit_rsp_disp32: mod=10, rm=100(SIB), SIB=0x24(base=rsp,index無し), disp32。
       フレーム上の値スロットの読み書きはすべてこの形になる */
    UINT8 store[] = { 0x48, 0x89, 0x8C, 0x24, 0x48, 0x00, 0x00, 0x00 };
    UINT8 load[]  = { 0x48, 0x8B, 0x84, 0x24, 0x48, 0x00, 0x00, 0x00 };
    UINT8 lea[]   = { 0x48, 0x8D, 0x8C, 0x24, 0x10, 0x00, 0x00, 0x00 };
    expect_insn(store, 8, 0, 8, "mov", "[rsp+0x48], rcx", "jit_mov_rsp_from_reg はストア");
    expect_insn(load,  8, 0, 8, "mov", "rax, [rsp+0x48]", "jit_mov_reg_from_rsp はロード");
    expect_insn(lea,   8, 0, 8, "lea", "rcx, [rsp+0x10]", "jit_lea_reg_rsp はアドレス計算");
}

void test_decode_mem_disp8(void) {
    /* jit_mov_reg_from_mem_disp8 / jit_mov_mem_disp8_from_reg。
       トランポリンが obj[0]/obj[1]/obj[3] を読むのに使う */
    UINT8 load0[] = { 0x4D, 0x8B, 0x5A, 0x00 };  /* mov r11, [r10+0x0] */
    UINT8 load8[] = { 0x4D, 0x8B, 0x5A, 0x08 };  /* mov r11, [r10+0x8] */
    UINT8 load24[] = { 0x49, 0x8B, 0x42, 0x18 }; /* mov rax, [r10+0x18] */
    UINT8 store8[] = { 0x49, 0x89, 0x42, 0x08 }; /* mov [r10+0x8], rax */
    expect_insn(load0, 4, 0, 4, "mov", "r11, [r10]", "disp8=0 は変位を出さない");
    expect_insn(load8, 4, 0, 4, "mov", "r11, [r10+0x8]", "meta->fixed_entry の読み出し");
    expect_insn(load24, 4, 0, 4, "mov", "rax, [r10+0x18]", "obj[3](word3)の読み出し");
    expect_insn(store8, 4, 0, 4, "mov", "[r10+0x8], rax", "disp8のストア方向");
}

void test_decode_indirect_call_and_jmp(void) {
    /* jit_call_r11 の本体(41 FF D3)と jit_jmp_reg(41 FF E3) */
    UINT8 call_r11[] = { 0x41, 0xFF, 0xD3 };
    UINT8 jmp_r11[]  = { 0x41, 0xFF, 0xE3 };
    expect_insn(call_r11, 3, 0, 3, "call", "r11", "FF /2 はレジスタ間接call");
    expect_insn(jmp_r11,  3, 0, 3, "jmp",  "r11", "FF /4 はレジスタ間接jmp");
}

void test_decode_branches_resolve_offsets(void) {
    /* 分岐先はコード先頭からのオフセットで表示する。
       je(0F 84) は6バイトなので、rel32=+3 なら着地点は 0x00 + 6 + 3 = 0x09 */
    UINT8 code[] = {
        0x0F, 0x84, 0x03, 0x00, 0x00, 0x00,  /* je 0x9 */
        0xE9, 0x03, 0x00, 0x00, 0x00,        /* jmp 0xe */
        0x0F, 0x85, 0x00, 0x00, 0x00, 0x00,  /* jne 0x11 (直後) */
        0x0F, 0x88, 0x00, 0x00, 0x00, 0x00,  /* js  0x17 */
        0x0F, 0x82, 0x00, 0x00, 0x00, 0x00   /* jb  0x1d */
    };
    expect_insn(code, sizeof(code), 0,  6, "je",  "0x9",  "0F 84 rel32 の着地点を解決する");
    expect_insn(code, sizeof(code), 6,  5, "jmp", "0xe",  "E9 rel32 の着地点を解決する");
    expect_insn(code, sizeof(code), 11, 6, "jne", "0x11", "0F 85 は jne");
    expect_insn(code, sizeof(code), 17, 6, "js",  "0x17", "0F 88 は js");
    expect_insn(code, sizeof(code), 23, 6, "jb",  "0x1d", "0F 82 は jb");
}

void test_decode_branch_outside_code_is_marked(void) {
    /* 末尾呼び出しの共有トランポリンはImmobilized Spaceへコピーされず g_jit_code に
       残るため、そこへの jmp はコード範囲の外を指す。オフセットとして表示すると
       嘘になるので、相対変位 + "outside" で示す */
    UINT8 code[] = { 0xE9, 0x00, 0x00, 0xFF, 0xFF };  /* jmp -0x10000 */
    os_disasm_insn_t insn;
    UINT64 n = os_disasm_item(code, sizeof(code), 0, &insn);
    assert(n == 5, "jmp rel32 は5バイト");
    assert(strcmp(insn.mnemonic, "jmp") == 0, "ニモニックはjmp");
    assert(strstr(insn.operands, "outside") != 0, "コード範囲外の飛び先はoutsideと注記する");
}

/* ---- 絶対アドレスの抽出(領域注釈の材料) ---- */

void test_movabs_exposes_immediate_as_target_addr(void) {
    /* 「movabs r11, 0x0c1d41aa が何を指しているのか読めない」というのが
       この機能の出発点。デコーダはその値をtarget_addrとして取り出すところまでを担う
       (領域の判定はランタイム側のos_classify_addr) */
    UINT8 code[] = { 0x49, 0xBB, 0xAA, 0x41, 0x1D, 0x0C, 0x00, 0x00, 0x00, 0x00 };
    os_disasm_insn_t insn;
    os_disasm_item(code, sizeof(code), 0, &insn);
    assert(insn.has_target_addr == 1, "movabs は絶対アドレスを持つ");
    assert(insn.target_addr == 0x0C1D41AAULL, "imm64 がそのまま target_addr になる");
    assert(insn.comment[0] == 0, "デコーダは comment を空のままにする");
}

void test_in_block_branch_has_no_target_addr(void) {
    /* ブロック内へ飛ぶ分岐は注釈の対象にしない。飛び先は定義上この関数自身と
       同じ領域にあり、注釈しても情報が増えないまま全行が伸びるだけになる */
    UINT8 code[] = {
        0x0F, 0x84, 0x03, 0x00, 0x00, 0x00,  /* je 0x9 (ブロック内) */
        0xE9, 0x00, 0x00, 0x00, 0x00,        /* jmp 0xb (ブロック内) */
        0xC3
    };
    os_disasm_insn_t insn;
    os_disasm_item(code, sizeof(code), 0, &insn);
    assert(insn.has_target_addr == 0, "ブロック内へのjccは絶対アドレスを持たない");
    os_disasm_item(code, sizeof(code), 6, &insn);
    assert(insn.has_target_addr == 0, "ブロック内へのjmpは絶対アドレスを持たない");
}

void test_outside_branch_exposes_absolute_target(void) {
    /* 末尾呼び出しの共有トランポリンへのjmpはコードブロックの外を指す。
       オフセットでは表せないので、絶対アドレスを立てて領域を引けるようにする */
    UINT8 code[16];
    for (unsigned i = 0; i < sizeof(code); i++) { code[i] = 0x90; }
    code[0] = 0xE9;
    code[1] = 0x00; code[2] = 0x10; code[3] = 0x00; code[4] = 0x00; /* rel32 = +0x1000 */
    os_disasm_insn_t insn;
    os_disasm_item(code, sizeof(code), 0, &insn);
    assert(insn.has_target_addr == 1, "ブロック外へのjmpは絶対アドレスを持つ");
    assert(insn.target_addr == (UINT64)(lisp_addr_t)code + 5 + 0x1000,
           "絶対アドレスは「次の命令の先頭 + rel32」");
}

void test_rip_relative_exposes_effective_address(void) {
    /* mov rax, [rip+0x10]。実効アドレスは「**次の命令の先頭** + disp」なので、
       命令長が確定してから計算する必要がある(ModRMを読んだ時点では分からない) */
    UINT8 code[] = { 0x48, 0x8B, 0x05, 0x10, 0x00, 0x00, 0x00, 0xC3 };
    os_disasm_insn_t insn;
    UINT64 n = os_disasm_item(code, sizeof(code), 0, &insn);
    assert(n == 7, "RIP相対の mov は7バイト");
    assert(strcmp(insn.operands, "rax, [rip+0x10]") == 0, "RIP相対として表示される");
    assert(insn.has_target_addr == 1, "RIP相対は絶対アドレスを持つ");
    assert(insn.target_addr == (UINT64)(lisp_addr_t)code + 7 + 0x10,
           "実効アドレスは命令の**末尾**からの相対(先頭からではない)");
}

void test_plain_instruction_has_no_target_addr(void) {
    UINT8 code[] = { 0x48, 0x89, 0xC1 };
    os_disasm_insn_t insn;
    os_disasm_item(code, sizeof(code), 0, &insn);
    assert(insn.has_target_addr == 0, "mov reg,reg は絶対アドレスを持たない");
}

void test_format_line_appends_comment(void) {
    /* comment はランタイム側が埋める。埋まっていれば行末へ ; <...> として付き、
       空なら何も出ない(引けなかったことを示す表示は出さない) */
    UINT8 code[] = { 0x49, 0xBB, 0xAA, 0x41, 0x1D, 0x0C, 0x00, 0x00, 0x00, 0x00 };
    os_disasm_insn_t insn;
    char line[192];

    os_disasm_item(code, sizeof(code), 0, &insn);
    os_disasm_format_line(&insn, line, sizeof(line));
    assert(strstr(line, ";") == 0, "comment が空なら注釈は出ない");

    strcpy(insn.comment, "<immobilized>");
    os_disasm_format_line(&insn, line, sizeof(line));
    assert(strstr(line, "movabs r11, 0xc1d41aa") != 0, "命令部分はそのまま");
    assert(strstr(line, "; <immobilized>") != 0, "注釈が行末に付く");
    /* operands へ連結していないこと(disassemble-to-list の利用側がパースせずに済む) */
    assert(strstr(insn.operands, "immobilized") == 0, "注釈は operands に混ざらない");
}

/* ---- コードに埋め込まれた文字列(za_emit_symbol_name) ---- */

void test_embedded_string_is_decoded_as_data(void) {
    /* jmp rel32 で "CAR\0" を飛び越える形。素朴なlinear sweepは 'C'(0x43)を
       REX.XB プレフィックスとして読んでしまい、以降が全部ずれる */
    UINT8 code[] = {
        0xE9, 0x04, 0x00, 0x00, 0x00,   /* jmp +4 */
        'C', 'A', 'R', 0x00,
        0x48, 0x89, 0xC1                /* mov rcx, rax */
    };
    os_disasm_insn_t insn;

    UINT64 n = os_disasm_item(code, sizeof(code), 0, &insn);
    assert(n == 5, "先頭の jmp rel32 は5バイト");

    n = os_disasm_item(code, sizeof(code), 5, &insn);
    assert(n == 4, ".asciz は NUL込みの4バイトを1項目にまとめる");
    assert(insn.kind == OS_DISASM_DATA, "種別が OS_DISASM_DATA になる");
    assert(strcmp(insn.mnemonic, ".asciz") == 0, "ニモニックは .asciz");
    assert(strcmp(insn.operands, "\"CAR\"") == 0, "オペランドに文字列の中身が出る");

    /* 文字列を飛ばした後、後続の命令が正しくデコードできる(ずれていない) */
    expect_insn(code, sizeof(code), 9, 3, "mov", "rcx, rax", "文字列の直後から復帰できる");
}

void test_jmp_over_code_is_not_mistaken_for_string(void) {
    /* ifのthen節末尾にある「else節を飛び越すjmp」。飛び越す中身は機械語なので、
       表示可能ASCII+NULという条件を満たさず .asciz にはならない */
    UINT8 code[] = {
        0xE9, 0x03, 0x00, 0x00, 0x00,   /* jmp +3 */
        0x48, 0x89, 0xC1,               /* mov rcx, rax (飛び越される側) */
        0xC3                            /* ret */
    };
    expect_insn(code, sizeof(code), 5, 3, "mov", "rcx, rax",
                "飛び越される機械語は .asciz と誤認されない");
}

void test_string_heuristic_requires_nul_and_printable(void) {
    /* NUL終端でない(条件4を満たさない) */
    UINT8 no_nul[] = { 0xE9, 0x03, 0x00, 0x00, 0x00, 'A', 'B', 'C', 0xC3 };
    os_disasm_insn_t insn;
    os_disasm_item(no_nul, sizeof(no_nul), 5, &insn);
    assert(insn.kind != OS_DISASM_DATA, "NUL終端でなければ .asciz にしない");

    /* 表示可能ASCIIでないバイトを含む(条件3を満たさない) */
    UINT8 not_ascii[] = { 0xE9, 0x03, 0x00, 0x00, 0x00, 'A', 0x01, 0x00, 0xC3 };
    os_disasm_item(not_ascii, sizeof(not_ascii), 5, &insn);
    assert(insn.kind != OS_DISASM_DATA, "表示可能ASCIIでないバイトを含めば .asciz にしない");

    /* 直前が jmp rel32 でない(条件1を満たさない) */
    UINT8 no_jmp[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 'A', 'B', 0x00, 0xC3 };
    os_disasm_item(no_jmp, sizeof(no_jmp), 5, &insn);
    assert(insn.kind != OS_DISASM_DATA, "直前が jmp rel32 でなければ .asciz にしない");
}

/* ---- エラーハンドリング・境界 ---- */

void test_offset_past_end_returns_zero(void) {
    UINT8 code[] = { 0xC3 };
    os_disasm_insn_t insn;
    assert(os_disasm_item(code, sizeof(code), 1, &insn) == 0, "コード末尾以降は0を返す(ループの終端)");
    assert(os_disasm_item(code, sizeof(code), 99, &insn) == 0, "遥か後方のオフセットでも0を返す");
    assert(os_disasm_item(code, 0, 0, &insn) == 0, "長さ0のコードは0を返す");
    assert(os_disasm_item(0, 16, 0, &insn) == 0, "NULLポインタは0を返す");
}

void test_undecodable_byte_advances_one(void) {
    /* 未対応opcodeで止まると以降の行が全部失われるので、1バイトだけ進めて再開できる
       ようにしてある。0x06 は64bitモードでは無効な opcode */
    UINT8 code[] = { 0x06, 0xC3 };
    os_disasm_insn_t insn;
    UINT64 n = os_disasm_item(code, sizeof(code), 0, &insn);
    assert(n == 1, "デコードできないバイトは1バイトとして扱う");
    assert(insn.kind == OS_DISASM_BAD, "種別が OS_DISASM_BAD になる");
    assert(strcmp(insn.mnemonic, "(bad)") == 0, "ニモニックは (bad)");
    expect_insn(code, sizeof(code), 1, 1, "ret", "", "次のバイトから復帰できる");
}

void test_truncated_instruction_does_not_read_past_end(void) {
    /* movabs のimm64が途中で切れている。コード範囲の外を読まずに (bad) で止まること */
    UINT8 code[] = { 0x48, 0xB8, 0x01, 0x02 };
    os_disasm_insn_t insn;
    UINT64 n = os_disasm_item(code, sizeof(code), 0, &insn);
    assert(n == 1, "切り詰められた命令は (bad) として1バイト進む");
    assert(insn.kind == OS_DISASM_BAD, "範囲外を読まずに OS_DISASM_BAD になる");
}

/* ---- Phase 1.3: 整形 ---- */

void test_format_line_layout(void) {
    UINT8 code[] = { 0x48, 0x89, 0xC1 };
    os_disasm_insn_t insn;
    char line[192];
    os_disasm_item(code, sizeof(code), 0, &insn);
    os_disasm_format_line(&insn, line, sizeof(line));
    assert(strncmp(line, "0000  48 89 c1 ", 15) == 0, "オフセット4桁とバイト列が先頭に出る");
    assert(strstr(line, "mov rcx, rax") != 0, "命令はIntel記法で出る");
}

void test_format_line_does_not_truncate_large_offsets(void) {
    /* オフセット欄は4桁揃えだが、64KBを超える位置では桁を増やして全体を出す。
       黙って上位桁を落とすと、存在しない位置を指す行になってしまう */
    os_disasm_insn_t insn;
    char line[192];
    memset(&insn, 0, sizeof(insn));  /* comment等を含め全フィールドを確定させる */
    insn.kind = OS_DISASM_INSN;
    insn.offset = 0x12345;
    insn.length = 1;
    insn.bytes[0] = 0xC3;
    strcpy(insn.mnemonic, "ret");
    insn.operands[0] = 0;
    os_disasm_format_line(&insn, line, sizeof(line));
    assert(strncmp(line, "12345  c3 ", 10) == 0, "4桁に収まらないオフセットは桁を増やして出す");
}

void test_format_line_truncates_into_small_buffer(void) {
    /* 出力先が足りなくても、書ける範囲まで書いて必ずNUL終端する */
    UINT8 code[] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0 };
    os_disasm_insn_t insn;
    char small[8];
    memset(small, 'X', sizeof(small));
    os_disasm_item(code, sizeof(code), 0, &insn);
    UINT64 n = os_disasm_format_line(&insn, small, sizeof(small));
    assert(n < sizeof(small), "書き込んだ長さはバッファサイズ未満");
    assert(small[sizeof(small) - 1] == 0 || small[n] == 0, "必ずNUL終端される");
}

/* ---- 実際のプロローグ列を通しで読む ---- */

void test_sweep_over_jit_prologue(void) {
    /* za_try_compile_defun が出すプロローグそのもの(push rbx / push r13 /
       sub rsp, ZA_FRAME_TOTAL / スロットへの退避 / za_gc_current_head の呼び出し) */
    UINT8 code[] = {
        0x53,                                             /* push rbx */
        0x41, 0x55,                                       /* push r13 */
        0x48, 0x81, 0xEC, 0xA8, 0x01, 0x00, 0x00,         /* sub rsp, 0x1a8 */
        0x48, 0x89, 0x8C, 0x24, 0x48, 0x00, 0x00, 0x00,   /* mov [rsp+0x48], rcx */
        0x49, 0xBB, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* movabs r11, 0x1000 */
        0x48, 0x83, 0xEC, 0x20,                           /* sub rsp, 0x20 */
        0x41, 0xFF, 0xD3,                                 /* call r11 */
        0x48, 0x83, 0xC4, 0x20,                           /* add rsp, 0x20 */
        0xC3                                              /* ret */
    };
    UINT64 offset = 0;
    UINT64 count = 0;
    os_disasm_insn_t insn;
    while (offset < sizeof(code)) {
        UINT64 n = os_disasm_item(code, sizeof(code), offset, &insn);
        if (n == 0) {
            break;
        }
        assert(insn.kind == OS_DISASM_INSN, "プロローグは全て命令としてデコードできる");
        offset += n;
        count++;
    }
    assert(offset == sizeof(code), "命令長の合計がコード長とちょうど一致する");
    assert(count == 9, "9命令に分解される");
}

/* ---- 越境読み出しの検出(valgrind 併用) ---- */

/**
 * srcの先頭lenバイトを「ちょうどlenバイトのヒープ領域」へコピーしてから走査する。
 * 静的配列やスタックに置いたままだと、バッファの直後にも有効なメモリが続くため
 * **範囲外読み出しをvalgrindが検出できない**。正確なサイズで確保すれば、
 * 1バイトでも踏み越えた時点で invalid read として報告される。
 * @return 走査で進んだ合計バイト数(=len でなければ途中で止まっている)
 */
static UINT64 sweep_on_exact_heap_buffer(const UINT8 *src, UINT64 len) {
    UINT8 *buf = (UINT8 *)malloc(len);
    if (buf == 0) {
        return 0;
    }
    memcpy(buf, src, len);

    os_disasm_insn_t insn;
    char line[192];
    UINT64 offset = 0;
    while (offset < len) {
        UINT64 n = os_disasm_item(buf, len, offset, &insn);
        if (n == 0) {
            break;
        }
        os_disasm_format_line(&insn, line, sizeof(line));
        offset += n;
    }
    free(buf);
    return offset;
}

void test_sweep_never_reads_past_the_buffer(void) {
    /* za.c が実際に出す並び。これを 1..sizeof(code) の**すべての長さ**で切り詰めて
       走査する。どの切り口でも必ず命令の途中でバッファが終わる位置が現れるので、
       命令長の計算が1バイトでも先を読めば valgrind が捕まえる。 */
    static const UINT8 code[] = {
        0x53, 0x41, 0x55,
        0x48, 0x81, 0xEC, 0x38, 0x1D, 0x00, 0x00,
        0x4C, 0x89, 0xB4, 0x24, 0x00, 0x1D, 0x00, 0x00,
        0x49, 0xBB, 0x0B, 0x74, 0x79, 0x0C, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x83, 0xEC, 0x20,
        0x41, 0xFF, 0xD3,
        0x48, 0x83, 0xC4, 0x20,
        0x4C, 0x39, 0xD8,
        0x0F, 0x84, 0x03, 0x00, 0x00, 0x00,
        0xE9, 0x04, 0x00, 0x00, 0x00,
        'C', 'A', 'R', 0x00,
        0x48, 0x8D, 0x8C, 0x24, 0x10, 0x00, 0x00, 0x00,
        0x49, 0x83, 0xE2, 0xF8,
        0x4D, 0x8B, 0x5A, 0x08,
        0x41, 0xFF, 0xE3,
        0x41, 0x5D, 0x5B, 0xC3
    };
    int all_covered = 1;
    for (UINT64 len = 1; len <= sizeof(code); len++) {
        /* (bad) は1バイト進むので、走査は必ずバッファ全体を覆い切る */
        if (sweep_on_exact_heap_buffer(code, len) != len) {
            all_covered = 0;
        }
    }
    assert(all_covered, "どの長さに切り詰めても走査が過不足なくバッファ全体を覆う");
}

void test_embedded_string_detection_stays_in_bounds(void) {
    /* 埋め込み文字列の判定は直前5バイトを遡って読む。オフセットが小さいときや
       文字列がバッファ末尾で切れているときに、前後どちらへも踏み越えないこと */
    static const UINT8 code[] = {
        0xE9, 0x20, 0x00, 0x00, 0x00,   /* rel32 が残りバイト数より大きい */
        'A', 'B', 'C', 0x00
    };
    for (UINT64 len = 1; len <= sizeof(code); len++) {
        UINT8 *buf = (UINT8 *)malloc(len);
        memcpy(buf, code, len);
        os_disasm_insn_t insn;
        for (UINT64 off = 0; off < len; off++) {
            os_disasm_item(buf, len, off, &insn);
        }
        free(buf);
    }
    assert(1, "埋め込み文字列の判定がバッファの前後を踏み越えない(valgrindで確認)");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    test_decode_mov_reg_reg();
    test_decode_alu_reg_reg();
    test_decode_movabs();
    test_decode_push_pop_ret();
    test_decode_rsp_frame_ops();
    test_decode_rsp_disp32_slots();
    test_decode_mem_disp8();
    test_decode_indirect_call_and_jmp();
    test_decode_branches_resolve_offsets();
    test_decode_branch_outside_code_is_marked();
    test_movabs_exposes_immediate_as_target_addr();
    test_in_block_branch_has_no_target_addr();
    test_outside_branch_exposes_absolute_target();
    test_rip_relative_exposes_effective_address();
    test_plain_instruction_has_no_target_addr();
    test_format_line_appends_comment();
    test_embedded_string_is_decoded_as_data();
    test_jmp_over_code_is_not_mistaken_for_string();
    test_string_heuristic_requires_nul_and_printable();
    test_offset_past_end_returns_zero();
    test_undecodable_byte_advances_one();
    test_truncated_instruction_does_not_read_past_end();
    test_format_line_layout();
    test_format_line_does_not_truncate_large_offsets();
    test_format_line_truncates_into_small_buffer();
    test_sweep_over_jit_prologue();

    test_sweep_never_reads_past_the_buffer();
    test_embedded_string_detection_stays_in_bounds();

    return g_test_failed ? 1 : 0;
}
