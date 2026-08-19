#include "types.h"
#include "lisp.h"
#include "runtime.h"
#include "eval.h"
#include "za.h"

#if defined(__x86_64__)

/* let-IIFEインライン化(拡張B)およびprogn対応により、以前はインタプリタへfallback
 * していたlet、let-star、or、cond等の多くがJIT対象になった分、プロセス全体で累積する
 * コード量が増えた。旧65536だと長いテストスイート(ext5+ext7+ext8)の途中でg_jit_usedが
 * 上限に達しoverflowで以降の全コンパイルがfallbackしてしまうため拡張した。 */
#define JIT_CODE_SIZE 524288
/** サポートする仮引数(&restより手前の固定引数)の最大個数(コード量とコンパイル時間を有限に保つための上限) */
#define ZA_MAX_PARAMS 16
/** サポートする+のオペランドの最大個数(同上) */
#define ZA_MAX_OPERANDS 16

static UINT8 g_jit_code[JIT_CODE_SIZE] __attribute__((aligned(16)));
static UINT64 g_jit_used = 0;
/** jit_emit8がg_jit_codeの残り容量を使い切ったことを示すフラグ。立った場合は
 * za_try_compile_defunがg_jit_usedをコンパイル開始前の位置まで巻き戻してnilを返す */
static int g_jit_overflow = 0;

/** 拡張9(setq): 今回のdefun内でza_compile_setqがlet-localへの書き込みに成功した
 * ことを示すフラグ。g_za_saw_escaping_lambdaと組み合わせて、setqとクロージャキャプチャの
 * 食い違いを検出するための粗い安全網に使う(za_try_compile_defun末尾で判定)。 */
static int g_za_saw_setq_local = 0;
/** 拡張9(setq): 今回のdefun内で値の位置に現れる裸の(lambda ...)(クロージャ生成、
 * let-IIFEインライン化のlambdaとは別の分岐)をコンパイルしたことを示すフラグ。
 * クロージャ生成時にlet-localの値を一度だけコピーする現在の実装(za_compile_lambda)は、
 * コピー後にそのlocalへsetqしてもクロージャ側には反映されない。インタプリタの環境モデル
 * (setqが既存consのcdrをその場で書き換える参照共有セマンティクス、runtime.cの
 * os_setq_variable)と食い違うため、同一defun内にsetqとエスケープするlambdaの両方が
 * 存在する場合は安全側に倒して全体をfallbackさせる(g_za_saw_setq_localとの併用、
 * za_try_compile_defun末尾参照)。 */
static int g_za_saw_escaping_lambda = 0;

/** 拡張(flet/labels): 今回のdefun内で`(function name)`がflet/labels束縛関数を
 * 指すケースをコンパイルしたことを示すフラグ。束縛関数のgensym登録は
 * flet/labels本体の動的extent内でのみglobal_environment上に存在するため、
 * `(function name)`でクロージャを取り出してその外へ持ち出す(脱出させる)使い方は
 * 対応できない。g_za_saw_escaping_lambdaと同じ「粗い過大近似+無条件fallback」で
 * 安全側に倒す(za_try_compile_defun末尾参照)。 */
static int g_za_saw_flet_labels_escape = 0;

static void jit_emit8(UINT8 b) {
    if (g_jit_used >= JIT_CODE_SIZE) {
        g_jit_overflow = 1;
        return;
    }
    g_jit_code[g_jit_used++] = b;
}

static void jit_emit32(UINT32 v) {
    jit_emit8((UINT8)(v));
    jit_emit8((UINT8)(v >> 8));
    jit_emit8((UINT8)(v >> 16));
    jit_emit8((UINT8)(v >> 24));
}

static void jit_emit64(UINT64 v) {
    jit_emit32((UINT32)v);
    jit_emit32((UINT32)(v >> 32));
}

// 手書き用の命令
static void jit_movabs_rax(UINT64 imm) { jit_emit8(0x48); jit_emit8(0xB8); jit_emit64(imm); }
static void jit_movabs_r11(UINT64 imm) { jit_emit8(0x49); jit_emit8(0xBB); jit_emit64(imm); }

static void jit_push_rbx(void) { jit_emit8(0x53); }
static void jit_pop_rbx(void) { jit_emit8(0x5B); }
static void jit_push_r13(void) { jit_emit8(0x41); jit_emit8(0x55); }
static void jit_pop_r13(void) { jit_emit8(0x41); jit_emit8(0x5D); }

static void jit_sub_rsp_imm8(UINT8 imm8) { jit_emit8(0x48); jit_emit8(0x83); jit_emit8(0xEC); jit_emit8(imm8); }
static void jit_add_rsp_imm8(UINT8 imm8) { jit_emit8(0x48); jit_emit8(0x83); jit_emit8(0xC4); jit_emit8(imm8); }

/*
 * Microsoft x64 ABIは呼び出し元に、呼び出し先が第1〜4引数(RCX/RDX/R8/R9)を
 * 自身のスタックフレームへスピルするための32byte「shadow space」の確保を
 * 義務付けている。呼び出し先のCコード(-O1コンパイル)がGC_PROTECT等で引数の
 * アドレスを取ると、コンパイラはそれを[rsp+0]/[rsp+8]/[rsp+16]/[rsp+24]
 * (=呼び出し元が確保しているはずのshadow space)へ書き込む。これを確保せずに
 * callすると、呼び出し元自身のスタックスロット(rsp+0〜+24)を呼び出し先が
 * 上書きしてしまう(Bug A: ZA_OFF_ENV_VAL=16がまさにこの範囲に収まり、
 * os_make_instanceのw2引数スピルで破壊されていた)。
 */
static void jit_call_r11(void) {
    jit_sub_rsp_imm8(32);
    jit_emit8(0x41); jit_emit8(0xFF); jit_emit8(0xD3);
    jit_add_rsp_imm8(32);
}
static void jit_ret(void) { jit_emit8(0xC3); }

/** 汎用レジスタ番号(ModRM/REXの拡張ビットの元になるindex。rax=0始まりでr15=15まで) */
enum {
    ZA_REG_RAX = 0, ZA_REG_RCX = 1, ZA_REG_RDX = 2, ZA_REG_RBX = 3,
    ZA_REG_RSP = 4, ZA_REG_RBP = 5, ZA_REG_RSI = 6, ZA_REG_RDI = 7,
    ZA_REG_R8 = 8, ZA_REG_R9 = 9, ZA_REG_R10 = 10, ZA_REG_R11 = 11,
    ZA_REG_R12 = 12, ZA_REG_R13 = 13, ZA_REG_R14 = 14, ZA_REG_R15 = 15
};

/** mov dst, src (64bitレジスタ間、"mov r/m64, r64"opcode0x89でエンコード) */
static void jit_mov_reg_reg(UINT8 dst, UINT8 src) {
    UINT8 rex = (UINT8)(0x48 | (((src >> 3) & 1) << 2) | ((dst >> 3) & 1));
    jit_emit8(rex);
    jit_emit8(0x89);
    jit_emit8((UINT8)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

/** movabs reg, imm64 (任意レジスタ版。既存のjit_movabs_rax/r11の一般化) */
static void jit_movabs_reg(UINT8 reg, UINT64 imm) {
    UINT8 rex = (UINT8)(0x48 | ((reg >> 3) & 1));
    jit_emit8(rex);
    jit_emit8((UINT8)(0xB8 | (reg & 7)));
    jit_emit64(imm);
}

/** g_jit_code内の自己参照movabs(g_jit_code+offset形式の即値)の最大記録数。
 * za_emit_symbol_nameが埋め込むシンボル名文字列を都度os_make_symbolへ渡すために
 * 呼び出しごとに発行されるため、1defun内の呼び出し式の個数に応じて増える
 * (他の固定上限値と同様、超過時はg_jit_overflowと同じグレースフル・フォールバックに
 * 合流する)。 */
#define ZA_MAX_JIT_RELOCS 128
/** jit_movabs_self_refが発行した自己参照movabsの即値フィールドの位置(g_jit_code内の
 * 絶対オフセット)の一覧。za_try_compile_defunがコンパイル成功後にコードをImmobilized
 * Spaceへコピーする際、この一覧を辿って即値を新しい配置先のアドレスに基づき書き換える
 * (za_try_compile_defun冒頭でリセットする) */
static UINT64 g_jit_reloc_patch_offsets[ZA_MAX_JIT_RELOCS];
static UINT32 g_jit_reloc_count = 0;

/** jit_emit_jmp_to_trampoline(トランポリンへの末尾呼び出しjmp)が発行するrel32フィールドの
 * 位置(g_jit_code内の絶対オフセット)の最大記録数。トランポリン自体はg_jit_code内に固定
 * (再配置されない)だが、jmp命令自身はコンパイル対象の関数本体の一部としてImmobilized
 * Spaceへコピーされて移動するため、rel32(その場のIP相対)は素朴なmemcpyでは古い相対距離
 * のまま残ってしまい、コピー先から実行すると全く別の場所へ飛ぶ。したがってg_jit_reloc_*
 * とは別に、trampoline_offset(コピー後も不変)への絶対距離として再計算しパッチする
 * 必要がある。tagbodyのgoが確定済みタグへ飛ぶ既存のjit_emit_jmp_to(target_offsetは今回の
 * コンパイル対象と一緒に移動する)とは意味が異なるため、記録対象はこの専用関数の呼び出しに
 * 限定する(両者を同一視して全件パッチすると、go側の元々正しかったrel32を誤って書き換えて
 * 壊してしまう)。 */
#define ZA_MAX_TRAMPOLINE_JMPS 128
static UINT64 g_jit_trampoline_jmp_patch_offsets[ZA_MAX_TRAMPOLINE_JMPS];
static UINT32 g_jit_trampoline_jmp_count = 0;

/** 1回のza_try_compile_defun呼び出し(1defunのコンパイル試行)で、g_za_quote_slots/
 * g_za_number_slots/g_za_lambda_slots(いずれかのプール)から確保したスロットの
 * アドレスの一覧(za_try_compile_defun冒頭でリセットする)。コンパイル成功時は
 * os_environment_register_literal_slotで環境の所有物として登録し、失敗時は
 * za_release_literal_slot_allocsで即座にフリーリストへ返却する(Phase3.6)。
 * 3プールの上限合計(32*3=96)を超えて確保されることはないため、この配列サイズで
 * 安全(g_jit_overflowと違い、上限超過はここでは理論上発生しないため専用の
 * overflowフラグには合流しない)。 */
#define ZA_MAX_LITERAL_SLOT_ALLOCS 96
static lisp_val_t *g_za_literal_slot_allocs[ZA_MAX_LITERAL_SLOT_ALLOCS];
static UINT32 g_za_literal_slot_alloc_count = 0;

/** addrをg_za_literal_slot_allocsへ記録する(3プール共有のスロット確保ヘルパー
 * za_alloc_quote_slotと、number/lambdaスロットのインライン確保箇所の両方から
 * 呼ばれる)。 */
static void za_track_literal_slot_alloc(lisp_val_t *addr) {
    if (g_za_literal_slot_alloc_count < ZA_MAX_LITERAL_SLOT_ALLOCS) {
        g_za_literal_slot_allocs[g_za_literal_slot_alloc_count++] = addr;
    }
}

/**
 * g_jit_code内の別オフセット(target_off、za_emit_symbol_name等が埋め込んだ文字列の
 * 先頭)を指す自己参照movabsを発行する。jit_movabs_regとの違いは、即値の埋め込み位置
 * (常に発行開始時点のg_jit_used+2。jit_movabs_regはREX+opcodeの2byteの後に8byte即値を
 * 置くため)をg_jit_reloc_patch_offsetsへ記録する点のみ。この記録により、
 * za_try_compile_defunがコンパイル済みコードをImmobilized Spaceへコピーする際、
 * 「g_jit_code内の別の場所を指す」という自己参照の意味を保ったまま即値を新しい
 * 配置先アドレスへパッチできる(素朴なmemcpyだけでは古いg_jit_codeを指したままになり
 * 壊れる)。
 * @param reg 書き込み先レジスタ
 * @param target_off 指し先のg_jit_code内オフセット
 */
static void jit_movabs_self_ref(UINT8 reg, UINT64 target_off) {
    UINT64 patch_offset = g_jit_used + 2;
    if (g_jit_reloc_count < ZA_MAX_JIT_RELOCS) {
        g_jit_reloc_patch_offsets[g_jit_reloc_count++] = patch_offset;
    } else {
        g_jit_overflow = 1;
    }
    jit_movabs_reg(reg, (UINT64)(void *)(g_jit_code + target_off));
}

/** and reg, imm8 (符号拡張、"and r/m64, imm8"opcode0x83 /4でエンコード) */
static void jit_and_reg_imm8(UINT8 reg, UINT8 imm8) {
    UINT8 rex = (UINT8)(0x48 | ((reg >> 3) & 1));
    jit_emit8(rex);
    jit_emit8(0x83);
    jit_emit8((UINT8)(0xC0 | (4 << 3) | (reg & 7)));
    jit_emit8(imm8);
}

/** jmp reg (レジスタ間接ジャンプ、"jmp r/m64"opcode0xFF /4でエンコード) */
static void jit_jmp_reg(UINT8 reg) {
    if ((reg >> 3) & 1) {
        jit_emit8(0x41);
    }
    jit_emit8(0xFF);
    jit_emit8((UINT8)(0xE0 | (reg & 7)));
}

/**
 * [rsp+disp32]をオペランドとする64bit命令(mov store/load, lea)の共通エンコーダ。
 * baseは常にrsp固定なのでSIBはscale=00,index=100(無し),base=100(rsp)の0x24で固定。
 * @param opcode 0x8B=mov reg,[rsp+disp32] / 0x89=mov [rsp+disp32],reg / 0x8D=lea reg,[rsp+disp32]
 */
static void jit_emit_rsp_disp32(UINT8 opcode, UINT8 reg, INT32 disp32) {
    UINT8 rex = (UINT8)(0x48 | (((reg >> 3) & 1) << 2));
    jit_emit8(rex);
    jit_emit8(opcode);
    jit_emit8((UINT8)(0x80 | ((reg & 7) << 3) | 0x04));
    jit_emit8(0x24);
    jit_emit32((UINT32)disp32);
}

static void jit_lea_reg_rsp(UINT8 reg, INT32 disp32) { jit_emit_rsp_disp32(0x8D, reg, disp32); }
static void jit_mov_reg_from_rsp(UINT8 reg, INT32 disp32) { jit_emit_rsp_disp32(0x8B, reg, disp32); }
static void jit_mov_rsp_from_reg(UINT8 reg, INT32 disp32) { jit_emit_rsp_disp32(0x89, reg, disp32); }

/**
 * mov reg, [base+disp8] (64bit読み出し)。トランポリンがobj[0]/obj[1]をr10経由で
 * 読むためだけに使う(disp8=0/8のみ、baseはrsp/r12以外を前提としSIBは出さない)。
 */
static void jit_mov_reg_from_mem_disp8(UINT8 dst, UINT8 base, UINT8 disp8) {
    UINT8 rex = (UINT8)(0x48 | (((dst >> 3) & 1) << 2) | ((base >> 3) & 1));
    jit_emit8(rex);
    jit_emit8(0x8B);
    jit_emit8((UINT8)(0x40 | ((dst & 7) << 3) | (base & 7)));
    jit_emit8(disp8);
}

static void jit_sub_rsp_imm32(UINT32 imm32) { jit_emit8(0x48); jit_emit8(0x81); jit_emit8(0xEC); jit_emit32(imm32); }
static void jit_add_rsp_imm32(UINT32 imm32) { jit_emit8(0x48); jit_emit8(0x81); jit_emit8(0xC4); jit_emit32(imm32); }

// mov rcx, rax (呼び出し結果を次呼び出しのMS ABI第1引数へ)
static void jit_mov_rcx_rax(void) { jit_emit8(0x48); jit_emit8(0x89); jit_emit8(0xC1); }
// mov rdx, rax (第2オペランドの値をMS ABI第2引数へ)
static void jit_mov_rdx_rax(void) { jit_emit8(0x48); jit_emit8(0x89); jit_emit8(0xC2); }
// mov r13, rax (第1オペランドの値を第2オペランド計算中も保持するレジスタへ退避)
static void jit_mov_r13_rax(void) { jit_emit8(0x49); jit_emit8(0x89); jit_emit8(0xC5); }
// mov rcx, r13 (退避していた第1オペランドの値をMS ABI第1引数へ戻す)
static void jit_mov_rcx_r13(void) { jit_emit8(0x4C); jit_emit8(0x89); jit_emit8(0xE9); }

// cmp rax, r11 (raxとr11を比較。ifのtestがnilかどうかの判定に使う)
static void jit_cmp_rax_r11(void) { jit_emit8(0x4C); jit_emit8(0x39); jit_emit8(0xD8); }

/**
 * je rel32のプレースホルダ(displacement=0)を出力し、書き込んだrel32フィールドの
 * 先頭オフセットを返す。着地点が確定した後にjit_patch_rel32で実際の値を書き込む。
 */
static UINT64 jit_emit_je_rel32_placeholder(void) {
    jit_emit8(0x0F);
    jit_emit8(0x84);
    UINT64 offset = g_jit_used;
    jit_emit32(0);
    return offset;
}

/** jmp rel32のプレースホルダを出力する。使い方はjit_emit_je_rel32_placeholderと同様 */
static UINT64 jit_emit_jmp_rel32_placeholder(void) {
    jit_emit8(0xE9);
    UINT64 offset = g_jit_used;
    jit_emit32(0);
    return offset;
}

/** jne rel32のプレースホルダを出力する。使い方はjit_emit_je_rel32_placeholderと同様 */
static UINT64 jit_emit_jne_rel32_placeholder(void) {
    jit_emit8(0x0F);
    jit_emit8(0x85);
    UINT64 offset = g_jit_used;
    jit_emit32(0);
    return offset;
}

/**
 * patch_offsetにある4バイトのrel32フィールドへ、target_offset(着地点)までの
 * 相対距離を書き込む。オーバーフロー済み、またはpatch_offsetがバッファ範囲外の場合は
 * 何もしない(呼び出し元がg_jit_overflow経由で失敗を検出しロールバックする)。
 */
static void jit_patch_rel32_target(UINT64 patch_offset, UINT64 target_offset) {
    if (g_jit_overflow || patch_offset + 4 > JIT_CODE_SIZE) {
        return;
    }
    INT64 rel = (INT64)target_offset - (INT64)(patch_offset + 4);
    UINT32 rel32 = (UINT32)rel;
    g_jit_code[patch_offset] = (UINT8)(rel32);
    g_jit_code[patch_offset + 1] = (UINT8)(rel32 >> 8);
    g_jit_code[patch_offset + 2] = (UINT8)(rel32 >> 16);
    g_jit_code[patch_offset + 3] = (UINT8)(rel32 >> 24);
}

/** 直前に出力したプレースホルダ(je/jmp/jne)を、現在位置(=着地点)へ向けてpatchする */
static void jit_patch_rel32(UINT64 patch_offset) {
    jit_patch_rel32_target(patch_offset, g_jit_used);
}

/** patch_offsetのプレースホルダを、g_jit_code内の別の(すでに確定した)地点target_offsetへ
 * 向けてpatchする。共有トランポリンスタブのような、今回のコンパイルより前に確定済みの
 * 位置へジャンプする際に使う。 */
static void jit_emit_jmp_to(UINT64 target_offset) {
    UINT64 patch = jit_emit_jmp_rel32_placeholder();
    jit_patch_rel32_target(patch, target_offset);
}

/** jit_emit_jmp_toと同じくrel32のjmpを発行するが、着地点が共有トランポリンのように
 * 「今回のコンパイル対象とは一緒に移動しない、固定された場所」である場合専用。
 * パッチ位置をg_jit_trampoline_jmp_patch_offsetsへ記録し、za_try_compile_defunの
 * コピー後にトランポリンの実アドレスへ向けて再計算・再パッチする。 */
static void jit_emit_jmp_to_trampoline(UINT64 target_offset) {
    UINT64 patch = jit_emit_jmp_rel32_placeholder();
    jit_patch_rel32_target(patch, target_offset);
    if (g_jit_trampoline_jmp_count < ZA_MAX_TRAMPOLINE_JMPS) {
        g_jit_trampoline_jmp_patch_offsets[g_jit_trampoline_jmp_count++] = patch;
    } else {
        g_jit_overflow = 1;
    }
}

/**
 * 自己書き換えコード実行前の命令キャッシュ同期。cpuidはシリアライズ命令であり、
 * 直前に書き込んだ機械語がその後のフェッチで確実に見えるようにする。
 */
static void jit_serialize_icache(void) {
    UINT32 a = 0;
    __asm__ volatile("cpuid" : "=a"(a) : "a"(a) : "rbx", "rcx", "rdx", "memory");
    (void)a;
}

/**
 * 制御転送チェック&早期脱出パターン(拡張5: block/return-from/catch/throw/tagbody)。
 * raxに評価済みの値がある状態で呼ぶ: os_is_control_transfer(rax)(eval.h:82、
 * TAG_INSTANCE+MAGIC_BLOCK_EXIT/MAGIC_CATCH_EXIT/MAGIC_GO_EXITのいずれかを判定する
 * ヒープ確保無しの単純な比較)を呼び出し、
 *  - 制御転送でなければraxを元の値に復元してフォールスルーする。
 *  - 制御転送であればraxを元の値に復元した上でプレースホルダjmpを発行し、そのオフセット
 *    を返す(呼び出し元がjit_patch_rel32/jit_patch_rel32_targetで伝播先へpatchする)。
 * os_is_control_transferはヒープ確保を一切しないため、呼び出し前後でr13へ元の値を
 * 退避するだけで安全(GCによる再配置を心配しなくてよい)。
 */
static UINT64 za_emit_ct_check_and_jmp_if_transfer(void) {
    jit_mov_rcx_rax();
    jit_mov_r13_rax();
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_is_control_transfer);
    jit_call_r11();
    jit_movabs_r11(0);
    jit_cmp_rax_r11();
    UINT64 je_not_transfer = jit_emit_je_rel32_placeholder();
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);
    UINT64 jmp_transfer = jit_emit_jmp_rel32_placeholder();
    jit_patch_rel32(je_not_transfer);
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);
    return jmp_transfer;
}

/** src_reg(TAG_INSTANCEでタグ付けされた値)のタグを外した実アドレスをdst_regへ計算する
 * (mov dst,src; and dst,-8)。obj[0]〜obj[3]をjit_mov_reg_from_mem_disp8(reg,dst_reg,k*8)
 * で読むための前処理(block/catchが自分自身の制御転送magicかどうかを生の構造体比較で
 * 判定する際に使う。eval_catch/eval_tagbodyがos_is_control_transferでなく直接
 * TAG_MASK+obj[0]比較を行っているのと同じ手口)。 */
static void za_emit_untag_instance(UINT8 dst_reg, UINT8 src_reg) {
    jit_mov_reg_reg(dst_reg, src_reg);
    jit_and_reg_imm8(dst_reg, (UINT8)0xF8);
}

/** valがfloat(MAGIC_FLOATのTAG_INSTANCE)かどうかを判定する。runtime.cのis_floatは
 * staticでza.cから参照できないため、reader.cのis_number_resultと同じ手口
 * (TAG_INSTANCEかつ先頭word=magicを直接比較)でここに複製する。 */
static int za_is_float_literal(lisp_val_t val) {
    return (val & TAG_MASK) == TAG_INSTANCE && ((UINT64 *)(val & ~TAG_MASK))[0] == MAGIC_FLOAT;
}

/** valがbignum(MAGIC_BIGNUMのTAG_INSTANCE)かどうかを判定する。za_is_float_literalと
 * 同じ理由でruntime.cのis_bignumをここに複製する。 */
static int za_is_bignum_literal(lisp_val_t val) {
    return (val & TAG_MASK) == TAG_INSTANCE && ((UINT64 *)(val & ~TAG_MASK))[0] == MAGIC_BIGNUM;
}

/**
 * is_literal: 0=paramsのidx番目を参照、1=fixnum即値(literalをそのままmovabs)、
 * 2=quoteシンボル(literalは現在のタグ付きシンボル値だが、GCで移動しうるため
 * emit時はmovabsで直接埋め込まず名前から再解決する。za_emit_operand参照)、
 * 3=&restパラメータそのものへの参照(param_indexはcdrする回数=fixed_countを保持する。
 * 値は元のevaluated_argsのうち固定引数分を消費した後に残る部分リストそのもので、
 * 0と同じcdrループの末尾でcc_carを呼ばない点だけが違う。インタプリタのbind_params
 * (eval.c)が新しいリストを作らずrestへ残りをそのまま束縛するのと同じ挙動)、
 * 5=quote対象がシンボル・fixnum・char・nil以外(cons/string/instance等、ヒープ確保され
 * GCで移動しうる値)。param_indexはg_za_quote_slots配列のインデックス。
 * za_compile_lambdaのクロージャparams/body(g_za_lambda_slots)と同じ「スロット+
 * os_gc_register_root」パターンでGC安全性を確保し、emit時はスロットのアドレスを
 * movabsで埋め込み、都度そこから現在値を読み直す(生ポインタ自体は埋め込まない)。
 * 6=(function sym)(拡張11)。param_indexは使わずliteralにsym(シンボル、生ポインタ)を
 * 保持する。emit時にza_emit_symbol_name+os_make_symbolで名前から再解決し、続けて
 * os_get_function(sym, env)を呼んでその結果を返す(envはZA_OFF_ENV_VALスロットから
 * 読む。za_compile_callの関数解決と同じ手口)。os_get_functionは新規ヒープ確保を
 * 行わず、返る関数オブジェクトは既存のグローバル環境から到達可能なため専用の
 * GCルートは不要(呼び出し元が結果を即座にstore+linkする既存の呼び出し規約に乗る)。
 * 7=裸の(quoteされていない)float/bignumリテラル(拡張13)。param_indexは
 * g_za_number_slots配列のインデックス。is_literal=5(quoteのヒープ値ケース)と
 * 全く同じ「スロット+os_gc_register_root」パターンでGC安全性を確保するが、
 * quote(拡張10)とは構文的トリガーが異なるため専用の別配列を使う。
 */
typedef struct {
    int is_literal;
    lisp_val_t literal;
    UINT64 param_index;
} za_operand_t;

/**
 * 拡張10(quote): (quote X)でXがシンボル・fixnum・char・nil以外(cons/string/instance等、
 * ヒープ確保されGCで移動しうる値)の場合に使う、g_za_lambda_slotsと同型のスロット配列
 * (g_za_lambda_slots本体はza_compile_lambdaのすぐ手前で定義されるが、こちらは
 * za_classify_operandがより前方で参照するため、za_operand_t定義の直後に置く)。
 * Xはコンパイル対象defunのソースASTの一部としてすでにヒープ上に存在する値をそのまま
 * 1回だけ格納し、os_gc_register_rootでGCに追跡させる。生成コードはこのスロットの
 * 現在値をmovabs+読み出しで都度再取得する。プロセス生涯で解放しない(g_za_lambda_slots
 * と同じ考え方)。
 */
#define ZA_MAX_QUOTE_SLOTS 32
static lisp_val_t g_za_quote_slots[ZA_MAX_QUOTE_SLOTS];
static UINT64 g_za_quote_slot_count = 0;
/** Phase3.6: 環境破棄時にza_free_literal_slotが返却したスロットindexのフリーリスト
 * (スタック、LIFO)。za_alloc_quote_slotはここを先に見て、空ならモノトニック
 * カウンタから新規確保する。 */
static UINT64 g_za_quote_slot_free[ZA_MAX_QUOTE_SLOTS];
static UINT64 g_za_quote_slot_free_count = 0;

/**
 * g_za_quote_slotsプールから1枠確保し、valueを格納してos_gc_register_rootする
 * (quoteのヒープ値ケースだけでなく、flet/labelsのgensymシンボル保持でも共有する
 * — gensymはg_symbol_tableに載らないため名前再解決に乗せられず、このスロット越しに
 * しか安全に参照できない、documents/jit.md/このファイル冒頭のコメント参照)。
 * このコンパイル試行で確保したスロットとしてg_za_literal_slot_allocsへも記録する
 * (za_try_compile_defunが成功/失敗いずれの場合も、環境への登録またはフリーリストへの
 * 即時返却に使う、Phase3.6)。
 * @return 確保できれば1(out_slot_idxに書く)、プール枯渇なら0
 */
static int za_alloc_quote_slot(lisp_val_t value, UINT64 *out_slot_idx) {
    UINT64 slot_idx;
    if (g_za_quote_slot_free_count > 0) {
        slot_idx = g_za_quote_slot_free[--g_za_quote_slot_free_count];
    } else if (g_za_quote_slot_count < ZA_MAX_QUOTE_SLOTS) {
        slot_idx = g_za_quote_slot_count++;
    } else {
        return 0;
    }
    g_za_quote_slots[slot_idx] = value;
    os_gc_register_root(&g_za_quote_slots[slot_idx]);
    za_track_literal_slot_alloc(&g_za_quote_slots[slot_idx]);
    *out_slot_idx = slot_idx;
    return 1;
}

/**
 * (quote X)のXが確定した後の分類本体(za_classify_operandのquote分岐と
 * za_compile_quasiquoteの定数畳み込みで共有する)。呼び出し側は`(quote X)`という
 * consを実際に合成せず、Xの値をそのまま渡すこと — quotedを未保護のままos_make_cons
 * で新たなconsを組んでから委譲すると、その合成呼び出し自体がGCを引き起こした場合に
 * quotedがFrom空間の古いアドレスを指したままza_alloc_quote_slotへ格納されてしまう
 * (za_alloc_quote_slotはvalueをそのまま信頼して格納するだけで、moveされたかどうかの
 * 検証はしない)。
 */
static int za_classify_quoted_value(lisp_val_t quoted, za_operand_t *out) {
    if ((quoted & TAG_MASK) == TAG_SYMBOL) {
        out->is_literal = 2;
        out->literal = quoted;
        return 1;
    }
    if (quoted == nil || (quoted & TAG_MASK) == TAG_FIXNUM || (quoted & TAG_MASK) == TAG_CHAR) {
        out->is_literal = 1;
        out->literal = quoted;
        return 1;
    }
    UINT64 slot_idx;
    if (!za_alloc_quote_slot(quoted, &slot_idx)) {
        return 0;
    }
    out->is_literal = 5;
    out->param_index = slot_idx;
    return 1;
}

/**
 * 拡張13: 裸の(quoteされていない)float/bignumリテラル用の、g_za_quote_slotsと同型の
 * スロット配列。reader.cがソースをパースした時点でヒープ確保済みのTAG_INSTANCE値
 * (MAGIC_FLOAT/MAGIC_BIGNUM)をここに1回だけ格納し、os_gc_register_rootでGCに
 * 追跡させる。quote(拡張10)とは構文的トリガーが異なる別の機能のため、専用の配列を
 * 分けている(g_za_quote_slot_countの消費ペースに影響しないようにする)。
 * プロセス生涯で解放しない(g_za_quote_slotsと同じ考え方)。
 */
#define ZA_MAX_NUMBER_SLOTS 32
static lisp_val_t g_za_number_slots[ZA_MAX_NUMBER_SLOTS];
static UINT64 g_za_number_slot_count = 0;
/** Phase3.6: g_za_quote_slot_freeと同じ考え方のフリーリスト。 */
static UINT64 g_za_number_slot_free[ZA_MAX_NUMBER_SLOTS];
static UINT64 g_za_number_slot_free_count = 0;

/** let-IIFEインライン化(拡張B)で使うローカル変数1個分の情報。val_offは
 * za_local_val_offで計算したフレームバイトオフセット。 */
typedef struct za_local_var {
    lisp_val_t sym;
    UINT32 val_off;
} za_local_var_t;

/** let-IIFEインライン化(拡張B)のネスト深さ・1letあたりの変数数の上限
 * (ZA_MAX_NLX_DEPTH等と同じ考え方の実装上の固定上限)。za_local_scope_tが
 * 配列サイズとして使うため、フレームレイアウト定数群より前で定義する。 */
#define ZA_MAX_LET_DEPTH      16
#define ZA_MAX_LOCALS_PER_LET 4

/** let-IIFEインライン化1段分のスコープ。parentで外側スコープへ辿れる連結リスト
 * (ネスト深さはこのチェーンを辿って数えるだけで済むため、既存のnlx_depthのように
 * 別のUINT64引数として全関数へスレッドする必要はない)。 */
typedef struct za_local_scope {
    za_local_var_t vars[ZA_MAX_LOCALS_PER_LET];
    UINT64 count;
    const struct za_local_scope *parent;
} za_local_scope_t;

/** flet/labelsの同時束縛数の上限(ZA_MAX_LOCALS_PER_LETと同じ考え方の実装上の固定上限)。
 * フレームレイアウト定数群より前で定義する。 */
#define ZA_MAX_FLET_BINDINGS 4

/** flet/labels束縛関数1個分のコンパイル時ブックキーピング。gensym_slot_addrは
 * g_za_quote_slots(za_alloc_quote_slot)上のこの束縛専用のgensymシンボルの
 * アドレスで、生成コードはここから都度現在値をロードして
 * os_get_function/os_set_function(…, global_environment)の第1引数に使う
 * (gensymは名前再解決に乗せられないため、documents/jit.md参照)。 */
typedef struct za_fn_binding {
    lisp_val_t orig_name;
    lisp_val_t *gensym_slot_addr;
} za_fn_binding_t;

/** flet/labels 1段分のスコープ。za_local_scope_tと同じ「親への連結リスト、
 * Cスタック上、実行時スロット不要」設計。束縛関数の本体は常にインタプリタ実行
 * (JIT再帰しない)なので、このスコープが実際に読まれるのはコンパイル時の2箇所
 * (za_compile_callの呼び出し先解決、za_classify_operandの(function sym)判定)
 * のみであり、既存のnlx_depth/localsのように全関数へパラメータとしてスレッド
 * せず、za_compile_flet_labelsがコンパイル対象body評価の直前後でのみ
 * g_za_fn_scope(下記)を差し替える(save/restore)方式を採る。C再帰は
 * レキシカルネストと1対1に対応するため、この方式はパラメータスレッディングと
 * 意味的に等価である。 */
typedef struct za_fn_scope {
    za_fn_binding_t bindings[ZA_MAX_FLET_BINDINGS];
    UINT64 count;
    const struct za_fn_scope *parent;
} za_fn_scope_t;

/** 現在コンパイル中の式を包むflet/labelsスコープの連結リスト先頭。
 * za_compile_flet_labelsがbody評価の直前に新スコープへ差し替え、直後に元へ戻す
 * (za_fn_scope_tのコメント参照)。 */
static const za_fn_scope_t *g_za_fn_scope = 0;

/** g_za_fn_scope(を含む親チェーン全体)からsymを探す。
 * @return 見つかれば1(out_bindingに書く)、見つからなければ0
 */
static int za_fn_scope_lookup(const za_fn_scope_t *scope, lisp_val_t sym, const za_fn_binding_t **out_binding) {
    for (const za_fn_scope_t *s = scope; s != 0; s = s->parent) {
        for (UINT64 i = 0; i < s->count; i++) {
            if (s->bindings[i].orig_name == sym) {
                *out_binding = &s->bindings[i];
                return 1;
            }
        }
    }
    return 0;
}

/**
 * paramsの固定引数部分(先頭からfixed_count個)の中からsymと同じシンボルの位置(0始まり)を探す。
 * fixed_countで探索範囲を区切ることで、&restのrest引数名(固定引数の後ろにある)が
 * 誤って「位置idxの1個の値」として参照されることを防ぐ。
 * @return 見つかれば1、見つからなければ0
 */
static int za_param_index(lisp_val_t params, lisp_val_t sym, UINT64 fixed_count, UINT64 *out_index) {
    UINT64 idx = 0;
    for (lisp_val_t cur = params; cur != nil && idx < fixed_count; cur = cc_cdr(cur), idx++) {
        if (cc_car(cur) == sym) {
            *out_index = idx;
            return 1;
        }
    }
    return 0;
}

/**
 * paramsに&rest仮引数が存在する場合、そのrest引数名(シンボル)を取得する。
 * paramsをfixed_count回cdrした先のセルが&restシンボルであるという、
 * za_validate_paramsが検証済みの構造(そのままの`params`/`fixed_count`)を前提にする。
 * @return 見つかれば1(out_symに書く)、&restが無ければ0
 */
static int za_rest_param_symbol(lisp_val_t params, UINT64 fixed_count, lisp_val_t *out_sym) {
    lisp_val_t cur = params;
    for (UINT64 i = 0; i < fixed_count; i++) {
        if (cur == nil || (cur & TAG_MASK) != TAG_CONS) {
            return 0;
        }
        cur = cc_cdr(cur);
    }
    if (cur == nil || (cur & TAG_MASK) != TAG_CONS || cc_car(cur) != g_sym_rest) {
        return 0;
    }
    lisp_val_t tail = cc_cdr(cur);
    if (tail == nil || (tail & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    *out_sym = cc_car(tail);
    return 1;
}

/**
 * 仮引数リストが「重複のない、平坦なシンボルのみのリスト」に、末尾で任意で
 * 「&rest シンボル1個」が続く形であることを検証する。固定引数はZA_MAX_PARAMS以下。
 * @return 検証できれば1(固定引数の個数をout_fixed_countに書く)、できなければ0
 */
static int za_validate_params(lisp_val_t params, UINT64 *out_fixed_count) {
    UINT64 count = 0;
    for (lisp_val_t cur = params; cur != nil; cur = cc_cdr(cur)) {
        if ((cur & TAG_MASK) != TAG_CONS) {
            return 0;
        }
        lisp_val_t sym = cc_car(cur);
        if ((sym & TAG_MASK) != TAG_SYMBOL) {
            return 0;
        }
        if (sym == g_sym_rest) {
            lisp_val_t tail = cc_cdr(cur);
            if (tail == nil || (tail & TAG_MASK) != TAG_CONS || cc_cdr(tail) != nil) {
                return 0;
            }
            lisp_val_t rest_name = cc_car(tail);
            if ((rest_name & TAG_MASK) != TAG_SYMBOL || rest_name == g_sym_rest) {
                return 0;
            }
            for (lisp_val_t chk = params; chk != cur; chk = cc_cdr(chk)) {
                if (cc_car(chk) == rest_name) {
                    return 0;
                }
            }
            *out_fixed_count = count;
            return 1;
        }
        for (lisp_val_t chk = params; chk != cur; chk = cc_cdr(chk)) {
            if (cc_car(chk) == sym) {
                return 0;
            }
        }
        count++;
        if (count > ZA_MAX_PARAMS) {
            return 0;
        }
    }
    *out_fixed_count = count;
    return 1;
}

/** let-IIFEインライン化(拡張B)のローカル変数を、内側スコープから外側へ向かって
 * 探索する。最初に見つかった(=最も内側の)一致を返すことで、シャドーイング
 * (letローカルが外側paramや外側letと同名)が自然に正しく解決される。 */
static int za_local_lookup(const za_local_scope_t *locals, lisp_val_t sym, UINT32 *out_val_off) {
    for (const za_local_scope_t *s = locals; s != 0; s = s->parent) {
        for (UINT64 i = 0; i < s->count; i++) {
            if (s->vars[i].sym == sym) {
                *out_val_off = s->vars[i].val_off;
                return 1;
            }
        }
    }
    return 0;
}

/**
 * +のオペランド1個を分類する。let-IIFEインライン化のローカル変数、paramsの
 * 固定引数部分に含まれるシンボル参照、即値fixnumリテラル、または(quote X)形式の
 * リテラル(拡張10: シンボルに限らずcons/string/instance等も対応。za_emit_operand
 * 参照)を許可する。
 * @return 分類できれば1(outに書く)、できなければ0
 */
static int za_classify_operand(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                                const za_local_scope_t *locals, za_operand_t *out) {
    if ((form & TAG_MASK) == TAG_FIXNUM) {
        out->is_literal = 1;
        out->literal = form;
        return 1;
    }
    // 拡張14: 裸の(quoteされていない)charリテラル。os_make_charはヒープ確保を伴わない
    // 純粋なビットパックなのでGCで移動せず、fixnumと同じくmovabsで直接埋め込める
    // (quoteされたcharケースは既に上のquote分岐(is_literal=1、nil/fixnum/charの
    // 即値判定)で対応済み)。
    if ((form & TAG_MASK) == TAG_CHAR) {
        out->is_literal = 1;
        out->literal = form;
        return 1;
    }
    // 拡張13: 裸の(quoteされていない)float/bignumリテラル。reader.cがソースを
    // パースした時点でヒープ確保済みのTAG_INSTANCE値であり、quoteのヒープ値ケース
    // (is_literal=5)と同じ「スロット+os_gc_register_root」パターンで扱う。formは
    // すでにdefunソースASTの一部としてヒープ上に存在する値をそのままコピーする
    // だけなので、ここで新たなヒープ確保は発生しない。
    if ((form & TAG_MASK) == TAG_INSTANCE && (za_is_float_literal(form) || za_is_bignum_literal(form))) {
        UINT64 slot_idx;
        if (g_za_number_slot_free_count > 0) {
            slot_idx = g_za_number_slot_free[--g_za_number_slot_free_count];
        } else if (g_za_number_slot_count < ZA_MAX_NUMBER_SLOTS) {
            slot_idx = g_za_number_slot_count++;
        } else {
            return 0;
        }
        g_za_number_slots[slot_idx] = form;
        os_gc_register_root(&g_za_number_slots[slot_idx]);
        za_track_literal_slot_alloc(&g_za_number_slots[slot_idx]);
        out->is_literal = 7;
        out->param_index = slot_idx;
        return 1;
    }
    if ((form & TAG_MASK) == TAG_SYMBOL) {
        UINT32 local_off;
        if (za_local_lookup(locals, form, &local_off)) {
            out->is_literal = 4;
            out->param_index = local_off;
            return 1;
        }
        UINT64 idx;
        if (za_param_index(params, form, fixed_count, &idx)) {
            out->is_literal = 0;
            out->param_index = idx;
            return 1;
        }
        lisp_val_t rest_sym;
        if (za_rest_param_symbol(params, fixed_count, &rest_sym) && form == rest_sym) {
            out->is_literal = 3;
            out->param_index = fixed_count;
            return 1;
        }
        // 拡張14: 裸のシンボルT。ランタイム起動時にg_sym_t = os_make_symbol("T")として
        // 生成され、他の一般シンボルと同じくGCコピー時にgc_copy_valueで再配置される
        // (runtime.c:681)ため、nilのような固定領域には置けない。よってis_literal=1で
        // 生ポインタをmovabsするのは不安全であり、quoteシンボルリテラル(is_literal=2)と
        // 全く同じ「名前から都度os_make_symbolで再解決する」手口を流用する
        // (local/paramの解決に失敗した場合のみ、つまりtという名前のローカル変数/仮引数が
        // あればそちらを優先するシャドーイング規則は保たれる)。
        if (form == g_sym_t) {
            out->is_literal = 2;
            out->literal = form;
            return 1;
        }
        return 0;
    }
    // 拡張7(ILOS)で(quote sym)形式のシンボルリテラルに対応し、拡張10でXの実際の
    // タグに応じて即値(fixnum/char/nil)またはヒープ値(cons/string/instance等)にも
    // 対応を広げた。
    if ((form & TAG_MASK) == TAG_CONS && cc_car(form) == g_sym_quote) {
        lisp_val_t rest = cc_cdr(form);
        if ((rest & TAG_MASK) != TAG_CONS || cc_cdr(rest) != nil) {
            return 0;
        }
        lisp_val_t quoted = cc_car(rest);
        return za_classify_quoted_value(quoted, out);
    }
    // 拡張11: (function sym)。symがシンボルの場合のみ対応する(非シンボル、
    // 例えば(function (lambda ...))はza_is_excluded_special_formのガードで
    // fallbackさせる)。os_get_functionは新規ヒープ確保を行わないため、quoteの
    // ヒープ値ケースと違い専用スロットは不要(za_operand_t定義直後のコメント参照)。
    if ((form & TAG_MASK) == TAG_CONS && cc_car(form) == g_sym_function) {
        lisp_val_t rest = cc_cdr(form);
        if ((rest & TAG_MASK) != TAG_CONS || cc_cdr(rest) != nil) {
            return 0;
        }
        lisp_val_t target = cc_car(rest);
        if ((target & TAG_MASK) != TAG_SYMBOL) {
            return 0;
        }
        // flet/labels束縛関数を(function name)で取り出すケースは、gensymの
        // global_environment登録がflet/labelsの動的extentの外では復元済み(=消えて
        // いる)ため、クロージャがそのextentを越えて使われると解決に失敗する。
        // 安全側に倒して無条件fallbackさせる(g_za_saw_flet_labels_escapeのコメント
        // 参照、既存のg_za_saw_escaping_lambdaと同じ思想)。
        const za_fn_binding_t *binding;
        if (za_fn_scope_lookup(g_za_fn_scope, target, &binding)) {
            g_za_saw_flet_labels_escape = 1;
            return 0;
        }
        out->is_literal = 6;
        out->literal = target;
        return 1;
    }
    return 0;
}

/** za_compile_expr/za_try_compile_defunが + - * < = eq null atom を認識するための
 * シンボル束。za_try_compile_defunで1度だけos_make_symbolして組み立てる。
 * car/cdr/consは永続グローバルシンボル(g_sym_car/g_sym_cdr/g_sym_cons)を直接使うため
 * ここには含めない。 */
typedef struct {
    lisp_val_t plus;
    lisp_val_t minus;
    lisp_val_t star;
    lisp_val_t lt;
    lisp_val_t eq;      /* "=" (数値の等値比較) */
    lisp_val_t gt;      /* ">" */
    lisp_val_t le;      /* "<=" */
    lisp_val_t ge;      /* ">=" */
    lisp_val_t eqp;     /* "EQ" (ポインタ同一性比較) */
    lisp_val_t nullsym; /* "NULL" */
    lisp_val_t atom;    /* "ATOM" */
} za_syms_t;

/**
 * 拡張4(lambda): (lambda (params...) . body)のparams/bodyはコンパイル対象defunの
 * ソースAST(生conscell)の一部であり、コンパイル成功後のMAGIC_FUNCTION_NATIVEオブジェクト
 * はfnポインタしか保持しないため、他の何にも保持されない。生成コードから直接movabsで
 * 埋め込むとGCで再配置され無効になるため、za.c専用の静的スロット配列へ1回だけ
 * os_make_cons(params, body)した結果を格納し、os_gc_register_root(runtime.c)で
 * 毎回のGCに追跡させる。生成コードはこのスロットの現在値をmovabs+読み出しで都度
 * 再取得する(スロットの値そのものではなくアドレスを埋め込むので安全)。
 * プロセス生涯で解放しない(JITコードバッファ自体も縮小しないのと同じ考え方)。
 */
#define ZA_MAX_LAMBDA_SLOTS 32
static lisp_val_t g_za_lambda_slots[ZA_MAX_LAMBDA_SLOTS];
static UINT64 g_za_lambda_slot_count = 0;
/** Phase3.6: g_za_quote_slot_freeと同じ考え方のフリーリスト。 */
static UINT64 g_za_lambda_slot_free[ZA_MAX_LAMBDA_SLOTS];
static UINT64 g_za_lambda_slot_free_count = 0;

/**
 * addrがg_za_quote_slots/g_za_number_slots/g_za_lambda_slotsのいずれかのプールの
 * 1エントリを指している前提で、そのプールを判別し(ポインタの範囲比較)、
 * os_gc_unregister_rootでGC rootから外してから、そのプールのフリーリストへ
 * indexを返却する(Phase3.6)。os_environment_reclaim_literal_slotsが環境の
 * literal-slotsスロットを辿って登録済みの各アドレスに対して呼ぶコールバックとして
 * 使う(runtime.cはこの3プールの構造を知らないため、コールバック越しに委譲する)。
 * どのプールにも属さないアドレスが渡された場合は何もしない(該当しない、通常発生しない)。
 * @param addr 解放するスロットのアドレス(いずれかのg_za_*_slots配列内の1要素)
 */
static void za_free_literal_slot(lisp_val_t *addr) {
    os_gc_unregister_root(addr);
    if (addr >= g_za_quote_slots && addr < g_za_quote_slots + ZA_MAX_QUOTE_SLOTS) {
        UINT64 idx = (UINT64)(addr - g_za_quote_slots);
        g_za_quote_slot_free[g_za_quote_slot_free_count++] = idx;
        return;
    }
    if (addr >= g_za_number_slots && addr < g_za_number_slots + ZA_MAX_NUMBER_SLOTS) {
        UINT64 idx = (UINT64)(addr - g_za_number_slots);
        g_za_number_slot_free[g_za_number_slot_free_count++] = idx;
        return;
    }
    if (addr >= g_za_lambda_slots && addr < g_za_lambda_slots + ZA_MAX_LAMBDA_SLOTS) {
        UINT64 idx = (UINT64)(addr - g_za_lambda_slots);
        g_za_lambda_slot_free[g_za_lambda_slot_free_count++] = idx;
        return;
    }
}

/**
 * g_za_literal_slot_allocs(このコンパイル試行で確保した全スロット)を1件ずつ
 * za_free_literal_slotで即座に解放し、記録数を0へ戻す。za_try_compile_defunの
 * 各失敗exitで呼び、失敗したコンパイル試行が確保したスロットをそのまま次回の
 * コンパイル試行へ即座に再利用可能にする(Phase3.6、計画の文言を越えたボーナス改善
 * — 従来は失敗時に永久リークしていた)。
 */
static void za_release_literal_slot_allocs(void) {
    for (UINT32 i = 0; i < g_za_literal_slot_alloc_count; i++) {
        za_free_literal_slot(g_za_literal_slot_allocs[i]);
    }
    g_za_literal_slot_alloc_count = 0;
}

/**
 * GC_PROTECTと同じ仕組み(shadow stackへのgc_rootnodeの連結)を、JIT生成コードから
 * `movabs r11, <addr>; call r11`で呼び出すための最小ヘルパー3つ。呼び出しの引数評価
 * (os_make_cons等でヒープ確保する)の間、レジスタ/ネイティブスタック上の値はGCに
 * 追跡されないため、za_try_compile_defunがフレーム上に確保する値スロット+
 * gc_rootnodeを明示的にこの3つでリンク/アンリンクする。
 */

/** 現在のshadow stackの先頭(呼び出し前のgc_roots)を返す。何もリンクしない。 */
static gc_rootnode *za_gc_current_head(void) {
    return get_current_process()->gc_roots;
}

/** node->var_ptr = var_ptr として、shadow stackの先頭にnodeを繋ぐ(GC_PROTECTのpush相当) */
static void za_gc_link(gc_rootnode *node, lisp_val_t *var_ptr) {
    node->var_ptr = var_ptr;
    node->next = get_current_process()->gc_roots;
    get_current_process()->gc_roots = node;
}

/** gc_rootsをsaved_headへ復元する(GC_PROTECTのpop相当。複数連続linkした分もまとめて外れる) */
static void za_gc_unlink(gc_rootnode *saved_head) {
    get_current_process()->gc_roots = saved_head;
}

/** nodeのnext(=link時点のgc_roots)へ復元し、node1個だけをshadow stackから外す。
 * 外部にsaved_headを持たなくてよい(node->nextはlink後、他のnode/gc_rootsの書き換えで
 * 変化しないため、常にこのnode自身の「元の1個前」を指し続ける)。拡張5のNLXスロット
 * (throw/catch/unwind-protect/return-fromが1個ずつ独立にlink/unlinkする)向け。 */
static void za_gc_unlink_node(gc_rootnode *node) {
    get_current_process()->gc_roots = node->next;
}

/**
 * za_try_compile_defunが確保する追加フレームのレイアウト(すべてrsp相対オフセット)。
 * env用は関数本体の実行中ずっとリンクしたまま(プロローグでリンク、エピローグ/
 * 末尾呼び出し脱出直前でアンリンク)。fn/acc/引数用は呼び出しサイトごとに一時的に
 * リンク/アンリンクする(このプロジェクトのグラマー制約により呼び出しは常に1つずつ
 * しか実行中にならないため、呼び出しサイトが複数あっても同じスロットを使い回せる)。
 */
#define ZA_OFF_ENV_SAVED_HEAD  0    /* env/args link前のgc_roots(関数終了時に復元する) */
/* 旧ZA_OFF_CALL_SAVED_HEAD(=8)は拡張15でZA_OFF_CALL_BASE配下のdepth化スロットへ
 * 移動した(下記参照)。この8バイトは未使用のまま残る(他オフセットは無変更)。 */
#define ZA_OFF_ENV_VAL         16
#define ZA_OFF_ENV_NODE        24   /* 16バイト(var_ptr+next) */
#define ZA_OFF_ARGS_VAL        40   /* 元のevaluated_args(param参照がcc_car/cc_cdrで辿る先頭) */
#define ZA_OFF_ARGS_NODE       48
#define ZA_OFF_FN_VAL          64
#define ZA_OFF_FN_NODE         72
#define ZA_OFF_ACC_VAL         88
#define ZA_OFF_ACC_NODE        96
/* 拡張4(lambda): クロージャ生成1箇所ごとの一時スロット。呼び出しは常に1つずつしか
 * 実行中にならない前提はza_compile_callと同様だが、呼び出しの引数位置にlambdaが
 * 現れるケース(例: (foo (lambda (y) ...)))ではza_compile_lambdaの実行中に外側の
 * za_compile_callがCALL_SAVED_HEAD/FN_VAL/ACC_VALを使用中の可能性があるため、
 * 衝突を避けて専用スロットを別に確保する。 */
#define ZA_OFF_LAMBDA_SAVED_HEAD 112
#define ZA_OFF_LAMBDA_ENV_VAL    120
#define ZA_OFF_LAMBDA_ENV_NODE   128  /* 16バイト */
#define ZA_OFF_LAMBDA_TMP_VAL    144
#define ZA_OFF_LAMBDA_TMP_NODE   152  /* 16バイト */
/* 拡張5(非局所脱出): throw/catch/unwind-protect/return-fromが値をリンクした
 * まま別の再帰的コンパイル呼び出し(GCを起こしうる)を挟んで保持するための
 * 深さ指定スロット配列。同時に生きているspanningスロットは常に1本のスタック
 * として積み重なるため、種類を問わず同じ配列を使い回す。 */
#define ZA_MAX_NLX_DEPTH       4
#define ZA_OFF_NLX_BASE        168
#define ZA_NLX_SLOT_SIZE       24   /* 値8バイト+gc_rootnode16バイト */
/* 168 + 4*24 = 264-271: 16バイト境界を保つためのpadding */
/* 旧ZA_OFF_ARG_BASE(=272)〜656手前は拡張15でdepth化スロット(ZA_OFF_CALL_BASE配下)へ
 * 移動した。この272-655は未使用のまま残る(ZA_OFF_LOCAL_BASE=656より前の他オフセットは
 * 無変更)。ZA_ARG_SLOT_SIZEは新しいdepth化スロットのサイズ計算にも引き続き使う。 */
#define ZA_ARG_SLOT_SIZE       24   /* 値8バイト+gc_rootnode16バイト */
/* let-IIFEインライン化(拡張B): let-localの値+gc_rootnodeのスロット領域。
 * ネスト深さ×1letあたりの変数数で固定サイズを確保し、兄弟let同士(ネストして
 * いない)はスロットを再利用する(depthのみでインデックスするため)。 */
#define ZA_OFF_LOCAL_BASE      656  /* ZA_OFF_ARG_BASE + 16*24 の直後 */
#define ZA_LOCAL_SLOT_SIZE     24   /* 値8バイト+gc_rootnode16バイト */
/* letのbodyは常に評価される(block/catch/unwind-protectと同じ扱い)ため、外側letの
 * body内で内側letが同時に「開いている」ことが構文的に起こり得る。CALL_SAVED_HEAD/
 * 引数スロットも拡張15で同じ理由から深さ配列化した(ZA_OFF_CALL_BASE参照)。
 * ZA_OFF_LAMBDA_SAVED_HEADのような単一スロットは「lambdaリテラルの本体コンパイルは
 * 生成時ではなく後で行われるため、za_compile_lambda自身が再帰的に同時稼働することは
 * ない」という別の理由で成立しているため、こちらはdepth化不要(let-scopeにはこの
 * 前提が無いため、ZA_OFF_NLX_BASEと同じ深さごとの配列にする)。 */
/* 拡張12でZA_MAX_LET_DEPTHを4から8へ拡張した際、べた書きの数値
 * (旧: 1040 = 656 + 4*4*24、1072 = 1040 + 4*8)を手計算し直す必要が
 * あるのはミスの元なので、ZA_MAX_LET_DEPTH経由の計算式に変更した
 * (656 + 8*4*24 = 1424、1424 + 8*8 = 1488、いずれも16バイト境界を保つ)。 */
#define ZA_OFF_LET_SAVED_HEAD_BASE \
    (ZA_OFF_LOCAL_BASE + ZA_MAX_LET_DEPTH * ZA_MAX_LOCALS_PER_LET * ZA_LOCAL_SLOT_SIZE)
/* 拡張15(引数位置での一般呼び出しネスト対応): 一般呼び出し1件ごとのCALL_SAVED_HEAD+
 * 引数スロットを、let-local/NLXと同じ「深さでインデックスする配列」にする。理由:
 * 一般呼び出しの引数(za_compile_call内、allow_call撤去後は`(f (g x))`のような
 * ネストが素通しされる)を評価中、外側呼び出しのCALL_SAVED_HEAD・既に評価済みの
 * 引数スロットは内側呼び出しの実行中も生き続ける必要があり、単一スロットでは
 * 内側が同じ番地を上書きしてしまう(ZA_OFF_FN_VAL/ZA_OFF_ACC_VALは呼び出し引数loopが
 * 完全に終わった後にしか書き込まれないため、こちらは単一スロットのままで安全)。 */
#define ZA_MAX_CALL_DEPTH 4  /* ZA_MAX_NLX_DEPTHと同じ値。392mod16=8のため偶数を保つ */
#define ZA_OFF_CALL_BASE (ZA_OFF_LET_SAVED_HEAD_BASE + ZA_MAX_LET_DEPTH * 8)
#define ZA_CALL_SLOT_SIZE (8 + ZA_MAX_OPERANDS * ZA_ARG_SLOT_SIZE)  /* SAVED_HEAD(8)+引数16本(24*16=384)=392 */
/* 拡張16(算術/比較/car/cdr/null/atom/eq/consのオペランド位置への複合式ネスト対応):
 * fold(+/-/*)・binary(</=/>/<=/>=/eq/cons)がオペランド評価の合間に保持するアキュムレータ
 * (1呼び出しあたり値1個のみ)を、ZA_OFF_CALL_BASEと同じ「深さでインデックスする配列」で
 * 保護する。1深さあたり値1個で済むためZA_CALL_SLOT_SIZE(392、引数16本分)より
 * ずっと小さい単一スロット(24=値8+gc_rootnode16)で足りる。 */
#define ZA_MAX_ARITH_DEPTH 4  /* ZA_MAX_CALL_DEPTH等と同じ値 */
#define ZA_OFF_ARITH_BASE (ZA_OFF_CALL_BASE + ZA_MAX_CALL_DEPTH * ZA_CALL_SLOT_SIZE)
#define ZA_ARITH_SLOT_SIZE 24  /* 値8バイト+gc_rootnode16バイト */
/* 4*24=96は元から16の倍数なので、ZA_MAX_CALL_DEPTHのように偶数に揃える特別な配慮は不要。 */
#define ZA_OFF_FLET_BASE (ZA_OFF_ARITH_BASE + ZA_MAX_ARITH_DEPTH * ZA_ARITH_SLOT_SIZE)
/* flet/labels: 束縛関数を1個ずつ逐次構築して即os_set_functionで消費するため
 * (本体はインタプリタ実行=不透明データなので構築時点では評価されない、
 * za_compile_lambdaの兄弟lambdaが同じTMPスロットを再利用できる理由と同じ)、
 * キャプチャenv構築用のスクラッチはZA_OFF_LAMBDA_*と同型の単一固定スロットで足りる。 */
#define ZA_OFF_FLET_SAVED_HEAD ZA_OFF_FLET_BASE
#define ZA_OFF_FLET_ENV_VAL    (ZA_OFF_FLET_BASE + 8)
#define ZA_OFF_FLET_ENV_NODE   (ZA_OFF_FLET_BASE + 16)  /* 16バイト */
#define ZA_OFF_FLET_TMP_VAL    (ZA_OFF_FLET_BASE + 32)
#define ZA_OFF_FLET_TMP_NODE   (ZA_OFF_FLET_BASE + 40)  /* 16バイト、56バイトで終わる */
/* 56-63: 16バイト境界を保つためのpadding(ZA_OFF_NLX_BASEの264-271と同じ考え方)。 */
/* 復元用の「旧バインディング値」はin-body全体の実行が終わるまで束縛数×ネスト深さ分
 * 生存させる必要があるため、ZA_OFF_NLX_BASEと同じ「深さでインデックスする配列」を
 * さらに束縛数方向にも広げた2次元配列にする(束縛ごとに独立したgc_rootnodeが要る)。 */
#define ZA_OFF_FLET_OLD_BASE   (ZA_OFF_FLET_BASE + 64)
#define ZA_FLET_OLD_SLOT_SIZE  24  /* 値8バイト+gc_rootnode16バイト */
/* quasiquote(za_compile_quasiquote): eval.cのqq_expandはcdr方向にも再帰するが、
 * これをそのままCの再帰・実行時スロット深さに写すとフラットな要素数(リスト長)が
 * そのまま深さ上限を消費してしまい非現実的に小さい上限しか許容できなくなる。
 * そこで1回のquasiquoteレベル内のリスト走査はza_compile_callの引数loopと同じ
 * 「配列で要素ごとにスロットを持ち、右から左へfoldする」方式にし、深さ(qq_depth)は
 * 「動的な内容を含むネストしたサブテンプレートに再帰する場合」だけ消費する
 * (call_depthが要素数[ZA_MAX_OPERANDS]とは別に管理されるのと同じ発想)。 */
#define ZA_MAX_QQ_DEPTH     4    /* ZA_MAX_CALL_DEPTH等と同じ値 */
#define ZA_MAX_QQ_ELEMENTS  16   /* ZA_MAX_OPERANDSと同じ値 */
#define ZA_QQ_SLOT_SIZE     24   /* 値8バイト+gc_rootnode16バイト */
#define ZA_OFF_QQ_BASE \
    (ZA_OFF_FLET_OLD_BASE + ZA_MAX_NLX_DEPTH * ZA_MAX_FLET_BINDINGS * ZA_FLET_OLD_SLOT_SIZE)
/* 1レベル分 = SAVED_HEAD(8、za_call_saved_head_offと同じ用途) +
 * 要素配列(ZA_MAX_QQ_ELEMENTS個、za_arg_val_offと同じ「配列インデックス」パターン) +
 * foldアキュムレータ用1個(za_arith_val_offと同じ単一スロット)。 */
#define ZA_QQ_LEVEL_SIZE    (8 + (ZA_MAX_QQ_ELEMENTS + 1) * ZA_QQ_SLOT_SIZE)
#define ZA_FRAME_EXTRA \
    (ZA_OFF_QQ_BASE + ZA_MAX_QQ_DEPTH * ZA_QQ_LEVEL_SIZE)
/* 既存のシャドウスペース(0x28=40)に追加分を足した、プロローグでsub rspする総量 */
#define ZA_FRAME_TOTAL         (0x28 + ZA_FRAME_EXTRA)

/** 深さdepth(0始まり)の一般呼び出しCALL_SAVED_HEADスロットのオフセット
 * (za_let_saved_head_offと同じ「配列インデックス」パターン)。 */
static UINT32 za_call_saved_head_off(UINT64 depth) { return ZA_OFF_CALL_BASE + (UINT32)depth * ZA_CALL_SLOT_SIZE; }
/** 深さdepth・引数インデックスiの引数スロットの値オフセット。 */
static UINT32 za_arg_val_off(UINT64 depth, UINT64 i) {
    return za_call_saved_head_off(depth) + 8 + (UINT32)i * ZA_ARG_SLOT_SIZE;
}
static UINT32 za_arg_node_off(UINT64 depth, UINT64 i) { return za_arg_val_off(depth, i) + 8; }

/** 深さdepth(0始まり)の算術/比較アキュムレータスロットの値オフセット(拡張16、
 * za_arg_val_offと同じ「配列インデックス」パターン)。 */
static UINT32 za_arith_val_off(UINT64 depth) { return ZA_OFF_ARITH_BASE + (UINT32)depth * ZA_ARITH_SLOT_SIZE; }
static UINT32 za_arith_node_off(UINT64 depth) { return za_arith_val_off(depth) + 8; }

/** 深さdepth(0始まり)のNLXスロットの値オフセット。throw/catch/unwind-protect/
 * return-fromが共有する(za_arg_val_offと同じ「配列インデックス」パターン)。 */
static UINT32 za_nlx_val_off(UINT64 depth) { return ZA_OFF_NLX_BASE + (UINT32)depth * ZA_NLX_SLOT_SIZE; }
static UINT32 za_nlx_node_off(UINT64 depth) { return za_nlx_val_off(depth) + 8; }

/** 深さdepth・インデックスidxのlet-localスロットの値オフセット(za_arg_val_off/
 * za_nlx_val_offと同じ「配列インデックス」パターン)。 */
static UINT32 za_local_val_off(UINT64 depth, UINT64 idx) {
    return ZA_OFF_LOCAL_BASE + (UINT32)depth * (ZA_MAX_LOCALS_PER_LET * ZA_LOCAL_SLOT_SIZE) +
           (UINT32)idx * ZA_LOCAL_SLOT_SIZE;
}
static UINT32 za_local_node_off(UINT64 depth, UINT64 idx) { return za_local_val_off(depth, idx) + 8; }

/** 深さdepth(0始まり)のlet SAVED_HEADスロットのオフセット。 */
static UINT32 za_let_saved_head_off(UINT64 depth) { return ZA_OFF_LET_SAVED_HEAD_BASE + (UINT32)depth * 8; }

/** nlx_depth(0始まり)・束縛インデックスidxのflet/labels旧バインディング退避スロット
 * の値オフセット(za_local_val_offと同じ「2次元配列インデックス」パターン)。 */
static UINT32 za_flet_old_val_off(UINT64 nlx_depth, UINT64 idx) {
    return ZA_OFF_FLET_OLD_BASE + (UINT32)nlx_depth * (ZA_MAX_FLET_BINDINGS * ZA_FLET_OLD_SLOT_SIZE) +
           (UINT32)idx * ZA_FLET_OLD_SLOT_SIZE;
}
static UINT32 za_flet_old_node_off(UINT64 nlx_depth, UINT64 idx) { return za_flet_old_val_off(nlx_depth, idx) + 8; }

/** 深さqq_depth(0始まり)のquasiquote SAVED_HEADスロットのオフセット
 * (za_call_saved_head_offと同じ用途)。 */
static UINT32 za_qq_saved_head_off(UINT64 qq_depth) { return ZA_OFF_QQ_BASE + (UINT32)qq_depth * ZA_QQ_LEVEL_SIZE; }
/** 深さqq_depth・要素インデックスiのquasiquote要素スロットの値オフセット
 * (za_arg_val_offと同じ「配列インデックス」パターン)。 */
static UINT32 za_qq_elem_val_off(UINT64 qq_depth, UINT64 i) {
    return za_qq_saved_head_off(qq_depth) + 8 + (UINT32)i * ZA_QQ_SLOT_SIZE;
}
static UINT32 za_qq_elem_node_off(UINT64 qq_depth, UINT64 i) { return za_qq_elem_val_off(qq_depth, i) + 8; }
/** 深さqq_depth(0始まり)のquasiquote foldアキュムレータスロットの値オフセット
 * (要素配列ZA_MAX_QQ_ELEMENTS個分の直後、za_arith_val_offと同じ単一スロットの発想)。 */
static UINT32 za_qq_acc_val_off(UINT64 qq_depth) {
    return za_qq_saved_head_off(qq_depth) + 8 + ZA_MAX_QQ_ELEMENTS * ZA_QQ_SLOT_SIZE;
}
static UINT32 za_qq_acc_node_off(UINT64 qq_depth) { return za_qq_acc_val_off(qq_depth) + 8; }

/** [rsp+off]の値をregへ読み出す */
static void za_load_slot(UINT8 reg, UINT32 off) { jit_mov_reg_from_rsp(reg, (INT32)off); }
/** regの値を[rsp+off]へ書き込む */
static void za_store_slot(UINT8 reg, UINT32 off) { jit_mov_rsp_from_reg(reg, (INT32)off); }
/** [rsp+off]のアドレスをregへ計算する(lea) */
static void za_addr_of_slot(UINT8 reg, UINT32 off) { jit_lea_reg_rsp(reg, (INT32)off); }

/** value_off番地の値をvar_ptr、node_off番地をgc_rootnodeとしてshadow stackへリンクする
 * (za_gc_linkの呼び出し。rcx=&node, rdx=&value)。 */
static void za_emit_gc_link_slot(UINT32 value_off, UINT32 node_off) {
    za_addr_of_slot(ZA_REG_RCX, node_off);
    za_addr_of_slot(ZA_REG_RDX, value_off);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_link);
    jit_call_r11();
}

/** node_off番地のgc_rootnodeを1個だけshadow stackから外す(za_gc_unlink_nodeの呼び出し。
 * rcx=&node)。拡張5のNLXスロット(throw/catch/unwind-protect/return-fromの一時値)向け。 */
static void za_emit_gc_unlink_slot(UINT32 node_off) {
    za_addr_of_slot(ZA_REG_RCX, node_off);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_unlink_node);
    jit_call_r11();
}

/* symの名前をg_jit_code内に埋め込み、そのオフセットを返す(定義は本ファイル後方)。
 * quoteシンボルオペランドのemit(za_emit_operand)が先に必要とするため前方宣言する。 */
static UINT64 za_emit_symbol_name(lisp_val_t sym);

/**
 * オペランド1個の値をraxへ計算する機械語を出力する。paramsへの参照であれば
 * ARGS_VALスロット(プロローグでリンク済みの元のevaluated_args先頭)から毎回読み直し、
 * cc_cdrをparam_index回・cc_carを1回呼ぶ。呼び出しコード生成(za_compile_call)が
 * os_make_cons等でヒープ確保する間にこの先頭が移動する可能性があるため、レジスタに
 * 生ポインタとしてキャッシュせず、リンク済みスロットから都度読み直す。
 */
static void za_emit_operand(const za_operand_t *op) {
    if (op->is_literal == 1) {
        jit_movabs_rax(op->literal);
        return;
    }
    if (op->is_literal == 2) {
        // 拡張7(ILOS): quoteシンボルは生ポインタをmovabsで埋め込まず、
        // za_compile_call/za_compile_dynamicと同じく名前から都度再解決する
        // (GCでシンボルオブジェクトが移動しても安全)。
        UINT64 name_off = za_emit_symbol_name(op->literal);
        jit_movabs_self_ref(ZA_REG_RCX, name_off);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_symbol);
        jit_call_r11();
        return;
    }
    if (op->is_literal == 5) {
        // 拡張10(quote): g_za_quote_slots[param_index]のアドレスをmovabsで埋め込み、
        // そこから都度dereferenceして現在値を読む(za_compile_lambdaのクロージャ
        // params/bodyスロット読み出しと同じ手口。スロットの値そのものではなく
        // アドレスを埋め込むので、GCでスロットの中身が指す先が移動しても安全)。
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)&g_za_quote_slots[op->param_index]);
        jit_mov_reg_from_mem_disp8(ZA_REG_RAX, ZA_REG_R11, 0);
        return;
    }
    if (op->is_literal == 7) {
        // 拡張13: 裸のfloat/bignumリテラル。is_literal==5と全く同じ手口
        // (g_za_number_slots[param_index]のアドレスをmovabsで埋め込み、都度
        // dereferenceして現在値を読む)。
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)&g_za_number_slots[op->param_index]);
        jit_mov_reg_from_mem_disp8(ZA_REG_RAX, ZA_REG_R11, 0);
        return;
    }
    if (op->is_literal == 6) {
        // 拡張11: (function sym)。za_compile_callの関数解決(za.c内、名前埋め込み→
        // os_make_symbol→os_get_function)と同じ3ステップをそのまま踏襲する。
        UINT64 name_off = za_emit_symbol_name(op->literal);
        jit_movabs_self_ref(ZA_REG_RCX, name_off);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_symbol);
        jit_call_r11();
        jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_RAX);
        za_load_slot(ZA_REG_RDX, ZA_OFF_ENV_VAL);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_get_function);
        jit_call_r11();
        return;
    }
    if (op->is_literal == 4) {
        // let-IIFEインライン化(拡張B): param_indexをparams配列のインデックスではなく
        // フレームバイトオフセットとして直接使う(za_local_val_off参照)。
        za_load_slot(ZA_REG_RAX, (UINT32)op->param_index);
        return;
    }
    za_load_slot(ZA_REG_RCX, ZA_OFF_ARGS_VAL);
    for (UINT64 i = 0; i < op->param_index; i++) {
        jit_movabs_r11((UINT64)(void *)cc_cdr);
        jit_call_r11();
        jit_mov_rcx_rax();
    }
    if (op->is_literal == 3) {
        // &rest: 固定引数分のcdrをすでに済ませたrcxの中身(残りの部分リストそのもの)を
        // そのまま返す。0(単一パラメータ参照)と違い、最後のcc_car呼び出しをしない。
        jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_RCX);
        return;
    }
    jit_movabs_r11((UINT64)(void *)cc_car);
    jit_call_r11();
}

static UINT64 g_za_trampoline_offset = 0;
static int g_za_trampoline_ready = 0;

/**
 * 全JIT関数の末尾呼び出しサイトが共有する小さなトランポリンスタブをg_jit_codeへ
 * 一度だけ書き込み、そのオフセットを返す(2回目以降は書き込まずキャッシュ済みの
 * オフセットを返す)。呼び出し規約(スタブ独自): rcx=evaluated_args, rdx=env,
 * r8=cell(TAG_RAW_POINTER付きのFunction Cellアドレス、os_get_function_cell参照)。
 * 呼び出し元はここへjmpする前に自分のフレームを完全に畳んでおくこと(呼び出し元の
 * レジスタ/フレームはここでは一切保存しない)。
 * このスタブ自体は毎回のza_try_compile_defun呼び出しの冒頭、コンパイル対象の
 * 関数用にg_jit_usedを記録する(=ロールバック時に巻き戻る)より前に確定させる
 * ことで、コンパイル失敗によるロールバックでスタブ自体が失われないようにする。
 */
static UINT64 za_ensure_trampoline(void) {
    if (g_za_trampoline_ready) {
        return g_za_trampoline_offset;
    }
    UINT64 offset = g_jit_used;

    // r8はFunction Cell(TAG_RAW_POINTER付き、Immobilized Space上の固定アドレス)。
    // 中身(現在のfn、TAG_INSTANCE)を読み出してr8を差し替えてから、以降は従来通り
    // fn自体に対する分岐を行う(拡張: 間接呼び出し化)。
    jit_mov_reg_reg(ZA_REG_R10, ZA_REG_R8);
    jit_and_reg_imm8(ZA_REG_R10, 0xF8); // ~TAG_MASK(0x7)
    jit_mov_reg_from_mem_disp8(ZA_REG_R8, ZA_REG_R10, 0); // r8 = *cell (fn, タグ付き)

    // r10 = fn(r8)からTAG_INSTANCEを外した生アドレス
    jit_mov_reg_reg(ZA_REG_R10, ZA_REG_R8);
    jit_and_reg_imm8(ZA_REG_R10, 0xF8); // ~TAG_MASK(0x7)

    // rax = obj[0] (magic)。MAGIC_FUNCTION_NATIVEでなければfallbackへ
    jit_mov_reg_from_mem_disp8(ZA_REG_RAX, ZA_REG_R10, 0);
    jit_movabs_reg(ZA_REG_R11, MAGIC_FUNCTION_NATIVE);
    jit_cmp_rax_r11();
    UINT64 jne_patch = jit_emit_jne_rel32_placeholder();

    // native高速path: r11 = obj[1](生の関数アドレス)へ末尾jmp。rcx=args,rdx=envは
    // 呼び出し規約上すでに正しい位置にあるので、そのままneue関数の入口へ飛べる。
    jit_mov_reg_from_mem_disp8(ZA_REG_R11, ZA_REG_R10, 8);
    jit_jmp_reg(ZA_REG_R11);

    jit_patch_rel32(jne_patch);
    // fallback: os_apply_function(fn, evaluated_args, env)はrcx=fn,rdx=args,r8=envの
    // 順。入ってきた時点でrcx=args,rdx=env,r8=fnなので3点をローテートする。
    jit_mov_reg_reg(ZA_REG_R9, ZA_REG_RCX);
    jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_R8);
    jit_mov_reg_reg(ZA_REG_R8, ZA_REG_RDX);
    jit_mov_reg_reg(ZA_REG_RDX, ZA_REG_R9);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_apply_function);
    jit_jmp_reg(ZA_REG_R11);

    if (g_jit_overflow) {
        return 0;
    }
    g_za_trampoline_offset = offset;
    g_za_trampoline_ready = 1;
    return offset;
}

/**
 * formの先頭がマクロ呼び出しである間、macroexpand-1相当(primitive_macroexpand_1)を
 * fixpointまで繰り返し適用する。cond/let/and/or等はすべてinit.lispのdefmacroで
 * 実装されており、Cの特殊形式としてzaが直接認識する対象ではないため、za_compile_expr
 * が式を評価する位置(bodyそのもの、およびifのtest/then/else)に立つたびにここを通す。
 * 展開後もza未対応の構文(prognやlambda即時呼び出し等)が残る場合は、za_compile_expr側が
 * 通常通りheadを認識できず0を返すことでフォールバックする(このマクロ展開自体は
 * 常に何らかのformを返すので失敗しない)。
 * @param form 展開対象のフォーム
 * @param env マクロ定義を解決する環境(defunの定義時環境)
 * @return マクロでなくなるまで展開した後のフォーム
 */
static lisp_val_t za_macroexpand(lisp_val_t form, lisp_val_t env) {
    for (;;) {
        lisp_val_t wrapped = os_make_cons(form, nil);
        lisp_val_t expanded = primitive_macroexpand_1(wrapped, env);
        if (expanded == form) {
            return form;
        }
        form = expanded;
    }
}

/**
 * head(必ずTAG_SYMBOL)が一般呼び出しとして扱えない特殊形式のシンボルかどうかを判定する。
 * eval.cのos_eval特殊形式ディスパッチ表と同じ集合(quote・if・+・-・*・<・=、
 * block・return-from・catch・throw・tagbody・go・unwind-protect・dynamic・defdynamic・
 * setq(拡張9)は za_compile_expr側で先に個別に処理済みなのでここには含めない)。
 * defglobal(レキシカルなグローバル変数、defdynamicとは無関係の別機構)は未対応のまま
 * ここに残す。
 */
static int za_is_excluded_special_form(lisp_val_t head) {
    // flet/labels(za_compile_flet_labels)・quasiquote(za_compile_quasiquote)は
    // ここから除外する(za_compile_exprが一般呼び出し判定より前で無条件に認識するため、
    // ここに来た時点でどれでもない)。
    return head == g_sym_quote ||
           head == g_sym_defun || head == g_sym_lambda || head == g_sym_defmacro ||
           head == g_sym_function ||
           head == g_sym_defvar || head == g_sym_defconstant ||
           head == g_sym_defglobal;
}

/**
 * formのコンス木のどこかに(unquote x)/(unquote-splicing x)がheadとして現れるかを
 * コンパイル時だけに判定する(eval.cのqq_expandの分岐条件elem==g_sym_unquote/
 * unquote_splicingと同じ着眼点の静的解析版)。真ならza_compile_quasiquoteはこの
 * サブフォームを実行時に組み立てる必要があり、偽なら(quote form)へ丸ごと畳み込める。
 * Cの素朴な再帰(コンパイル時のみ、実行時スロット/GCは絡まないためza_rewrite_fn_refs
 * 同様に深さ制限を設けない)。
 */
static int za_qq_contains_unquote(lisp_val_t form) {
    // nilはg_nil_cellという自己参照consでTAG_CONSタグを持つため、下のTAG_CONS判定
    // だけではnilを弾けずcar(nil)==nilへの無限再帰に陥る(eval.cのqq_expandも
    // 同じ理由でform==nilを先にチェックしている、この関数直前のコメント参照)。
    if (form == nil || (form & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    lisp_val_t elem = cc_car(form);
    if (elem == g_sym_unquote || elem == g_sym_unquote_splicing) {
        return 1;
    }
    return za_qq_contains_unquote(elem) || za_qq_contains_unquote(cc_cdr(form));
}

/**
 * symの名前をg_jit_code内にNUL終端バイト列として埋め込み、そのオフセットを返す
 * (直前にjmpで飛び越えるので、書き込んだバイト列自体は命令として実行されない)。
 * 呼び出し側はこのアドレスをos_make_symbolへ毎回渡して現在のタグ付きシンボルポインタを
 * 再解決する。symのタグ付きポインタ自体をmovabsで埋め込んでしまうと、コンパイル後に
 * 実行されるGCでシンボルオブジェクトが移動した際、生成済み機械語が古いアドレスを
 * 指し続けてしまい安全ではないため(os_make_symbolの重複チェックループと同じ内部表現
 * <word0=名前文字列、文字列は長さ8バイト+本体>を前提に直接読む)。
 */
static UINT64 za_emit_symbol_name(lisp_val_t sym) {
    lisp_addr_t sym_addr = sym & ~TAG_MASK;
    lisp_val_t str_obj = ((lisp_val_t *)sym_addr)[0];
    lisp_addr_t str_addr = str_obj & ~TAG_MASK;
    UINT64 len = ((UINT64 *)str_addr)[0];
    const char *bytes = (const char *)(str_addr + 8);

    UINT64 jmp_patch = jit_emit_jmp_rel32_placeholder();
    UINT64 str_offset = g_jit_used;
    for (UINT64 i = 0; i < len; i++) {
        jit_emit8((UINT8)bytes[i]);
    }
    jit_emit8(0);
    jit_patch_rel32(jmp_patch);
    return str_offset;
}

/**
 * 拡張5(非局所脱出)のうち`tagbody`/`go`用のコンパイル時ラベル解決テーブル。
 * `go`は宛先のタグが同一(ネストしていない)tagbody内に静的に存在する前提で、
 * 実行時に制御転送オブジェクトを作らずコンパイル時に直接jmpへ解決する。
 * 前方参照(未解決のタグへのgo)は`pending`にプレースホルダのオフセットを積んでおき、
 * 該当タグの位置に到達した時点でまとめてpatchする。全てコンパイル時のスタック上の
 * ローカル変数であり、実行時メモリは不要。
 */
#define ZA_MAX_TAGBODY_TAGS 16
#define ZA_MAX_TAGBODY_GOTOS_PER_TAG 8

typedef struct za_tag_entry {
    lisp_val_t tag;
    UINT64 offset;
    int resolved;
    UINT64 pending[ZA_MAX_TAGBODY_GOTOS_PER_TAG];
    int pending_count;
} za_tag_entry_t;

typedef struct za_tagbody_ctx {
    za_tag_entry_t tags[ZA_MAX_TAGBODY_TAGS];
    int tag_count;
    /* このtagbody自身のnlx_depth(za_compile_tagbodyが呼ばれた時点の値)。goが
     * catch/throw/unwind-protectのspanningスロットを開いたまま(nlx_depthが
     * これより深い状態)で直接jmpすると、そのスロットのunlinkコードを飛び越えて
     * GCルートリンクが残留する。za_compile_goはこの値と自分の呼び出し時点の
     * nlx_depthを比較し、不一致ならコンパイルを断念する(block/return-fromは
     * spanningスロットを使わないのでnlx_depthを変えず、この制約の対象にならない)。 */
    UINT64 base_nlx_depth;
} za_tagbody_ctx_t;

static int za_compile_expr(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                            const za_local_scope_t *locals, const za_syms_t *syms,
                            lisp_val_t env, int is_tail, UINT64 trampoline_offset,
                            UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 arith_depth);

static int za_compile_call(lisp_val_t form, lisp_val_t fn_sym, lisp_val_t params, UINT64 fixed_count,
                            const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env, int is_tail,
                            UINT64 trampoline_offset, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth,
                            UINT64 arith_depth);

static int za_compile_setq(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                            const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env,
                            UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 trampoline_offset,
                            UINT64 arith_depth);

/** forms(リスト)を先頭から順にza_compile_expr(is_tail=0)で評価する
 * 共通ヘルパー(block/catch/tagbodyのbody、unwind-protectのcleanup-formsで使う)。
 * 各要素の評価結果が制御転送であれば残りのformsをスキップしrax=その転送値のまま
 * 戻る(1を返す)。formsが空(nil)ならrax=nilとする(eval_prognの空リスト規約)。
 * 最後の要素の結果には制御転送チェックを行わない(その値がそのままこの関数の結果
 * になるので、呼び出し元が必要なら自分で判定する)。 */
static int za_compile_body_forms(lisp_val_t forms, lisp_val_t params, UINT64 fixed_count,
                                  const za_local_scope_t *locals, const za_syms_t *syms,
                                  lisp_val_t env, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 trampoline_offset,
                                  UINT64 arith_depth);

/** `(block name . body)`。bodyをza_compile_body_formsで評価し、結果が制御転送で
 * MAGIC_BLOCK_EXITかつobj[1]がnameと一致するならobj[2]を、そうでなければ結果を
 * そのまま返す。 */
static int za_compile_block(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                             const za_local_scope_t *locals, const za_syms_t *syms,
                             lisp_val_t env, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 trampoline_offset,
                             UINT64 arith_depth);

/** `(return-from name . value-rest)`。value-restが空ならnil、それ以外は評価した値を
 * MAGIC_BLOCK_EXITでラップして返す(すでに制御転送ならラップせずそのまま返す)。 */
static int za_compile_return_from(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                                   const za_local_scope_t *locals, const za_syms_t *syms,
                                   lisp_val_t env, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 trampoline_offset,
                                   UINT64 arith_depth);

/** `(catch tag-form . body)`。 */
static int za_compile_catch(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                             const za_local_scope_t *locals, const za_syms_t *syms,
                             lisp_val_t env, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 trampoline_offset,
                             UINT64 arith_depth);

/** `(throw tag-form result-form)`。 */
static int za_compile_throw(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                             const za_local_scope_t *locals, const za_syms_t *syms,
                             lisp_val_t env, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 trampoline_offset,
                             UINT64 arith_depth);

/** `(unwind-protect protected-form . cleanup-forms)`。 */
static int za_compile_unwind_protect(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                                      const za_local_scope_t *locals, const za_syms_t *syms,
                                      lisp_val_t env, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 trampoline_offset,
                                      UINT64 arith_depth);

/** `(tagbody . body)`。呼ばれるたびに新規のza_tagbody_ctx_tをスタックローカルに
 * 構築して再帰するので、tagbodyのネストにも対応する(外側のtb_ctxを書き換えたり
 * 派生させたりしない)。goは常に自分に渡されたtb_ctx(=最内側のtagbody)だけを
 * 見るため、同名タグの再利用(forマクロが常に%for-loopを使う等)は変数の
 * シャドーイングと同じ意味論で正しく解決される。内側から外側限定のタグへの
 * go、または外側から内側限定のタグへのgoは、いずれも自分自身のタグ表に無い
 * 名前として未解決の前方参照になり、本体走査終了時に0を返して安全にfallbackする
 * (za_compile_go参照)。 */
static int za_compile_tagbody(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                               const za_local_scope_t *locals, const za_syms_t *syms,
                               lisp_val_t env, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 trampoline_offset,
                               UINT64 arith_depth);

/** `(go tag)`。tb_ctx==NULL(tagbodyの外)、またはnlx_depthがtb_ctx->base_nlx_depthより
 * 深い(catch/throw/unwind-protectのspanningスロットを開いたままjmpで飛び越えることに
 * なる)場合は0を返す。tb_ctxは常に呼び出し元(最内側のtagbody)のものだけを見るので、
 * ネストしたtagbodyでは外側のタグ表を探索しない(=goは自分のtagbody内のタグにしか
 * 直接ジャンプできない)。 */
static int za_compile_go(lisp_val_t form, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx);

/** `(dynamic name)`。nameは評価しない。os_get_dynamicで動的束縛(g_dynamic_bindings)
 * から現在の値を返す(未束縛ならnil)。 */
static int za_compile_dynamic(lisp_val_t form);

/** `(defdynamic name value-form)`。value-formを評価し、os_set_dynamicで動的束縛
 * (g_dynamic_bindings)へ登録してnameを返す。 */
static int za_compile_defdynamic(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                                  const za_local_scope_t *locals, const za_syms_t *syms,
                                  lisp_val_t env, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 trampoline_offset,
                                  UINT64 arith_depth);

/** `((lambda (v1 v2...) . body) i1 i2...)`形式のIIFE(let-IIFEインライン化、拡張B)を、
 * 実際の関数呼び出し手続きを経ずbodyをその場にインライン展開する。letのボディは
 * block/catch/unwind-protectと同様、常に評価可能とする。 */
static int za_compile_let(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                           const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env, int is_tail,
                           UINT64 trampoline_offset, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth,
                           UINT64 arith_depth);

/** `(flet bindings . body)`/`(labels bindings . body)`(is_labelsで判別)。unwind-protect
 * と同様、復元cleanupを必ず経由させる必要があるためis_tailを取らない(za_compile_flet_labels
 * 定義直前のコメント参照)。 */
static int za_compile_flet_labels(lisp_val_t form, int is_labels, lisp_val_t params, UINT64 fixed_count,
                                   const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env,
                                   UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth,
                                   UINT64 trampoline_offset, UINT64 arith_depth);

/** `(quasiquote template)`。za_compile_quasiquote定義直前のコメント参照。qq_depthは
 * 0始まりで、動的な内容を含むネストしたサブテンプレートに再帰する場合だけ+1する
 * (1レベル内のリスト走査自体はCのforループで反復するため、要素数は消費しない)。 */
static int za_compile_quasiquote(lisp_val_t template_form, lisp_val_t params, UINT64 fixed_count,
                                  const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env,
                                  UINT64 trampoline_offset, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx,
                                  UINT64 call_depth, UINT64 arith_depth, UINT64 qq_depth);

/** `(progn . body)`。let*の展開の末端(`(let* () . body)` => `(progn ,@body)`)が
 * 必ずこの形を経由するため、let*がlet-IIFEインライン化(拡張B)の恩恵を受けるには
 * progn自体もインライン展開できる必要がある。prognは新しい変数束縛を持たないので
 * let-IIFEのgc_roots保存/unlinkは不要で、za_compile_letのbodyループと同じ「最後の
 * フォームのみ呼び出し元のis_tailを継承」ロジックだけを行う。if/let-IIFE同様、
 * 一般呼び出しの判定より前で無条件に認識する。 */
static int za_compile_progn(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                             const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env, int is_tail,
                             UINT64 trampoline_offset, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth,
                             UINT64 arith_depth);

/**
 * オペランド1個を評価してraxへ値を残す共通ヘルパー(拡張16)。まずza_classify_operand/
 * za_emit_operandの高速パス(fixnumリテラル・params/local参照等、CALL/GC無し)を試し、
 * leafに分類できない場合のみza_compile_expr(is_tail=0)へ再帰する。これにより
 * fold/binary/unaryのオペランド位置に一般呼び出し・if・入れ子の算術/比較など任意の式を
 * 直接書けるようになる。nlx_depth/tb_ctx/call_depthは呼び出し元からそのまま素通しする
 * (オペランド内のreturn-from/throw等のNLXも正しく解決できるようにするため)。
 * @return 評価できれば1、できなければ0
 */
static int za_compile_operand(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                               const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env,
                               UINT64 trampoline_offset, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx,
                               UINT64 call_depth, UINT64 arith_depth) {
    za_operand_t leaf;
    if (za_classify_operand(form, params, fixed_count, locals, &leaf)) {
        za_emit_operand(&leaf);
        return 1;
    }
    return za_compile_expr(form, params, fixed_count, locals, syms, env, 0, trampoline_offset, nlx_depth, tb_ctx,
                            call_depth, arith_depth);
}

/**
 * 「(op operand operand...)」(オペランド2個以上、ZA_MAX_OPERANDS以下)を検証しつつ、
 * pairwise foldでraxへ計算結果を残す機械語を出力する。各オペランドはza_compile_operand
 * 経由で評価するため、leaf(fixnumリテラル/params/local参照等)だけでなく一般呼び出しや
 * 入れ子の算術/比較などの複合式も置ける(拡張16)。呼び出す2引数ラッパー
 * (primitive_add2/primitive_subtract2/primitive_multiply2)はwrapper_fnで指定する。
 *
 * 1個目のオペランドの評価結果は、2個目以降の評価(一般呼び出し等でGCが起こり得る)を
 * 挟んで生きたまま参照される必要があるため、生レジスタではなくarith_depthでインデックス
 * するスロット+gc_rootnode(za_arith_val_off/za_arith_node_off)へlinkして保護する
 * (za_compile_callのCALL_SAVED_HEAD/引数スロットと同じ理由。za.c冒頭の拡張16コメント
 * 参照)。2個目以降のオペランド自身はarith_depth+1で評価する(このfold呼び出し自身の
 * スロットを、オペランド内にネストした別のfold/binaryが上書きしないようにするため)。
 *
 * オペランド評価中に制御転送(NLX)が起きた場合は、za_compile_callのabort_cleanupと
 * 同じ二層構造で処理する: 1個目の転送はまだ何もlinkしていないので直接終端へ、
 * 2個目以降の転送はいったんcleanupブロックに合流してlinkしたスロットをunlinkしてから
 * 終端へ落ちる。
 * @return 対応できれば1、できなければ0(この場合何バイト書き込んだかは呼び出し元が
 * ロールバックするので気にしなくてよい)
 */
static int za_compile_fold(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                            const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env,
                            UINT64 trampoline_offset, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth,
                            UINT64 arith_depth, void *wrapper_fn) {
    if (arith_depth >= ZA_MAX_ARITH_DEPTH) {
        return 0;
    }
    lisp_val_t operand_forms[ZA_MAX_OPERANDS];
    UINT64 count = 0;
    for (lisp_val_t rest = cc_cdr(form); rest != nil; rest = cc_cdr(rest)) {
        if ((rest & TAG_MASK) != TAG_CONS) {
            return 0;
        }
        if (count >= ZA_MAX_OPERANDS) {
            return 0;
        }
        operand_forms[count++] = cc_car(rest);
    }
    if (count < 2) {
        return 0;
    }

    if (!za_compile_operand(operand_forms[0], params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth,
                             tb_ctx, call_depth, arith_depth)) {
        return 0;
    }
    // 1個目はまだ何もlinkしていないため、制御転送ならcleanupを経由せず直接終端へ合流する。
    UINT64 ct_patch0 = za_emit_ct_check_and_jmp_if_transfer();

    UINT64 val_off = za_arith_val_off(arith_depth);
    UINT64 node_off = za_arith_node_off(arith_depth);
    za_store_slot(ZA_REG_RAX, val_off);
    za_emit_gc_link_slot(val_off, node_off);

    UINT64 ct_patches[ZA_MAX_OPERANDS];
    UINT64 ct_patch_count = 0;
    for (UINT64 i = 1; i < count; i++) {
        if (!za_compile_operand(operand_forms[i], params, fixed_count, locals, syms, env, trampoline_offset,
                                 nlx_depth, tb_ctx, call_depth, arith_depth + 1)) {
            return 0;
        }
        ct_patches[ct_patch_count++] = za_emit_ct_check_and_jmp_if_transfer();
        jit_mov_rdx_rax();
        za_load_slot(ZA_REG_RCX, val_off);
        jit_movabs_r11((UINT64)wrapper_fn);
        jit_call_r11();
        if (i != count - 1) {
            za_store_slot(ZA_REG_RAX, val_off);
        }
    }
    // raxに最終結果が残った状態でunlinkを呼ぶ(za_gc_unlink_node自体もCALL経由で
    // volatileレジスタを破壊するため)ので、いったん非volatileなr13へ退避する。
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    za_emit_gc_unlink_slot(node_off);
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);
    UINT64 skip_cleanup_patch = jit_emit_jmp_rel32_placeholder();

    // cleanup: 2個目以降のオペランド評価中に制御転送が起きた場合にここへ合流し、
    // すでにlinkしたアキュムレータスロットをunlinkしてから終端(直後)へ落ちる。
    UINT64 cleanup_offset = g_jit_used;
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    za_emit_gc_unlink_slot(node_off);
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);

    jit_patch_rel32(skip_cleanup_patch);
    jit_patch_rel32(ct_patch0);
    for (UINT64 i = 0; i < ct_patch_count; i++) {
        jit_patch_rel32_target(ct_patches[i], cleanup_offset);
    }
    return 1;
}

/**
 * 「(- operand)」(単項マイナス、0-operandとして符号を反転)または
 * 「(- operand operand...)」(オペランド2個以上、za_compile_foldと同じ左からのfold)
 * を検証しemitする。オペランドはza_compile_fold(拡張16でza_compile_operand経由に
 * 対応済み)へそのまま委譲する。単項マイナスはコンパイル時に先頭へ即値fixnum 0を
 * 挿した一時formを組み立ててza_compile_foldへ渡すことで、既存のfold処理をそのまま
 * 再利用する(os_make_cons自体はコンパイル時に1度呼ぶだけで、実行時アロケーション
 * ではない)。
 * @return 対応できれば1、できなければ0
 */
static int za_compile_minus(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                             const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env,
                             UINT64 trampoline_offset, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth,
                             UINT64 arith_depth) {
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    if (cc_cdr(rest) == nil) {
        lisp_val_t zero_form = os_make_cons(cc_car(form),
                                    os_make_cons(os_make_fixnum(0), os_make_cons(cc_car(rest), nil)));
        return za_compile_fold(zero_form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth,
                                tb_ctx, call_depth, arith_depth, (void *)primitive_subtract2);
    }
    return za_compile_fold(form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                            call_depth, arith_depth, (void *)primitive_subtract2);
}

/**
 * 「(op operand)」(ちょうど1オペランド)を検証しつつ、1引数ラッパー(cc_car/cc_cdr/
 * primitive_null1/primitive_atom1)を呼んでraxへ結果を残す機械語を出力する。オペランドは
 * za_compile_operand経由で評価するため、leaf以外の複合式も置ける(拡張16)。オペランドの
 * 評価結果を後続の評価を挟んで保護する必要が無い(wrapper呼び出し直前でしか使わない)ため、
 * fold/binaryと異なり新規スロットは不要で、制御転送チェックのみ追加する。
 * @return 対応できれば1、できなければ0
 */
static int za_compile_unary(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                             const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env,
                             UINT64 trampoline_offset, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth,
                             UINT64 arith_depth, void *wrapper_fn) {
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS || cc_cdr(rest) != nil) {
        return 0;
    }
    if (!za_compile_operand(cc_car(rest), params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth,
                             tb_ctx, call_depth, arith_depth)) {
        return 0;
    }
    // 保護すべき値が無いので新規スロットは不要。制御転送ならwrapper呼び出しを
    // スキップしてrax(=転送値)のまま終端へ合流するだけでよい。
    UINT64 ct_patch = za_emit_ct_check_and_jmp_if_transfer();
    jit_mov_rcx_rax();
    jit_movabs_r11((UINT64)wrapper_fn);
    jit_call_r11();
    jit_patch_rel32(ct_patch);
    return 1;
}

/**
 * 「(op operand operand)」(ちょうど2オペランド)を検証しつつ、2引数関数(wrapper_fn、
 * rcx=第一オペランド, rdx=第二オペランド)を呼んでraxへ結果を残す機械語を出力する。
 * オペランドはza_compile_operand経由で評価するため、leaf以外の複合式も置ける(拡張16)。
 * 比較(primitive_less_than2/primitive_num_equal2/primitive_eq2)に限らず、2引数を取る
 * 任意の関数(os_make_cons等)に使える汎用の形。3個以上の連鎖・1個以下は今回は非対応
 * (フォールバックする)。
 *
 * za_compile_foldと同じ理由で、1個目(op0)の評価結果は2個目(op1)の評価を挟んで
 * arith_depthスロットへlinkして保護し、制御転送のct_patch/cleanup配線もfoldと
 * 同じ二層構造にする。
 * @return 対応できれば1、できなければ0
 */
static int za_compile_binary(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                              const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env,
                              UINT64 trampoline_offset, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth,
                              UINT64 arith_depth, void *wrapper_fn) {
    if (arith_depth >= ZA_MAX_ARITH_DEPTH) {
        return 0;
    }
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    lisp_val_t op0_form = cc_car(rest);
    lisp_val_t rest2 = cc_cdr(rest);
    if (rest2 == nil || (rest2 & TAG_MASK) != TAG_CONS || cc_cdr(rest2) != nil) {
        return 0;
    }
    lisp_val_t op1_form = cc_car(rest2);

    if (!za_compile_operand(op0_form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                             call_depth, arith_depth)) {
        return 0;
    }
    // 1個目(op0)はまだ何もlinkしていないため、制御転送ならcleanupを経由せず直接終端へ合流する。
    UINT64 ct_patch0 = za_emit_ct_check_and_jmp_if_transfer();

    UINT64 val_off = za_arith_val_off(arith_depth);
    UINT64 node_off = za_arith_node_off(arith_depth);
    za_store_slot(ZA_REG_RAX, val_off);
    za_emit_gc_link_slot(val_off, node_off);

    if (!za_compile_operand(op1_form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                             call_depth, arith_depth + 1)) {
        return 0;
    }
    UINT64 ct_patch1 = za_emit_ct_check_and_jmp_if_transfer();

    jit_mov_rdx_rax();
    za_load_slot(ZA_REG_RCX, val_off);
    jit_movabs_r11((UINT64)wrapper_fn);
    jit_call_r11();
    // raxに結果が残った状態でunlinkを呼ぶ(za_gc_unlink_node自体もCALL経由で
    // volatileレジスタを破壊するため)ので、いったん非volatileなr13へ退避する。
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    za_emit_gc_unlink_slot(node_off);
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);
    UINT64 skip_cleanup_patch = jit_emit_jmp_rel32_placeholder();

    // cleanup: op1評価中に制御転送が起きた場合にここへ合流し、op0用にlinkした
    // スロットをunlinkしてから終端(直後)へ落ちる。
    UINT64 cleanup_offset = g_jit_used;
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    za_emit_gc_unlink_slot(node_off);
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);

    jit_patch_rel32(skip_cleanup_patch);
    jit_patch_rel32(ct_patch0);
    jit_patch_rel32_target(ct_patch1, cleanup_offset);
    return 1;
}

/**
 * 「外側のenv(ZA_OFF_ENV_VAL)を親とする新規environmentを作り、外側関数の固定引数
 * (params[0..fixed_count-1]、&restを除く)と、呼ばれた時点で見えているlet-IIFE
 * ローカル(locals、let-IIFEインライン化拡張B)を全てos_set_variableでコピーする」
 * という、クロージャ用キャプチャenv構築の共通処理を出力する
 * (za_compile_lambda・za_compile_flet_labelsで共有)。呼び出し前にsaved_head_off/
 * env_val_off/env_node_off/tmp_val_off/tmp_node_offに対応するスロットが未使用で
 * あることを前提とする(呼び出し後、env_val_offに新規envが格納されlinkされた状態、
 * tmp_val_offはlinkされたスクラッチとして残る)。localsは外側→内側の順でコピーする:
 * os_set_variableは同一env内の既存の同名束縛を上書きするため、この順序であれば
 * 内側letが外側letや外側paramと同名でシャドーイングしている場合も、後からコピー
 * される内側の値が正しく最終的に勝つ。
 */
static void za_emit_build_capture_env(lisp_val_t params, UINT64 fixed_count, const za_local_scope_t *locals,
                                       UINT32 saved_head_off, UINT32 env_val_off, UINT32 env_node_off,
                                       UINT32 tmp_val_off, UINT32 tmp_node_off) {
    // 1. 専用スコープ開始前のgc_rootsを保存する。
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_current_head);
    jit_call_r11();
    za_store_slot(ZA_REG_RAX, saved_head_off);

    // 2. 新規env = os_make_environment("LAMBDA-ENV", 現在のenv)を構築し、linkする。
    UINT64 env_name_off = za_emit_symbol_name(os_make_symbol("LAMBDA-ENV"));
    jit_movabs_self_ref(ZA_REG_RCX, env_name_off);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_symbol);
    jit_call_r11();
    jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_RAX);
    za_load_slot(ZA_REG_RDX, ZA_OFF_ENV_VAL);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_environment);
    jit_call_r11();
    za_store_slot(ZA_REG_RAX, env_val_off);
    za_emit_gc_link_slot(env_val_off, env_node_off);

    // 3. TMPスロットを一度だけlinkし、外側の固定引数を1つずつos_set_variableで
    // 新規envへコピーする。
    jit_movabs_rax(nil);
    za_store_slot(ZA_REG_RAX, tmp_val_off);
    za_emit_gc_link_slot(tmp_val_off, tmp_node_off);

    lisp_val_t p = params;
    for (UINT64 i = 0; i < fixed_count; i++) {
        lisp_val_t param_sym = cc_car(p);
        p = cc_cdr(p);

        za_operand_t op;
        op.is_literal = 0;
        op.param_index = i;
        za_emit_operand(&op); /* rax = 外側param[i]の現在値 */
        za_store_slot(ZA_REG_RAX, tmp_val_off);

        UINT64 psym_off = za_emit_symbol_name(param_sym);
        jit_movabs_self_ref(ZA_REG_RCX, psym_off);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_symbol);
        jit_call_r11();
        jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_RAX);
        za_load_slot(ZA_REG_RDX, tmp_val_off);
        za_load_slot(ZA_REG_R8, env_val_off);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_set_variable);
        jit_call_r11();
    }

    // 3.5. let-IIFEローカル(locals)を外側→内側の順で同様にコピーする(クロージャ
    // キャプチャ対策(a)、let-IIFEインライン化拡張B)。localsの連結リストは内側→外側
    // なので、祖先を配列に集めてから逆順(外側→内側)に処理する。
    {
        const za_local_scope_t *chain[ZA_MAX_LET_DEPTH];
        UINT64 chain_count = 0;
        for (const za_local_scope_t *s = locals; s != 0 && chain_count < ZA_MAX_LET_DEPTH; s = s->parent) {
            chain[chain_count++] = s;
        }
        for (UINT64 ci = chain_count; ci > 0; ci--) {
            const za_local_scope_t *s = chain[ci - 1];
            for (UINT64 i = 0; i < s->count; i++) {
                za_operand_t lop;
                lop.is_literal = 4;
                lop.param_index = s->vars[i].val_off;
                za_emit_operand(&lop); /* rax = let-localの現在値 */
                za_store_slot(ZA_REG_RAX, tmp_val_off);

                UINT64 lsym_off = za_emit_symbol_name(s->vars[i].sym);
                jit_movabs_self_ref(ZA_REG_RCX, lsym_off);
                jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_symbol);
                jit_call_r11();
                jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_RAX);
                za_load_slot(ZA_REG_RDX, tmp_val_off);
                za_load_slot(ZA_REG_R8, env_val_off);
                jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_set_variable);
                jit_call_r11();
            }
        }
    }
}

/**
 * 「(lambda (lambda_params...) . lambda_body)」から、外側のJIT関数と同じ表現
 * (MAGIC_FUNCTION_INTERPRETED、eval.cのmake_interpreted_function/apply_functionが
 * 素で扱える形)のクロージャを組み立てる機械語を出力する。lambda本体自体はzaが
 * コンパイルせず、呼び出し時は常にインタプリタ経路(apply_function)が実行する。
 * 自由変数解析はせず、外側関数の固定引数と見えているlet-IIFEローカルを呼ばれるたびに
 * 全て新規environmentへコピーし、それをクロージャのenvとする
 * (setqはza対象外なのでコピー後の再代入で共有が破れる心配は無い、
 * za_emit_build_capture_env参照)。
 * @return 対応できれば1、できなければ0(lambdaスロット枯渇時も含む)
 */
static int za_compile_lambda(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                              const za_local_scope_t *locals) {
    /*
     * paramsはコンパイル時にこの関数末尾のループでcc_car/cc_cdrにより辿られるが、
     * その前にos_make_cons(下記)・os_make_symbol("LAMBDA-ENV")という実アロケーションを
     * 伴うランタイム呼び出しをコンパイル時に直接行っており、GCが発火するとparamsの
     * 指す先が再配置される。呼び出し元(za_compile_call/za_compile_expr)が保持する
     * paramsはこの関数にとって値渡しの別コピーであり、呼び出し元側での保護は
     * この関数のローカル変数までは更新しないため、ここで明示的に保護する。
     */
    GC_PROTECT(params);
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    lisp_val_t lambda_params = cc_car(rest);
    lisp_val_t lambda_body = cc_cdr(rest);

    UINT64 slot_idx;
    if (g_za_lambda_slot_free_count > 0) {
        slot_idx = g_za_lambda_slot_free[--g_za_lambda_slot_free_count];
    } else if (g_za_lambda_slot_count < ZA_MAX_LAMBDA_SLOTS) {
        slot_idx = g_za_lambda_slot_count++;
    } else {
        return 0;
    }
    g_za_lambda_slots[slot_idx] = os_make_cons(lambda_params, lambda_body);
    os_gc_register_root(&g_za_lambda_slots[slot_idx]);
    lisp_val_t *slot_addr = &g_za_lambda_slots[slot_idx];
    za_track_literal_slot_alloc(slot_addr);

    za_emit_build_capture_env(params, fixed_count, locals, ZA_OFF_LAMBDA_SAVED_HEAD, ZA_OFF_LAMBDA_ENV_VAL,
                               ZA_OFF_LAMBDA_ENV_NODE, ZA_OFF_LAMBDA_TMP_VAL, ZA_OFF_LAMBDA_TMP_NODE);

    // 4. クロージャ本体を構築する:
    // os_make_instance(MAGIC_FUNCTION_INTERPRETED, lambda_params, lambda_body, new_env)
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)slot_addr);
    jit_mov_reg_from_mem_disp8(ZA_REG_R13, ZA_REG_R11, 0); /* r13 = (lambda_params . lambda_body) 現在値 */
    jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_R13);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)cc_car);
    jit_call_r11();
    za_store_slot(ZA_REG_RAX, ZA_OFF_LAMBDA_TMP_VAL); /* lambda_paramsを一時退避 */

    jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_R13);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)cc_cdr);
    jit_call_r11();
    jit_mov_reg_reg(ZA_REG_R8, ZA_REG_RAX);          /* r8 = lambda_body(=w2) */

    za_load_slot(ZA_REG_RDX, ZA_OFF_LAMBDA_TMP_VAL); /* rdx = lambda_params(=w1) */
    za_load_slot(ZA_REG_R9, ZA_OFF_LAMBDA_ENV_VAL);  /* r9 = new_env(=w3) */
    jit_movabs_reg(ZA_REG_RCX, MAGIC_FUNCTION_INTERPRETED);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_instance);
    jit_call_r11();

    // 5. lambda専用スコープのgc_rootsをまとめてunlinkし、結果(rax)を復元する。
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    za_load_slot(ZA_REG_RCX, ZA_OFF_LAMBDA_SAVED_HEAD);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_unlink);
    jit_call_r11();

    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);
    return 1;
}

/**
 * let-IIFEインライン化(拡張B)のために、`(lambda (v1 v2...) . body)`の仮引数部分が
 * 「重複のない、平坦なシンボルのみ・&rest無し・ZA_MAX_LOCALS_PER_LET以下」で
 * あることを検証し、シンボルを出現順にout_symsへ書き出す(v1スコープでは&restを
 * 持つIIFEは非対応、documents/let.mdの対策(b)は不採用のためbody内のlambda検出は
 * 行わない — クロージャキャプチャはza_compile_lambda側の対策(a)で吸収する)。
 * @return 検証できれば1(個数をout_countに書く)、できなければ0
 */
static int za_local_validate_lambda_vars(lisp_val_t lambda_vars, lisp_val_t *out_syms, UINT64 *out_count) {
    UINT64 count = 0;
    for (lisp_val_t cur = lambda_vars; cur != nil; cur = cc_cdr(cur)) {
        if ((cur & TAG_MASK) != TAG_CONS) {
            return 0;
        }
        lisp_val_t sym = cc_car(cur);
        if ((sym & TAG_MASK) != TAG_SYMBOL || sym == g_sym_rest) {
            return 0;
        }
        for (UINT64 i = 0; i < count; i++) {
            if (out_syms[i] == sym) {
                return 0;
            }
        }
        if (count >= ZA_MAX_LOCALS_PER_LET) {
            return 0;
        }
        out_syms[count++] = sym;
    }
    *out_count = count;
    return 1;
}

/**
 * `((lambda (v1 v2...) . body) i1 i2...)`形式のIIFE(let-IIFEインライン化、拡張B)を、
 * 実際の関数呼び出し手続き(za_compile_call)を経ずbodyをその場にインライン展開する。
 * let, let-star, or, case, case-using, with-open-*はいずれもマクロ展開後この形に
 * 帰着するため自動的に恩恵を受ける(setqを使うforのみza_is_excluded_special_formの
 * 既存除外で対象外のまま、documents/let.md参照)。
 *
 * letのbodyはblock/catch/unwind-protectと同様、常に評価可能とする。init式はまだ新スコープを
 * 積んでいない外側のlocalsで評価し、bodyは新スコープ(parent=呼び出し時のlocals)で
 * 評価する。最後のbody要素のみ呼び出し元のis_tailを継承する(それ以外は0)。
 *
 * GC安全性: za_compile_call/za_compile_lambdaと同じ「SAVED_HEAD保存→(init評価/body
 * 実行)→za_gc_unlinkで一括アンリンク」パターンを踏襲する。init式評価中に
 * return-from/throw/goで中断した場合はabort_cleanupへjmpし、その時点までにlinkした
 * init分だけをまとめてunlinkしてから共通着地点へ合流する。bodyの非最終要素が制御転送
 * した場合はza_emit_ct_check_and_jmp_if_transferで検出し、bodyの後続評価をスキップして
 * 終端のunlinkへ合流する(unlink自体は転送値に関わらず常に行う)。末尾呼び出しで
 * トランポリンへjmpして抜けた場合、この関数末尾の明示的unlinkは実行時に到達しない
 * (unreachableな)コードになるだけで安全 — za_compile_callのis_tail=1経路が
 * ENV_SAVED_HEADまで一括でunlinkするため、let-localのノードも含めて解除される。
 */
static int za_compile_let(lisp_val_t form, lisp_val_t params, UINT64 fixed_count, const za_local_scope_t *locals,
                           const za_syms_t *syms, lisp_val_t env, int is_tail, UINT64 trampoline_offset,
                           UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 arith_depth) {
    /* paramsはinit式/body評価のza_compile_expr再帰経由でコンパイル時アロケーションが
     * 起きうるため、za_compile_call/za_compile_lambdaと同じ理由で明示的に保護する。 */
    GC_PROTECT(params);

    lisp_val_t head = cc_car(form);
    lisp_val_t lrest = cc_cdr(head);
    if (lrest == nil || (lrest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    lisp_val_t lambda_vars = cc_car(lrest);
    lisp_val_t lambda_body = cc_cdr(lrest);

    lisp_val_t var_syms[ZA_MAX_LOCALS_PER_LET];
    UINT64 var_count = 0;
    if (!za_local_validate_lambda_vars(lambda_vars, var_syms, &var_count)) {
        return 0;
    }

    UINT64 depth = 0;
    for (const za_local_scope_t *s = locals; s != 0; s = s->parent) {
        depth++;
    }
    if (depth >= ZA_MAX_LET_DEPTH) {
        return 0;
    }

    lisp_val_t init_forms[ZA_MAX_LOCALS_PER_LET];
    UINT64 init_count = 0;
    for (lisp_val_t rest = cc_cdr(form); rest != nil; rest = cc_cdr(rest)) {
        if ((rest & TAG_MASK) != TAG_CONS) {
            return 0;
        }
        if (init_count >= ZA_MAX_LOCALS_PER_LET) {
            return 0;
        }
        init_forms[init_count++] = cc_car(rest);
    }
    if (init_count != var_count) {
        return 0;
    }

    // 1. letスコープ開始前のgc_rootsを保存する。
    UINT64 saved_head_off = za_let_saved_head_off(depth);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_current_head);
    jit_call_r11();
    za_store_slot(ZA_REG_RAX, saved_head_off);

    // 2. 各init式を、まだ新スコープを積んでいない外側のlocalsで左から順に評価し、
    // 対応するローカルスロットへ格納・linkする。いずれかが制御転送を返した場合は
    // 残りのinit評価・body実行をすべて中止し、abort_cleanupへ直接jmpする
    // (za_compile_callの引数評価ループと同一パターン)。
    UINT64 ct_patches[ZA_MAX_LOCALS_PER_LET];
    UINT64 ct_patch_count = 0;
    for (UINT64 i = 0; i < var_count; i++) {
        if (!za_compile_expr(init_forms[i], params, fixed_count, locals, syms, env, 0, trampoline_offset,
                              nlx_depth, tb_ctx, call_depth, arith_depth)) {
            return 0;
        }
        ct_patches[ct_patch_count++] = za_emit_ct_check_and_jmp_if_transfer();
        UINT64 val_off = za_local_val_off(depth, i);
        UINT64 node_off = za_local_node_off(depth, i);
        za_store_slot(ZA_REG_RAX, val_off);
        za_emit_gc_link_slot(val_off, node_off);
    }

    // abort_cleanup: init評価中の制御転送で中断した場合のみ到達する(通常経路は
    // 直後のjmpで読み飛ばす)。
    UINT64 abort_skip_patch = jit_emit_jmp_rel32_placeholder();
    UINT64 abort_cleanup_offset = g_jit_used;
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    za_load_slot(ZA_REG_RCX, saved_head_off);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_unlink);
    jit_call_r11();
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);
    UINT64 abort_to_end_patch = jit_emit_jmp_rel32_placeholder();
    jit_patch_rel32(abort_skip_patch);
    for (UINT64 i = 0; i < ct_patch_count; i++) {
        jit_patch_rel32_target(ct_patches[i], abort_cleanup_offset);
    }

    // 3. 新しいローカルスコープ(parent=呼び出し時のlocals)を積み、bodyをコンパイル
    // する。za_compile_body_formsはis_tail=0を全要素に固定するため再利用せず、
    // letが末尾位置にある場合のtail-call性を保つ専用ループを使う(最後のフォームのみ
    // 呼び出し元のis_tailを継承)。
    za_local_scope_t new_scope;
    new_scope.count = var_count;
    new_scope.parent = locals;
    for (UINT64 i = 0; i < var_count; i++) {
        new_scope.vars[i].sym = var_syms[i];
        new_scope.vars[i].val_off = za_local_val_off(depth, i);
    }

    UINT64 body_end_patches[ZA_MAX_OPERANDS];
    UINT64 body_end_patch_count = 0;
    if (lambda_body == nil) {
        jit_movabs_rax(nil);
    } else {
        for (lisp_val_t rest = lambda_body; rest != nil; rest = cc_cdr(rest)) {
            if ((rest & TAG_MASK) != TAG_CONS) {
                return 0;
            }
            lisp_val_t elem = cc_car(rest);
            int is_last = (cc_cdr(rest) == nil);
            if (!za_compile_expr(elem, params, fixed_count, &new_scope, syms, env, is_last ? is_tail : 0,
                                  trampoline_offset, nlx_depth, tb_ctx, call_depth, arith_depth)) {
                return 0;
            }
            if (!is_last) {
                if (body_end_patch_count >= ZA_MAX_OPERANDS) {
                    return 0;
                }
                body_end_patches[body_end_patch_count++] = za_emit_ct_check_and_jmp_if_transfer();
            }
        }
    }
    for (UINT64 i = 0; i < body_end_patch_count; i++) {
        jit_patch_rel32(body_end_patches[i]);
    }

    // 4. letスコープのgc_rootsをまとめてunlinkする(末尾呼び出しでトランポリンへ
    // jmpして抜けた場合、以下は実行時に到達しないコードになるだけで安全 — 関数末尾
    // コメント参照)。
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    za_load_slot(ZA_REG_RCX, saved_head_off);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_unlink);
    jit_call_r11();
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);

    jit_patch_rel32(abort_to_end_patch);
    return 1;
}

static int za_compile_progn(lisp_val_t form, lisp_val_t params, UINT64 fixed_count, const za_local_scope_t *locals,
                             const za_syms_t *syms, lisp_val_t env, int is_tail, UINT64 trampoline_offset,
                             UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 arith_depth) {
    lisp_val_t body = cc_cdr(form);

    UINT64 body_end_patches[ZA_MAX_OPERANDS];
    UINT64 body_end_patch_count = 0;
    if (body == nil) {
        jit_movabs_rax(nil);
    } else {
        for (lisp_val_t rest = body; rest != nil; rest = cc_cdr(rest)) {
            if ((rest & TAG_MASK) != TAG_CONS) {
                return 0;
            }
            lisp_val_t elem = cc_car(rest);
            int is_last = (cc_cdr(rest) == nil);
            if (!za_compile_expr(elem, params, fixed_count, locals, syms, env, is_last ? is_tail : 0,
                                  trampoline_offset, nlx_depth, tb_ctx, call_depth, arith_depth)) {
                return 0;
            }
            if (!is_last) {
                if (body_end_patch_count >= ZA_MAX_OPERANDS) {
                    return 0;
                }
                body_end_patches[body_end_patch_count++] = za_emit_ct_check_and_jmp_if_transfer();
            }
        }
    }
    for (UINT64 i = 0; i < body_end_patch_count; i++) {
        jit_patch_rel32(body_end_patches[i]);
    }
    return 1;
}

/**
 * formを評価してraxに結果を残す機械語を出力する。対応するグラマーは
 * 「fixnumリテラル / params固定引数への参照 / (+ operand operand...) / (- operand) /
 * (- operand operand...) / (* operand operand...) / (< operand operand) /
 * (= operand operand) / (if test then else?) / (fn-sym arg arg...)」(if・+/-/*の
 * test/then/else/operandそれぞれの位置には再帰的に上記のいずれかを許容する。拡張15
 * により、ifのtest位置・一般呼び出しの引数位置にも一般呼び出しをネストして書ける
 * ようになった(下記call_depth参照)。拡張16により、+/-/*・比較演算子・
 * car/cdr/null/atom/eq/consのoperand位置にも、leafに限定せず任意の式(一般呼び出し・
 * if・入れ子の算術/比較なども含む)をネストして書けるようになった(za_compile_operand
 * 経由、下記arith_depth参照。</、=はちょうど2オペランドのみ対応)。formおよびifの
 * test/then/elseは、上記のいずれにも分類する前にza_macroexpandでfixpointまで展開する
 * (andのようにif木へ完全展開されるマクロはこれで透過的にコンパイル対象になる)。
 * @param is_tail この位置が末尾位置かどうか(body直下・ifのthen/else<ifが末尾の場合>で
 * 呼び出し側から継承。ifのtestと呼び出しの引数位置は常に0)
 * @param trampoline_offset 末尾呼び出しが共有トランポリンへjmpする際の着地先オフセット
 * @param call_depth 現在アクティブな一般呼び出し(za_compile_call)のネスト深さ。
 * za_compile_call自身が自分の引数を評価する再帰にのみcall_depth+1を渡し、そのほかの
 * 経路(if/let/progn/block等)は素通しする。za_compile_callはこの値がZA_MAX_CALL_DEPTH
 * 以上ならコンパイルを断念する(拡張15、ZA_OFF_CALL_BASE参照)。
 * @param arith_depth 現在アクティブな未完了のfold/binary(+/-/*・比較・cons等)呼び出しの
 * ネスト深さ。za_compile_fold/za_compile_binary自身が自分のオペランドを評価する再帰
 * (za_compile_operand経由)にのみarith_depth+1を渡し、call_depthと同様そのほかの経路は
 * 素通しする。この値がZA_MAX_ARITH_DEPTH以上ならコンパイルを断念する(拡張16、
 * ZA_OFF_ARITH_BASE参照)。
 * @return 対応できれば1、できなければ0(何バイト書き込んだかは呼び出し元がロールバックする)
 */
static int za_compile_expr(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                            const za_local_scope_t *locals, const za_syms_t *syms,
                            lisp_val_t env, int is_tail, UINT64 trampoline_offset,
                            UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 arith_depth) {
    form = za_macroexpand(form, env);

    za_operand_t leaf;
    if (za_classify_operand(form, params, fixed_count, locals, &leaf)) {
        za_emit_operand(&leaf);
        return 1;
    }
    // nilはTAG_CONS(g_nil_cellへの自己参照)なのでza_classify_operandには分類されず、
    // 次のTAG_CONSチェックだけでは素通りしてcc_car(nil)=nilをheadとして誤って評価継続
    // してしまう。andの展開結果(if x y nil)のようにexpr位置に直接nilリテラルが現れる
    // ケースをここで先に捕まえる。+/-/*のオペランド(za_classify_operand)側はleaf限定の
    // 仕様を保つためnilを追加しない。
    if (form == nil) {
        jit_movabs_rax(nil);
        return 1;
    }
    if ((form & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    lisp_val_t head = cc_car(form);
    // let-IIFEインライン化(拡張B): headが(lambda 仮引数 . body)という形のcons
    // (即時呼び出しIIFE)であれば、実際の関数呼び出し手続きを経ずbodyをその場に
    // インライン展開する。let/let*/or/case/case-using/with-open-*はいずれも
    // マクロ展開後この形に帰着するため自動的に恩恵を受ける。if同様、一般呼び出しの判定より
    // 前で無条件に認識する(ifのtest位置等でも使えるようにするため)。
    if ((head & TAG_MASK) == TAG_CONS && cc_car(head) == g_sym_lambda) {
        return za_compile_let(form, params, fixed_count, locals, syms, env, is_tail, trampoline_offset, nlx_depth,
                               tb_ctx, call_depth, arith_depth);
    }
    // progn: let*の展開の末端が必ず`(progn ,@body)`を経由するため、let*がlet-IIFE
    // インライン化の恩恵を受けるにはprognもインライン展開できる必要がある(za_compile_progn
    // 参照)。let-IIFE同様、一般呼び出しの判定より前で無条件に認識する。
    if (head == g_sym_progn) {
        return za_compile_progn(form, params, fixed_count, locals, syms, env, is_tail, trampoline_offset, nlx_depth,
                                 tb_ctx, call_depth, arith_depth);
    }
    if (head == syms->plus) {
        return za_compile_fold(form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                                call_depth, arith_depth, (void *)primitive_add2);
    }
    if (head == syms->minus) {
        return za_compile_minus(form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                                 call_depth, arith_depth);
    }
    if (head == syms->star) {
        return za_compile_fold(form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                                call_depth, arith_depth, (void *)primitive_multiply2);
    }
    if (head == syms->lt) {
        return za_compile_binary(form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                                  call_depth, arith_depth, (void *)primitive_less_than2);
    }
    if (head == syms->eq) {
        return za_compile_binary(form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                                  call_depth, arith_depth, (void *)primitive_num_equal2);
    }
    if (head == syms->gt) {
        return za_compile_binary(form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                                  call_depth, arith_depth, (void *)primitive_greater_than2);
    }
    if (head == syms->le) {
        return za_compile_binary(form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                                  call_depth, arith_depth, (void *)primitive_less_equal2);
    }
    if (head == syms->ge) {
        return za_compile_binary(form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                                  call_depth, arith_depth, (void *)primitive_greater_equal2);
    }
    if (head == g_sym_car) {
        return za_compile_unary(form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                                 call_depth, arith_depth, (void *)cc_car);
    }
    if (head == g_sym_cdr) {
        return za_compile_unary(form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                                 call_depth, arith_depth, (void *)cc_cdr);
    }
    if (head == syms->nullsym) {
        return za_compile_unary(form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                                 call_depth, arith_depth, (void *)primitive_null1);
    }
    if (head == syms->atom) {
        return za_compile_unary(form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                                 call_depth, arith_depth, (void *)primitive_atom1);
    }
    if (head == syms->eqp) {
        return za_compile_binary(form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                                  call_depth, arith_depth, (void *)primitive_eq2);
    }
    if (head == g_sym_cons) {
        return za_compile_binary(form, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth, tb_ctx,
                                  call_depth, arith_depth, (void *)os_make_cons);
    }
    if (head == g_sym_lambda) {
        g_za_saw_escaping_lambda = 1;
        return za_compile_lambda(form, params, fixed_count, locals);
    }
    if (head == g_sym_if) {
        lisp_val_t rest = cc_cdr(form);
        if (rest == nil || (rest & TAG_MASK) != TAG_CONS) {
            return 0;
        }
        lisp_val_t test_form = cc_car(rest);
        lisp_val_t rest2 = cc_cdr(rest);
        if (rest2 == nil || (rest2 & TAG_MASK) != TAG_CONS) {
            return 0;
        }
        lisp_val_t then_form = cc_car(rest2);
        lisp_val_t rest3 = cc_cdr(rest2);
        lisp_val_t else_form = nil;
        int has_else = 0;
        if (rest3 != nil) {
            if ((rest3 & TAG_MASK) != TAG_CONS || cc_cdr(rest3) != nil) {
                return 0;
            }
            else_form = cc_car(rest3);
            has_else = 1;
        }

        if (!za_compile_expr(test_form, params, fixed_count, locals, syms, env, 0, trampoline_offset, nlx_depth, tb_ctx,
                              call_depth, arith_depth)) {
            return 0;
        }
        // testが制御転送(return-from/throw/go等をtest位置に書いたケース)ならthen/elseの
        // 評価を行わず、if全体の結果としてそのまま伝播する(ct_patchはif全体の終端、
        // つまり通常経路がthen/elseを評価し終えて流れ落ちる地点と同じ着地点へ合流する)。
        UINT64 ct_patch = za_emit_ct_check_and_jmp_if_transfer();
        jit_movabs_r11(nil);
        jit_cmp_rax_r11();
        UINT64 je_patch = jit_emit_je_rel32_placeholder();

        if (!za_compile_expr(then_form, params, fixed_count, locals, syms, env, is_tail, trampoline_offset, nlx_depth,
                              tb_ctx, call_depth, arith_depth)) {
            return 0;
        }
        // then分岐の実行後は必ずelse/nilフォールバック側を飛び越える(elseが無い場合も
        // このjmpが無いと直後のjit_movabs_rax(nil)にそのまま流れ落ち、thenの結果が
        // 無条件にnilで上書きされてしまう)
        UINT64 jmp_patch = jit_emit_jmp_rel32_placeholder();

        jit_patch_rel32(je_patch);
        if (has_else) {
            if (!za_compile_expr(else_form, params, fixed_count, locals, syms, env, is_tail, trampoline_offset,
                                  nlx_depth, tb_ctx, call_depth, arith_depth)) {
                return 0;
            }
        } else {
            jit_movabs_rax(nil);
        }
        jit_patch_rel32(jmp_patch);
        jit_patch_rel32(ct_patch);
        return 1;
    }

    // 拡張5(非局所脱出): if同様、一般呼び出しの判定より前で無条件に認識する
    // (インタプリタと同じくifのtestや呼び出しの引数位置にも書けるようにするため)。
    if (head == g_sym_block) {
        return za_compile_block(form, params, fixed_count, locals, syms, env, nlx_depth, tb_ctx, call_depth,
                                 trampoline_offset, arith_depth);
    }
    if (head == g_sym_return_from) {
        return za_compile_return_from(form, params, fixed_count, locals, syms, env, nlx_depth, tb_ctx, call_depth,
                                       trampoline_offset, arith_depth);
    }
    if (head == g_sym_catch) {
        return za_compile_catch(form, params, fixed_count, locals, syms, env, nlx_depth, tb_ctx, call_depth,
                                 trampoline_offset, arith_depth);
    }
    if (head == g_sym_throw) {
        return za_compile_throw(form, params, fixed_count, locals, syms, env, nlx_depth, tb_ctx, call_depth,
                                 trampoline_offset, arith_depth);
    }
    if (head == g_sym_unwind_protect) {
        return za_compile_unwind_protect(form, params, fixed_count, locals, syms, env, nlx_depth, tb_ctx, call_depth,
                                          trampoline_offset, arith_depth);
    }
    if (head == g_sym_tagbody) {
        return za_compile_tagbody(form, params, fixed_count, locals, syms, env, nlx_depth, tb_ctx, call_depth,
                                   trampoline_offset, arith_depth);
    }
    if (head == g_sym_go) {
        return za_compile_go(form, nlx_depth, tb_ctx);
    }

    // 拡張(flet/labels): unwind-protectと同様、復元cleanupを必ず経由させる必要が
    // あるため一般呼び出しの判定より前で無条件に認識する(za_compile_flet_labels
    // 定義直前のコメント参照)。
    if (head == g_sym_flet) {
        return za_compile_flet_labels(form, 0, params, fixed_count, locals, syms, env, nlx_depth, tb_ctx, call_depth,
                                       trampoline_offset, arith_depth);
    }
    if (head == g_sym_labels) {
        return za_compile_flet_labels(form, 1, params, fixed_count, locals, syms, env, nlx_depth, tb_ctx, call_depth,
                                       trampoline_offset, arith_depth);
    }

    // 拡張6(動的変数): if/block等と同様、一般呼び出しの判定より前で無条件に認識する。
    if (head == g_sym_dynamic) {
        return za_compile_dynamic(form);
    }
    if (head == g_sym_defdynamic) {
        return za_compile_defdynamic(form, params, fixed_count, locals, syms, env, nlx_depth, tb_ctx, call_depth,
                                      trampoline_offset, arith_depth);
    }

    // 拡張9(setq): let-localへの再代入。if/block等と同様、一般呼び出しの判定より前で
    // 無条件に認識する。let-local以外(固定引数・グローバル・dynamic変数)への
    // setqはza_compile_setq内でza_local_lookupが失敗し0を返すため、非対応のまま
    // インタプリタへfallbackする。
    if (head == g_sym_setq) {
        return za_compile_setq(form, params, fixed_count, locals, syms, env, nlx_depth, tb_ctx, call_depth,
                                trampoline_offset, arith_depth);
    }

    // 拡張(quasiquote): if/setq等と同様、一般呼び出しの判定より前で無条件に認識する
    // (za_compile_quasiquote定義直前のコメント参照)。formは`(quasiquote template)`
    // という2要素リストなので、渡すのはcadrのtemplateだけ。qq_depthは0から始める。
    if (head == g_sym_quasiquote) {
        lisp_val_t qq_rest = cc_cdr(form);
        if ((qq_rest & TAG_MASK) != TAG_CONS) {
            return 0;
        }
        return za_compile_quasiquote(cc_car(qq_rest), params, fixed_count, locals, syms, env, trampoline_offset,
                                      nlx_depth, tb_ctx, call_depth, arith_depth, 0);
    }

    // 一般呼び出し: headがシンボルで、除外リストの特殊形式でなければ関数呼び出しとして
    // 扱う。ネスト深さの上限チェック・スロットのdepth化はza_compile_call自身が行う
    // (拡張15、ZA_MAX_CALL_DEPTH参照)。
    if ((head & TAG_MASK) != TAG_SYMBOL || za_is_excluded_special_form(head)) {
        return 0;
    }
    return za_compile_call(form, head, params, fixed_count, locals, syms, env, is_tail, trampoline_offset, nlx_depth,
                            tb_ctx, call_depth, arith_depth);
}

/**
 * 一般呼び出し「(fn_sym arg arg...)」をコンパイルする(呼び出しごとに以下の順で
 * 機械語を出力する)。
 *   1. 呼び出し前のgc_rootsをCALL_SAVED_HEADスロット(自分のcall_depth用)へ保存する。
 *   2. 引数を左から順にza_compile_expr(call_depth+1, is_tail=0)で評価し、対応する
 *      引数スロットへ書き込み、その都度linkする。拡張15により引数がさらに一般呼び出し
 *      であってもcall_depth+1側の専用スロットを使うため、自分のCALL_SAVED_HEAD・
 *      評価済みの引数スロットと衝突しない。
 *   3. os_make_symbolでfn_symの名前から現在のタグ付きシンボルポインタを再解決し、
 *      os_get_function_cell(sym, env)の結果(Function Cellのアドレス、fn_obj自体では
 *      ない)をfnスロットへ書き込み、linkする(envはENV_VALスロットから読み直す)。
 *      FN_VAL/ACC_VALは引数loop完了後にしか書き込まれず直後に消費されるため、depth化
 *      せず単一スロットのままで安全。
 *   4. accスロットをnilで初期化しlinkした後、引数スロットを右から左へ
 *      os_make_cons(argslot[i], accslot)で辿ってconsし、その都度accスロットを書き換える。
 *   5. CALL_SAVED_HEADでfn/acc/引数のリンクをまとめて外す。
 *   6. 非末尾ならos_apply_via_cell(cell, evaluated_args, env)を通常のcallで呼ぶ(内部で
 *      cellの中身を読んでos_apply_functionへ委譲する)。末尾ならenvのリンクも外し、
 *      自分のフレームを完全に畳んだ上で共有トランポリンへjmpする(トランポリンもr8に
 *      cellを受け取り、中身を読んでから既存の分岐に入る)。
 * @param call_depth 自分が使うCALL_SAVED_HEAD/引数スロットの深さ。ZA_MAX_CALL_DEPTH
 * 以上ならコンパイルを断念する(引数を評価する再帰にはcall_depth+1を渡す)。
 * @return 対応できれば1、できなければ0(何バイト書き込んだかは呼び出し元がロールバックする)
 */
static int za_compile_call(lisp_val_t form, lisp_val_t fn_sym, lisp_val_t params, UINT64 fixed_count,
                            const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env, int is_tail,
                            UINT64 trampoline_offset, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx,
                            UINT64 call_depth, UINT64 arith_depth) {
    if (call_depth >= ZA_MAX_CALL_DEPTH) {
        return 0;
    }
    // fn_symがflet/labels束縛関数を指す場合、名前再解決ではなくgensymスロット経由で
    // 解決する(下記手順3参照)。このスコープ探索はfn_sym自身(=呼び出しformの構文上の
    // head)にのみ関わるため、引数評価中に入れ子のflet/labelsがg_za_fn_scopeを一時的に
    // 差し替えて元に戻す(za_compile_flet_labels参照)影響を受けない。
    const za_fn_binding_t *fn_binding = 0;
    za_fn_scope_lookup(g_za_fn_scope, fn_sym, &fn_binding);
    /*
     * fn_sym/paramsは引数loop(下記)がza_compile_expr経由でza_compile_lambdaへ再入した
     * 場合、そちら側でos_make_cons/os_make_symbolという実アロケーションを伴うコンパイル
     * 時呼び出しが発生しGCが起動する可能性がある。fn_symはloopの後(za_emit_symbol_name)
     * まで、paramsはloopの後続の引数の再帰コンパイルまで生き続ける値渡しのコピーであり、
     * 呼び出し元での保護はこの関数のローカルコピーまでは更新しないため、ここで明示的に
     * 保護する。
     */
    GC_PROTECT(fn_sym);
    GC_PROTECT(params);
    lisp_val_t arg_forms[ZA_MAX_OPERANDS];
    UINT64 argc = 0;
    for (lisp_val_t rest = cc_cdr(form); rest != nil; rest = cc_cdr(rest)) {
        if ((rest & TAG_MASK) != TAG_CONS) {
            return 0;
        }
        if (argc >= ZA_MAX_OPERANDS) {
            return 0;
        }
        arg_forms[argc++] = cc_car(rest);
    }

    // 1. 呼び出し前のgc_rootsを保存する。
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_current_head);
    jit_call_r11();
    za_store_slot(ZA_REG_RAX, za_call_saved_head_off(call_depth));

    // 2. 引数を左から順に評価し、引数スロットへ書き込み、linkする。いずれかの引数が
    // 制御転送を返した場合は残りの引数評価・関数解決・呼び出しをすべて中止し、
    // abort_cleanup(このループの直後)へ直接jmpする(すでにlinkした引数0..i-1分を
    // CALL_SAVED_HEADでまとめてunlinkしてから、関数末尾の共通着地点へ合流する)。
    // 引数の評価自体はcall_depth+1で再帰する(拡張15、関数doc comment参照)ため、
    // 引数がさらに一般呼び出しであってもこのループが使うスロットとは衝突しない。
    UINT64 ct_patches[ZA_MAX_OPERANDS];
    UINT64 ct_patch_count = 0;
    for (UINT64 i = 0; i < argc; i++) {
        if (!za_compile_expr(arg_forms[i], params, fixed_count, locals, syms, env, 0, trampoline_offset, nlx_depth,
                              tb_ctx, call_depth + 1, arith_depth)) {
            return 0;
        }
        ct_patches[ct_patch_count++] = za_emit_ct_check_and_jmp_if_transfer();
        za_store_slot(ZA_REG_RAX, za_arg_val_off(call_depth, i));
        za_emit_gc_link_slot(za_arg_val_off(call_depth, i), za_arg_node_off(call_depth, i));
    }

    // abort_cleanup: 引数評価中の制御転送で中断した場合のみ到達する(通常経路は
    // 直後のjmpで読み飛ばす)。rax(制御転送値)をr13へ退避してCALL_SAVED_HEADで
    // unlinkし、関数末尾の共通着地点(非末尾呼び出しの通常完了と同じ地点)へ合流する。
    UINT64 abort_skip_patch = jit_emit_jmp_rel32_placeholder();
    UINT64 abort_cleanup_offset = g_jit_used;
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    za_load_slot(ZA_REG_RCX, za_call_saved_head_off(call_depth));
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_unlink);
    jit_call_r11();
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);
    UINT64 abort_to_end_patch = jit_emit_jmp_rel32_placeholder();
    jit_patch_rel32(abort_skip_patch);
    for (UINT64 i = 0; i < ct_patch_count; i++) {
        jit_patch_rel32_target(ct_patches[i], abort_cleanup_offset);
    }

    // 3. os_get_function_cell(fn_sym, env)の結果(Function Cellのアドレス)を
    // fnスロットへ書き込み、linkする。呼び出し先そのもの(fn_obj)ではなくcellの
    // アドレスを保持することで、6a/6bの呼び出しはos_apply_via_cell/トランポリンを
    // 経由した間接呼び出しになる(拡張: Function Cellによる間接呼び出し化)。
    if (fn_binding) {
        // flet/labels束縛関数: gensymは名前再解決(g_symbol_table検索)に乗せられない
        // ため、コンパイル時に確保したgensymスロットのアドレスをmovabsで埋め込み、
        // そこから都度現在値を読む(za_alloc_quote_slot/is_literal=5と同じ手口)。
        // 解決は常にglobal_environmentに対して行う(Design B、レキシカルenvは
        // 使わない。global_environment自体もGCで内容が更新されうるため、値そのもの
        // ではなく変数のアドレスを埋め込み都度dereferenceする)。
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)fn_binding->gensym_slot_addr);
        jit_mov_reg_from_mem_disp8(ZA_REG_R13, ZA_REG_R11, 0);
        jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_R13);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)&global_environment);
        jit_mov_reg_from_mem_disp8(ZA_REG_RDX, ZA_REG_R11, 0);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_get_function_cell);
        jit_call_r11();
    } else {
        UINT64 name_off = za_emit_symbol_name(fn_sym);
        jit_movabs_self_ref(ZA_REG_RCX, name_off);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_symbol);
        jit_call_r11();
        jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);

        jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_R13);
        za_load_slot(ZA_REG_RDX, ZA_OFF_ENV_VAL);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_get_function_cell);
        jit_call_r11();
    }
    za_store_slot(ZA_REG_RAX, ZA_OFF_FN_VAL);
    za_emit_gc_link_slot(ZA_OFF_FN_VAL, ZA_OFF_FN_NODE);

    // 4. accスロットをnilで初期化しlinkした後、右から左へos_make_consでfoldする。
    jit_movabs_rax(nil);
    za_store_slot(ZA_REG_RAX, ZA_OFF_ACC_VAL);
    za_emit_gc_link_slot(ZA_OFF_ACC_VAL, ZA_OFF_ACC_NODE);
    for (UINT64 i = argc; i > 0; i--) {
        za_load_slot(ZA_REG_RCX, za_arg_val_off(call_depth, i - 1));
        za_load_slot(ZA_REG_RDX, ZA_OFF_ACC_VAL);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_cons);
        jit_call_r11();
        za_store_slot(ZA_REG_RAX, ZA_OFF_ACC_VAL);
    }

    // 5. fn/acc/引数のリンクをまとめて外す。
    za_load_slot(ZA_REG_RCX, za_call_saved_head_off(call_depth));
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_unlink);
    jit_call_r11();

    if (!is_tail) {
        // 6a. 非末尾: os_apply_via_cell(cell, evaluated_args, env)を通常のcallで呼ぶ。
        // FN_VALはos_get_function_cellが返したcellアドレスであり、fn_obj自体は
        // 呼び出し先が都度cellの中身を読んで取得する(間接呼び出し化)。
        za_load_slot(ZA_REG_RCX, ZA_OFF_FN_VAL);
        za_load_slot(ZA_REG_RDX, ZA_OFF_ACC_VAL);
        za_load_slot(ZA_REG_R8, ZA_OFF_ENV_VAL);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_apply_via_cell);
        jit_call_r11();
    } else {
        // 6b. 末尾: envのリンクも外す。
        za_load_slot(ZA_REG_RCX, ZA_OFF_ENV_SAVED_HEAD);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_unlink);
        jit_call_r11();

        // トランポリンの呼び出し規約(rcx=evaluated_args, rdx=env, r8=cell)に沿って、
        // フレームを解体する前に値スロットからレジスタへ読み出しておく。
        za_load_slot(ZA_REG_RCX, ZA_OFF_ACC_VAL);
        za_load_slot(ZA_REG_RDX, ZA_OFF_ENV_VAL);
        za_load_slot(ZA_REG_R8, ZA_OFF_FN_VAL);
        jit_add_rsp_imm32(ZA_FRAME_TOTAL);
        jit_pop_r13();
        jit_pop_rbx();
        jit_emit_jmp_to_trampoline(trampoline_offset);
        // 通常経路はここで(実行時に)トランポリンへ抜けるため、以下の
        // abort_to_end_patchの着地点はis_tail=1の場合は「引数評価中に制御転送で
        // 中断した」場合のみ機械語レベルで到達する(その場合は実呼び出しを一切
        // 行わずrax=制御転送値のまま、非末尾呼び出しの通常完了と同じ形で
        // za_compile_expr側へ戻る — 呼び出しが実際には発生していないため
        // トランポリンへは絶対に飛ばない)。
    }

    // 引数評価中に制御転送で中断した場合の共通着地点(is_tail=0の通常完了地点とも
    // 一致する)。ここに到達する時点でrax=結果(通常のapply結果、または中断した
    // 制御転送値)であり、追加の処理は不要。
    jit_patch_rel32(abort_to_end_patch);
    return 1;
}

/** za_compile_quasiquoteの1レベル分のリスト走査で集める要素(コンパイル時のみ使う、
 * 実行時スロットとは無関係)。is_splice=0はELEMENT(qq_expandのhead位置、この関数
 * 自身をqq_depth+1で再帰する)、is_splice=1はSPLICE(,@xのx、za_compile_operandで
 * 直接評価する、テンプレート再帰はしない)。 */
typedef struct {
    lisp_val_t data;
    int is_splice;
} za_qq_item_t;

/**
 * 「(quasiquote template)」を検証しつつeval.cのqq_expand/qq_appendと等価な結果を
 * 実行時に構築する機械語を出力する。eval.cのqq_expand同様、ネストしたbacktickは
 * 特別扱いしない(quote-levelを追跡しない素朴な実装。za_qq_contains_unquote/この
 * 関数のどちらもQUASIQUOTEシンボル自体を特別扱いしないため、自然にこの意味論に
 * 一致する)。
 *
 * 1. templateのコンス木のどこにもunquote/unquote-splicingが現れなければ
 *    (za_qq_contains_unquote)、(quote template)へ丸ごと合成し既存のquote
 *    コンパイルパス(za_compile_operand経由のza_classify_operand/za_alloc_quote_slot)
 *    へ委譲する(実行時cons化は一切不要)。
 * 2. templateが直接`(unquote x)`/`(unquote-splicing x)`(qq_expandの先頭チェックに
 *    相当)なら、xをza_compile_operandでそのままコンパイルする。foldもqq_depthの
 *    消費も不要。
 * 3. それ以外は1レベル分のリストをCのforループで走査し(za_compile_callの引数loop
 *    と同じ「配列に集めてから右から左へfold」方式、リスト長はqq_depthを消費しない)、
 *    各要素をELEMENT(この関数自身をqq_depth+1で再帰=qq_expandの「head =
 *    qq_expand(elem)」に相当。中でさらに(1)(2)へ自然に分岐する)、または
 *    SPLICE(,@xのxをza_compile_operandで直接評価。qq_expandがxをos_evalで直接
 *    評価するのに対応)に分類する。末尾(tail)はTAIL_NIL(nil)/TAIL_CONST(atom、
 *    dotted listの終端)/TAIL_UNQUOTE(x、`(a . ,b)`の,bまたは裸の,x/,@x)の
 *    いずれかで、foldの初期シードになる。right-to-leftでfoldし、ELEMENTは
 *    os_make_cons、SPLICEはqq_appendを使う(za_compile_callの手順4と同型)。
 *    qq_depth>=ZA_MAX_QQ_DEPTH、要素数>=ZA_MAX_QQ_ELEMENTSはいずれもfallback
 *    (0を返す)。NLX(制御転送)安全性はza_compile_callのabort_cleanupと同じ
 *    二層構造で確保する。
 * @param qq_depth 現在のネスト深さ(0始まり、ELEMENT再帰時だけ+1する)
 * @return 対応できれば1、できなければ0(何バイト書き込んだかは呼び出し元が
 * ロールバックするので気にしなくてよい)
 */
static int za_compile_quasiquote(lisp_val_t template_form, lisp_val_t params, UINT64 fixed_count,
                                  const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env,
                                  UINT64 trampoline_offset, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx,
                                  UINT64 call_depth, UINT64 arith_depth, UINT64 qq_depth) {
    // 1. 全体が定数(unquote/unquote-splicingを一切含まない)なら丸ごとquote委譲する。
    // (quote template_form)というconsを実際に合成してza_compile_operandへ委譲すると、
    // その合成用os_make_cons自体がGCを引き起こした場合にtemplate_formが未保護のまま
    // From空間の古いアドレスを指し続けてしまう(za_classify_quoted_value直前のコメント
    // 参照)ため、合成せずtemplate_formをそのまま分類する。
    if (!za_qq_contains_unquote(template_form)) {
        za_operand_t leaf;
        if (!za_classify_quoted_value(template_form, &leaf)) {
            return 0;
        }
        za_emit_operand(&leaf);
        return 1;
    }
    // 2. template自身が裸の(unquote x)/(unquote-splicing x)(qq_expandの先頭チェック)
    // なら、xを直接コンパイルしfold不要で済ませる。
    if ((template_form & TAG_MASK) == TAG_CONS) {
        lisp_val_t bare_head = cc_car(template_form);
        if (bare_head == g_sym_unquote || bare_head == g_sym_unquote_splicing) {
            lisp_val_t bare_rest = cc_cdr(template_form);
            if ((bare_rest & TAG_MASK) != TAG_CONS) {
                return 0;
            }
            return za_compile_operand(cc_car(bare_rest), params, fixed_count, locals, syms, env, trampoline_offset,
                                       nlx_depth, tb_ctx, call_depth, arith_depth);
        }
    }
    if (qq_depth >= ZA_MAX_QQ_DEPTH) {
        return 0;
    }

    // 3. 1レベル分のリストを走査し、要素配列とtailの種類を確定する(コンパイル時のみ、
    // cc_car/cc_cdrの読み出しだけでアロケーションは起きない)。
    za_qq_item_t items[ZA_MAX_QQ_ELEMENTS];
    UINT64 item_count = 0;
    enum { QQ_TAIL_NIL, QQ_TAIL_CONST, QQ_TAIL_UNQUOTE } tail_kind = QQ_TAIL_NIL;
    lisp_val_t tail_data = nil;
    lisp_val_t remaining = template_form;
    for (;;) {
        if (remaining == nil) {
            tail_kind = QQ_TAIL_NIL;
            break;
        }
        if ((remaining & TAG_MASK) != TAG_CONS) {
            tail_kind = QQ_TAIL_CONST;
            tail_data = remaining;
            break;
        }
        lisp_val_t elem = cc_car(remaining);
        if (elem == g_sym_unquote || elem == g_sym_unquote_splicing) {
            lisp_val_t unq_rest = cc_cdr(remaining);
            if ((unq_rest & TAG_MASK) != TAG_CONS) {
                return 0;
            }
            tail_kind = QQ_TAIL_UNQUOTE;
            tail_data = cc_car(unq_rest);
            break;
        }
        if ((elem & TAG_MASK) == TAG_CONS && cc_car(elem) == g_sym_unquote_splicing) {
            lisp_val_t splice_rest = cc_cdr(elem);
            if ((splice_rest & TAG_MASK) != TAG_CONS) {
                return 0;
            }
            if (item_count >= ZA_MAX_QQ_ELEMENTS) {
                return 0;
            }
            items[item_count].data = cc_car(splice_rest);
            items[item_count].is_splice = 1;
            item_count++;
            remaining = cc_cdr(remaining);
            continue;
        }
        if (item_count >= ZA_MAX_QQ_ELEMENTS) {
            return 0;
        }
        items[item_count].data = elem;
        items[item_count].is_splice = 0;
        item_count++;
        remaining = cc_cdr(remaining);
    }

    // paramsは本関数の残り全体(tail・各要素の再帰コンパイル、いずれもza_compile_lambda
    // 経由の実アロケーションを伴い得る)で使われ続けるため、za_compile_callのfn_sym/
    // paramsと同じ理由で明示的に保護する。
    GC_PROTECT(params);

    // 4. 呼び出し前のgc_rootsを保存する(za_compile_callの手順1と同じ用途)。
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_current_head);
    jit_call_r11();
    za_store_slot(ZA_REG_RAX, za_qq_saved_head_off(qq_depth));

    UINT64 ct_patches[ZA_MAX_QQ_ELEMENTS + 1];
    UINT64 ct_patch_count = 0;

    // 5. tailをfoldの初期シードとしてaccスロットへ評価・link(za_compile_callのacc
    // 初期化に相当、ただしnil固定ではなくtailの実際の値を使う)。
    switch (tail_kind) {
        case QQ_TAIL_NIL:
            jit_movabs_rax(nil);
            break;
        case QQ_TAIL_CONST: {
            za_operand_t tail_leaf;
            if (!za_classify_quoted_value(tail_data, &tail_leaf)) {
                return 0;
            }
            za_emit_operand(&tail_leaf);
            break;
        }
        case QQ_TAIL_UNQUOTE:
            if (!za_compile_operand(tail_data, params, fixed_count, locals, syms, env, trampoline_offset, nlx_depth,
                                     tb_ctx, call_depth, arith_depth)) {
                return 0;
            }
            break;
    }
    ct_patches[ct_patch_count++] = za_emit_ct_check_and_jmp_if_transfer();
    za_store_slot(ZA_REG_RAX, za_qq_acc_val_off(qq_depth));
    za_emit_gc_link_slot(za_qq_acc_val_off(qq_depth), za_qq_acc_node_off(qq_depth));

    // 6. 各要素を左から順に評価し、要素スロットへ格納・link(za_compile_callの手順2と
    // 同じ、制御転送はabort_cleanupへ合流する)。ELEMENTはこの関数自身をqq_depth+1で
    // 再帰し、SPLICEはza_compile_operandで直接評価する(上記doc comment参照)。
    for (UINT64 i = 0; i < item_count; i++) {
        int ok;
        if (items[i].is_splice) {
            ok = za_compile_operand(items[i].data, params, fixed_count, locals, syms, env, trampoline_offset,
                                     nlx_depth, tb_ctx, call_depth, arith_depth);
        } else {
            ok = za_compile_quasiquote(items[i].data, params, fixed_count, locals, syms, env, trampoline_offset,
                                        nlx_depth, tb_ctx, call_depth, arith_depth, qq_depth + 1);
        }
        if (!ok) {
            return 0;
        }
        ct_patches[ct_patch_count++] = za_emit_ct_check_and_jmp_if_transfer();
        za_store_slot(ZA_REG_RAX, za_qq_elem_val_off(qq_depth, i));
        za_emit_gc_link_slot(za_qq_elem_val_off(qq_depth, i), za_qq_elem_node_off(qq_depth, i));
    }

    // abort_cleanup: tail/要素評価中の制御転送で中断した場合のみ到達する(通常経路は
    // 直後のjmpで読み飛ばす。za_compile_callのabort_cleanupと同型)。
    UINT64 abort_skip_patch = jit_emit_jmp_rel32_placeholder();
    UINT64 abort_cleanup_offset = g_jit_used;
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    za_load_slot(ZA_REG_RCX, za_qq_saved_head_off(qq_depth));
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_unlink);
    jit_call_r11();
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);
    UINT64 abort_to_end_patch = jit_emit_jmp_rel32_placeholder();
    jit_patch_rel32(abort_skip_patch);
    for (UINT64 i = 0; i < ct_patch_count; i++) {
        jit_patch_rel32_target(ct_patches[i], abort_cleanup_offset);
    }

    // 7. 右から左へfoldする(ELEMENTはos_make_cons、SPLICEはqq_append)。
    for (UINT64 i = item_count; i > 0; i--) {
        za_load_slot(ZA_REG_RCX, za_qq_elem_val_off(qq_depth, i - 1));
        za_load_slot(ZA_REG_RDX, za_qq_acc_val_off(qq_depth));
        void *combinator = items[i - 1].is_splice ? (void *)qq_append : (void *)os_make_cons;
        jit_movabs_reg(ZA_REG_R11, (UINT64)combinator);
        jit_call_r11();
        za_store_slot(ZA_REG_RAX, za_qq_acc_val_off(qq_depth));
    }

    // 8. リンクをまとめて外し、accスロットから最終結果をraxへ読み直す(za_gc_unlink自体
    // もCALL経由でraxを破壊するため)。
    za_load_slot(ZA_REG_RCX, za_qq_saved_head_off(qq_depth));
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_unlink);
    jit_call_r11();
    za_load_slot(ZA_REG_RAX, za_qq_acc_val_off(qq_depth));

    jit_patch_rel32(abort_to_end_patch);
    return 1;
}

static int za_compile_body_forms(lisp_val_t forms, lisp_val_t params, UINT64 fixed_count,
                                  const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env,
                                  UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 trampoline_offset,
                                  UINT64 arith_depth) {
    if (forms == nil) {
        jit_movabs_rax(nil);
        return 1;
    }
    // 各要素(最後を除く)が制御転送ならこの共通着地点へ合流し、rax=その転送値のまま
    // 戻る。要素数はZA_MAX_OPERANDSと同じ規模の上限で十分(既存の他構文と同じ考え方)。
    UINT64 end_patches[ZA_MAX_OPERANDS];
    UINT64 end_patch_count = 0;
    for (lisp_val_t rest = forms; rest != nil; rest = cc_cdr(rest)) {
        if ((rest & TAG_MASK) != TAG_CONS) {
            return 0;
        }
        lisp_val_t elem = cc_car(rest);
        int is_last = (cc_cdr(rest) == nil);
        if (!za_compile_expr(elem, params, fixed_count, locals, syms, env, 0, trampoline_offset, nlx_depth, tb_ctx,
                              call_depth, arith_depth)) {
            return 0;
        }
        if (!is_last) {
            if (end_patch_count >= ZA_MAX_OPERANDS) {
                return 0;
            }
            end_patches[end_patch_count++] = za_emit_ct_check_and_jmp_if_transfer();
        }
    }
    for (UINT64 i = 0; i < end_patch_count; i++) {
        jit_patch_rel32(end_patches[i]);
    }
    return 1;
}

static int za_compile_block(lisp_val_t form, lisp_val_t params, UINT64 fixed_count, const za_local_scope_t *locals,
                             const za_syms_t *syms, lisp_val_t env, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx,
                             UINT64 call_depth, UINT64 trampoline_offset, UINT64 arith_depth) {
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    lisp_val_t name = cc_car(rest);
    if ((name & TAG_MASK) != TAG_SYMBOL && name != nil) {
        return 0;
    }
    lisp_val_t body = cc_cdr(rest);

    if (!za_compile_body_forms(body, params, fixed_count, locals, syms, env, nlx_depth, tb_ctx, call_depth,
                                trampoline_offset, arith_depth)) {
        return 0;
    }

    // 結果が制御転送でなければ(通常経路)そのまま返す。制御転送ならobj[1]==nameを
    // チェックしてobj[2]を返す(eval_blockと同じ。obj[0]のmagicはチェックしない —
    // eval_blockも同様にチェックしていないため、throw/goのタグがたまたまnameと
    // 同じシンボルの場合もここで捕捉される)。
    UINT64 ct_patch = za_emit_ct_check_and_jmp_if_transfer();
    UINT64 skip_patch = jit_emit_jmp_rel32_placeholder();

    jit_patch_rel32(ct_patch);
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    // name==nilの場合、nilは固定の非移動セル(g_nil_cell)であり実行時にGCで移動しない
    // ため、通常のシンボルのように名前文字列からos_make_symbolで都度再解決する必要が
    // なく、コンパイル時の値をそのまま即値として埋め込める(za_compile_return_fromの
    // 対応する分岐と同じ理由)。
    if (name == nil) {
        jit_movabs_reg(ZA_REG_R11, nil);
    } else {
        UINT64 name_off = za_emit_symbol_name(name);
        jit_movabs_self_ref(ZA_REG_RCX, name_off);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_symbol);
        jit_call_r11();
        jit_mov_reg_reg(ZA_REG_R11, ZA_REG_RAX);
    }

    za_emit_untag_instance(ZA_REG_RCX, ZA_REG_R13);
    jit_mov_reg_from_mem_disp8(ZA_REG_RAX, ZA_REG_RCX, 8);
    jit_cmp_rax_r11();
    UINT64 je_match = jit_emit_je_rel32_placeholder();
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);
    UINT64 jmp_no_match = jit_emit_jmp_rel32_placeholder();

    jit_patch_rel32(je_match);
    jit_mov_reg_from_mem_disp8(ZA_REG_RAX, ZA_REG_RCX, 16);

    jit_patch_rel32(jmp_no_match);
    jit_patch_rel32(skip_patch);
    return 1;
}

static int za_compile_return_from(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                                   const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env,
                                   UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 trampoline_offset,
                                   UINT64 arith_depth) {
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    lisp_val_t name = cc_car(rest);
    if ((name & TAG_MASK) != TAG_SYMBOL && name != nil) {
        return 0;
    }
    lisp_val_t value_rest = cc_cdr(rest);
    if (value_rest != nil && (value_rest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    if (nlx_depth >= ZA_MAX_NLX_DEPTH) {
        return 0;
    }

    if (value_rest != nil) {
        if (!za_compile_expr(cc_car(value_rest), params, fixed_count, locals, syms, env, 0, trampoline_offset,
                              nlx_depth, tb_ctx, call_depth, arith_depth)) {
            return 0;
        }
    } else {
        jit_movabs_rax(nil);
    }

    // valが制御転送ならラップせずそのまま返す(eval_return_fromと同じ)。
    UINT64 ct_patch = za_emit_ct_check_and_jmp_if_transfer();

    // valをos_make_symbol呼び出し(ヒープ確保を伴いGCを起こしうる)を挟んで保護する
    // ため、NLXスロットへ格納してlinkする(r13への素の退避では不十分)。
    UINT64 val_off = za_nlx_val_off(nlx_depth);
    UINT64 node_off = za_nlx_node_off(nlx_depth);
    za_store_slot(ZA_REG_RAX, val_off);
    za_emit_gc_link_slot(val_off, node_off);

    // name==nilの場合はza_compile_blockと同じ理由でos_make_symbolによる再解決を
    // 省略し、nilの即値をそのまま埋め込む。
    if (name == nil) {
        jit_movabs_reg(ZA_REG_RDX, nil);
    } else {
        UINT64 name_off = za_emit_symbol_name(name);
        jit_movabs_self_ref(ZA_REG_RCX, name_off);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_symbol);
        jit_call_r11();
        jit_mov_reg_reg(ZA_REG_RDX, ZA_REG_RAX);
    }
    za_load_slot(ZA_REG_R8, val_off);
    jit_movabs_reg(ZA_REG_RCX, MAGIC_BLOCK_EXIT);
    jit_movabs_reg(ZA_REG_R9, nil);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_instance);
    jit_call_r11();

    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    za_emit_gc_unlink_slot(node_off);
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);

    jit_patch_rel32(ct_patch);
    return 1;
}

/**
 * `(dynamic name)`。nameは評価しない(quote相当)。os_get_dynamicで動的束縛
 * (g_dynamic_bindings、レキシカルなenvとは無関係の単一グローバルalist)から
 * 現在の値を読み取って返す(未束縛ならnil)。os_get_dynamicはcc_assoc_eqによる
 * 走査のみでヒープ確保しないため、os_make_symbol呼び出しを挟んでの保護は不要。
 */
static int za_compile_dynamic(lisp_val_t form) {
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS || cc_cdr(rest) != nil) {
        return 0;
    }
    lisp_val_t name = cc_car(rest);
    if ((name & TAG_MASK) != TAG_SYMBOL) {
        return 0;
    }

    UINT64 name_off = za_emit_symbol_name(name);
    jit_movabs_self_ref(ZA_REG_RCX, name_off);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_symbol);
    jit_call_r11();
    jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_RAX);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_get_dynamic);
    jit_call_r11();
    return 1;
}

/**
 * `(defdynamic name value-form)`。value-formを評価し、結果が制御転送でなければ
 * os_set_dynamicで動的束縛(g_dynamic_bindings)へ登録してnameを返す(制御転送なら
 * os_set_dynamicを呼ばずそのまま返す。eval_defdynamicと同じ規約 — os_set_dynamic
 * 自身の戻り値はvalなので、最終的な戻り値としてはそちらを使わない)。
 * valをos_set_dynamic呼び出し(既存束縛が無い場合os_make_consでヒープ確保しGCを
 * 起こしうる)を挟んで保護するため、NLXスロットへ格納してlink/unlinkする
 * (return-fromと同じ手口。defdynamic自身は非局所脱出構文ではないのでnlx_depthは
 * 変えずそのまま借用する)。os_set_dynamic呼び出し後にnameを戻り値として使う際は、
 * 呼び出し前のnameの生レジスタ退避に頼らず、name_offのバイト列から再度
 * os_make_symbolして現在有効なタグ付きシンボル値を取り直す
 * (za_emit_symbol_nameのコメントと同じ理由。symbolオブジェクトもGCで移動する)。
 */
static int za_compile_defdynamic(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                                  const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env,
                                  UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 trampoline_offset,
                                  UINT64 arith_depth) {
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    lisp_val_t name = cc_car(rest);
    if ((name & TAG_MASK) != TAG_SYMBOL) {
        return 0;
    }
    lisp_val_t rest2 = cc_cdr(rest);
    if (rest2 == nil || (rest2 & TAG_MASK) != TAG_CONS || cc_cdr(rest2) != nil) {
        return 0;
    }
    if (nlx_depth >= ZA_MAX_NLX_DEPTH) {
        return 0;
    }

    if (!za_compile_expr(cc_car(rest2), params, fixed_count, locals, syms, env, 0, trampoline_offset, nlx_depth,
                          tb_ctx, call_depth, arith_depth)) {
        return 0;
    }

    // valが制御転送ならos_set_dynamicを呼ばずそのまま返す(eval_defdynamicと同じ)。
    UINT64 ct_patch = za_emit_ct_check_and_jmp_if_transfer();

    UINT64 val_off = za_nlx_val_off(nlx_depth);
    UINT64 node_off = za_nlx_node_off(nlx_depth);
    za_store_slot(ZA_REG_RAX, val_off);
    za_emit_gc_link_slot(val_off, node_off);

    UINT64 name_off = za_emit_symbol_name(name);
    jit_movabs_self_ref(ZA_REG_RCX, name_off);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_symbol);
    jit_call_r11();
    jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_RAX);
    za_load_slot(ZA_REG_RDX, val_off);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_set_dynamic);
    jit_call_r11();

    za_emit_gc_unlink_slot(node_off);

    // 戻り値はos_set_dynamicの戻り値(val)ではなくname自身。上のos_set_dynamic呼び出しで
    // GCが起きている可能性があるため、同じname_offのバイト列から再度os_make_symbolして
    // 現在有効なタグ付きシンボル値を取り直す。
    jit_movabs_self_ref(ZA_REG_RCX, name_off);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_symbol);
    jit_call_r11();

    jit_patch_rel32(ct_patch);
    return 1;
}

/**
 * 拡張9: `(setq sym val-form)`。let-localのみ対応する(v1スコープ)。外側defunの
 * 固定引数・グローバル変数・dynamic変数への再代入は書き込み可能なスロットが無いため
 * 非対応でfallbackする(za_local_lookupが失敗し0を返す)。
 * let-localのスロットはza_compile_let(束縛時)で既にza_emit_gc_link_slotによりアドレス
 * リンク済みなので、val-formの評価結果を同じスロットへ上書きするだけでよく、
 * 再リンクは不要(GCスキャンはリンクされたアドレスを都度dereferenceするため)。
 * val-form評価と書き込みの間にヒープ確保を伴う呼び出しは挟まないため、defdynamic/
 * return-fromのようなNLXスロット経由の退避保護も不要。
 * g_za_saw_setq_localをセットする副作用については、g_za_saw_escaping_lambdaの
 * コメントおよびza_try_compile_defun末尾のチェックを参照。
 */
static int za_compile_setq(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                            const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env,
                            UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 trampoline_offset,
                            UINT64 arith_depth) {
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    lisp_val_t sym = cc_car(rest);
    if ((sym & TAG_MASK) != TAG_SYMBOL) {
        return 0;
    }
    lisp_val_t rest2 = cc_cdr(rest);
    if (rest2 == nil || (rest2 & TAG_MASK) != TAG_CONS || cc_cdr(rest2) != nil) {
        return 0;
    }
    lisp_val_t val_form = cc_car(rest2);

    UINT32 val_off;
    if (!za_local_lookup(locals, sym, &val_off)) {
        return 0;
    }

    if (!za_compile_expr(val_form, params, fixed_count, locals, syms, env, 0, trampoline_offset, nlx_depth, tb_ctx,
                          call_depth, arith_depth)) {
        return 0;
    }

    // val-formが制御転送(return-from/throw/go)ならスロットへ書き込まずそのまま伝播する。
    UINT64 ct_patch = za_emit_ct_check_and_jmp_if_transfer();
    za_store_slot(ZA_REG_RAX, val_off);
    jit_patch_rel32(ct_patch);

    g_za_saw_setq_local = 1;
    return 1;
}

static int za_compile_catch(lisp_val_t form, lisp_val_t params, UINT64 fixed_count, const za_local_scope_t *locals,
                             const za_syms_t *syms, lisp_val_t env, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx,
                             UINT64 call_depth, UINT64 trampoline_offset, UINT64 arith_depth) {
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    lisp_val_t tag_form = cc_car(rest);
    lisp_val_t body = cc_cdr(rest);
    if (nlx_depth >= ZA_MAX_NLX_DEPTH) {
        return 0;
    }

    if (!za_compile_expr(tag_form, params, fixed_count, locals, syms, env, 0, trampoline_offset, nlx_depth, tb_ctx,
                          call_depth, arith_depth)) {
        return 0;
    }
    // tagが制御転送ならbodyへ入らずそのまま返す(eval_catchと同じ)。
    UINT64 tag_ct_patch = za_emit_ct_check_and_jmp_if_transfer();

    UINT64 tag_val_off = za_nlx_val_off(nlx_depth);
    UINT64 tag_node_off = za_nlx_node_off(nlx_depth);
    za_store_slot(ZA_REG_RAX, tag_val_off);
    za_emit_gc_link_slot(tag_val_off, tag_node_off);

    if (!za_compile_body_forms(body, params, fixed_count, locals, syms, env, nlx_depth + 1, tb_ctx, call_depth,
                                trampoline_offset, arith_depth)) {
        return 0;
    }

    // 結果がTAG_INSTANCEでobj[0]==MAGIC_CATCH_EXITかつobj[1]==tag(スロットの現在値)
    // ならobj[2]を、そうでなければ結果をそのまま返す(eval_catchと同じ)。
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    jit_and_reg_imm8(ZA_REG_RAX, (UINT8)TAG_MASK);
    jit_movabs_r11(TAG_INSTANCE);
    jit_cmp_rax_r11();
    UINT64 je_is_instance = jit_emit_je_rel32_placeholder();
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);
    UINT64 jmp_not_instance = jit_emit_jmp_rel32_placeholder();

    jit_patch_rel32(je_is_instance);
    za_emit_untag_instance(ZA_REG_RCX, ZA_REG_R13);
    jit_mov_reg_from_mem_disp8(ZA_REG_RAX, ZA_REG_RCX, 0);
    jit_movabs_r11(MAGIC_CATCH_EXIT);
    jit_cmp_rax_r11();
    UINT64 je_magic = jit_emit_je_rel32_placeholder();
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);
    UINT64 jmp_not_magic = jit_emit_jmp_rel32_placeholder();

    jit_patch_rel32(je_magic);
    za_load_slot(ZA_REG_R11, tag_val_off);
    jit_mov_reg_from_mem_disp8(ZA_REG_RAX, ZA_REG_RCX, 8);
    jit_cmp_rax_r11();
    UINT64 je_tag = jit_emit_je_rel32_placeholder();
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);
    UINT64 jmp_not_tag = jit_emit_jmp_rel32_placeholder();

    jit_patch_rel32(je_tag);
    jit_mov_reg_from_mem_disp8(ZA_REG_RAX, ZA_REG_RCX, 16);

    jit_patch_rel32(jmp_not_instance);
    jit_patch_rel32(jmp_not_magic);
    jit_patch_rel32(jmp_not_tag);

    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    za_emit_gc_unlink_slot(tag_node_off);
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);

    jit_patch_rel32(tag_ct_patch);
    return 1;
}

static int za_compile_throw(lisp_val_t form, lisp_val_t params, UINT64 fixed_count, const za_local_scope_t *locals,
                             const za_syms_t *syms, lisp_val_t env, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx,
                             UINT64 call_depth, UINT64 trampoline_offset, UINT64 arith_depth) {
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    lisp_val_t tag_form = cc_car(rest);
    lisp_val_t rest2 = cc_cdr(rest);
    if (rest2 == nil || (rest2 & TAG_MASK) != TAG_CONS || cc_cdr(rest2) != nil) {
        return 0;
    }
    lisp_val_t result_form = cc_car(rest2);
    if (nlx_depth >= ZA_MAX_NLX_DEPTH) {
        return 0;
    }

    if (!za_compile_expr(tag_form, params, fixed_count, locals, syms, env, 0, trampoline_offset, nlx_depth, tb_ctx,
                          call_depth, arith_depth)) {
        return 0;
    }
    UINT64 tag_ct_patch = za_emit_ct_check_and_jmp_if_transfer();

    UINT64 tag_val_off = za_nlx_val_off(nlx_depth);
    UINT64 tag_node_off = za_nlx_node_off(nlx_depth);
    za_store_slot(ZA_REG_RAX, tag_val_off);
    za_emit_gc_link_slot(tag_val_off, tag_node_off);

    if (!za_compile_expr(result_form, params, fixed_count, locals, syms, env, 0, trampoline_offset, nlx_depth + 1,
                          tb_ctx, call_depth, arith_depth)) {
        return 0;
    }

    // resultが制御転送ならtagのlinkだけ外してそのまま返す。そうでなければtagを外して
    // MAGIC_CATCH_EXITでラップする(いずれもeval_throwと同じ)。
    UINT64 result_ct_patch = za_emit_ct_check_and_jmp_if_transfer();
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    za_emit_gc_unlink_slot(tag_node_off);
    za_load_slot(ZA_REG_RDX, tag_val_off);
    jit_mov_reg_reg(ZA_REG_R8, ZA_REG_R13);
    jit_movabs_reg(ZA_REG_RCX, MAGIC_CATCH_EXIT);
    jit_movabs_reg(ZA_REG_R9, nil);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_instance);
    jit_call_r11();
    UINT64 skip_ct_patch = jit_emit_jmp_rel32_placeholder();

    jit_patch_rel32(result_ct_patch);
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    za_emit_gc_unlink_slot(tag_node_off);
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);

    jit_patch_rel32(skip_ct_patch);
    jit_patch_rel32(tag_ct_patch);
    return 1;
}

/** za_rewrite_fn_refsの前方宣言(body-list版と相互再帰する)。 */
static lisp_val_t za_rewrite_fn_refs(lisp_val_t form, lisp_val_t env, const za_fn_scope_t *scope, int *ok);
/** za_rewrite_binding_list(入れ子flet/labelsのbindings書き換え)の前方宣言。 */
static lisp_val_t za_rewrite_binding_list(lisp_val_t bindings, lisp_val_t env, const za_fn_scope_t *scope, int *ok);

/**
 * formsを「progn的に評価される式の並び」(labels束縛bodyそのもの、flet/labels
 * (入れ子含む)のbody部分)として扱い、各要素をza_rewrite_fn_refsで書き換えた上で
 * os_make_consで新しいリストへ組み直す。コンパイル時アロケーション
 * (os_make_cons)を伴うため、za_compile_call等と同じ規約でGC_PROTECTしながら進める。
 * @param ok 失敗時(*okが元々1でも)0を書く。呼び出し元はこの場合戻り値を無視して
 * fallbackする。
 */
static lisp_val_t za_rewrite_body_list(lisp_val_t forms, lisp_val_t env, const za_fn_scope_t *scope, int *ok) {
    if (forms == nil) {
        return nil;
    }
    if ((forms & TAG_MASK) != TAG_CONS) {
        *ok = 0;
        return nil;
    }
    GC_PROTECT(forms);
    lisp_val_t first = za_rewrite_fn_refs(cc_car(forms), env, scope, ok);
    if (!*ok) {
        return nil;
    }
    GC_PROTECT(first);
    lisp_val_t rest = za_rewrite_body_list(cc_cdr(forms), env, scope, ok);
    if (!*ok) {
        return nil;
    }
    return os_make_cons(first, rest);
}

/**
 * 入れ子flet/labelsのbindings((name params . body) ...)を書き換える。nameとparamsは
 * 呼び出しhead位置・値位置のいずれでもない別namespace(それぞれ「これから新規束縛する
 * 名前」「仮引数リスト」)なので変更せず、各bindingのbodyのみza_rewrite_body_listで
 * 同じscope(=呼び出し元、まだ入れ子自身の新しい束縛は含まない)を保って再帰する。
 */
static lisp_val_t za_rewrite_binding_list(lisp_val_t bindings, lisp_val_t env, const za_fn_scope_t *scope, int *ok) {
    if (bindings == nil) {
        return nil;
    }
    if ((bindings & TAG_MASK) != TAG_CONS) {
        *ok = 0;
        return nil;
    }
    GC_PROTECT(bindings);
    lisp_val_t binding = cc_car(bindings);
    if ((binding & TAG_MASK) != TAG_CONS) {
        *ok = 0;
        return nil;
    }
    lisp_val_t name = cc_car(binding);
    lisp_val_t brest = cc_cdr(binding);
    if ((brest & TAG_MASK) != TAG_CONS) {
        *ok = 0;
        return nil;
    }
    lisp_val_t binding_params = cc_car(brest);
    lisp_val_t binding_body = cc_cdr(brest);
    lisp_val_t new_body = za_rewrite_body_list(binding_body, env, scope, ok);
    if (!*ok) {
        return nil;
    }
    GC_PROTECT(name);
    GC_PROTECT(binding_params);
    GC_PROTECT(new_body);
    lisp_val_t new_binding = os_make_cons(name, os_make_cons(binding_params, new_body));
    GC_PROTECT(new_binding);
    lisp_val_t new_rest = za_rewrite_binding_list(cc_cdr(bindings), env, scope, ok);
    if (!*ok) {
        return nil;
    }
    return os_make_cons(new_binding, new_rest);
}

/**
 * 拡張(labels): labels束縛関数のbody(常にインタプリタ実行、JIT再帰しない)内部に
 * 現れる自己/相互再帰呼び出しを、scope内の名前をgensymへ差し替えることで解決可能に
 * するASTリライトパス。インタプリタの関数呼び出し解決(os_get_function、eval.c)は
 * シンボルの素の名前一致(global_environmentへのeq連想)しか見ないため、
 * このリライトを経ずに元のシンボル名(例: FACT)のままでは、za_compile_flet_labelsが
 * gensymキーでglobal_environmentへ登録した束縛を見つけられない(documents/jit.md、
 * za_fn_scope_tのコメント参照)。
 *
 * 書き換え対象は「呼び出しhead位置」と「(function name)のname位置」の2箇所のみ
 * (関数namespaceと変数namespaceは分離されており、lambda引数・let-local・&rest名等の
 * 値位置に現れる同名シンボルは無関係の変数参照なので書き換えない)。
 *
 * @param form 書き換え対象フォーム(defunの定義時ソースASTの一部、まだJIT非依存)
 * @param env マクロ展開に使う環境(defunの定義時環境、za_macroexpandへそのまま渡す)
 * @param scope 現在のlabels束縛群(入れ子であれば親スコープも含む)
 * @param ok 失敗時0を書く(束縛数上限超過、構文エラー、入れ子flet/labelsの
 * 名前衝突など)。この場合戻り値は不定でよく、呼び出し元はfallbackする。
 * @return 書き換え後のフォーム(新しいcons構造、コンパイル時アロケーション)
 */
static lisp_val_t za_rewrite_fn_refs(lisp_val_t form, lisp_val_t env, const za_fn_scope_t *scope, int *ok) {
    form = za_macroexpand(form, env);
    // nilはTAG_CONS(g_nil_cellへの自己参照)なので次のTAG_CONSチェックだけでは
    // 素通りしてしまい、cc_car(nil)=nilをheadとして扱った結果、一般呼び出し分岐
    // (headがシンボルでない場合の再帰)がza_rewrite_fn_refs(nil)を無限に再帰呼び出し
    // してCスタックを溢れさせる(za_compile_exprの同名コメント参照、同じ危険性)。
    // 書き換え不要な値位置の参照として、他のアトムと同じくここで先に捕まえる。
    if (form == nil) {
        return form;
    }
    if ((form & TAG_MASK) != TAG_CONS) {
        // 裸のシンボル・fixnum等。呼び出しhead位置・(function name)のname位置は
        // 呼び出し元(このforms自身がconsであるケース)が個別に処理するため、
        // ここに来るのは常に「値位置の参照」であり書き換え不要。
        return form;
    }

    lisp_val_t head = cc_car(form);
    if (head == g_sym_quote || head == g_sym_quasiquote) {
        return form;
    }

    GC_PROTECT(form);
    lisp_val_t rest = cc_cdr(form);

    if (head == g_sym_setq) {
        // (setq var value-form): 単一pair(eval_setq/za_compile_setqいずれも
        // 複数pair非対応、za_compile_setqのコメント参照)。varは別namespaceなので
        // 書き換えず、value-formのみ再帰する。
        if ((rest & TAG_MASK) != TAG_CONS) {
            *ok = 0;
            return form;
        }
        lisp_val_t var = cc_car(rest);
        lisp_val_t rest2 = cc_cdr(rest);
        if ((rest2 & TAG_MASK) != TAG_CONS || cc_cdr(rest2) != nil) {
            *ok = 0;
            return form;
        }
        lisp_val_t val_form = cc_car(rest2);
        lisp_val_t new_val = za_rewrite_fn_refs(val_form, env, scope, ok);
        if (!*ok) {
            return form;
        }
        GC_PROTECT(var);
        GC_PROTECT(new_val);
        return os_make_cons(head, os_make_cons(var, os_make_cons(new_val, nil)));
    }

    if (head == g_sym_function) {
        // (function name): nameがscope内の名前なら、動的extentを越えて
        // クロージャが使われる可能性を安全側に倒して無条件fallbackさせる
        // (g_za_saw_flet_labels_escapeのコメント、za_classify_operandの
        // 同名分岐と同じ思想 — こちらはJIT実行されないbody内部の参照なので
        // 別途この関数で検出する必要がある)。
        if ((rest & TAG_MASK) != TAG_CONS || cc_cdr(rest) != nil) {
            return form;
        }
        lisp_val_t target = cc_car(rest);
        if ((target & TAG_MASK) == TAG_SYMBOL) {
            const za_fn_binding_t *binding;
            // gensym_slot_addr==0(センチネル)は入れ子flet/labels自身の局所束縛名
            // (下のhead==g_sym_flet/g_sym_labels分岐参照)であり、実gensymへの解決を
            // 経ていない単なる「ここでシャドウされている」印。そのnameへの(function ...)
            // 参照はeval.cの通常のレキシカル評価がそのまま正しく処理できるため、
            // escapeフォールバックの対象外とする(実gensymを指すbinding、つまり
            // outer/ancestorスコープの実際に解決済みの束縛のみを対象とする)。
            if (za_fn_scope_lookup(scope, target, &binding) && binding->gensym_slot_addr != 0) {
                g_za_saw_flet_labels_escape = 1;
                *ok = 0;
                return form;
            }
        }
        return form;
    }

    if (head == g_sym_flet || head == g_sym_labels) {
        // 入れ子flet/labels: 内側で新規に束縛される各名前について、
        // gensym_slot_addr=0(未確定センチネル)の一時エントリを持つ新しい
        // za_fn_scope_tフレームを構築し、parentを現在のscopeにして積んだ上で
        // 内側のbindings/bodyを再帰する。センチネルは「ここで局所的にシャドウ
        // されているが、まだ実gensymへは解決しない」を意味し、この一時フレーム内で
        // 見つかった参照はza_fn_scope_lookup呼び出し元(gensym_slot_addr!=0チェック)
        // により書き換え対象から除外される。これにより、内側スコープの名前への
        // 参照は前処理時点では素通りし(実際の解決はeval.cの通常のレキシカル評価、
        // あるいはこの入れ子形式が「in body」位置でza_compile_flet_labelsにより
        // 再帰コンパイルされる際に行われる)、外側scope由来の同名参照だけが
        // 誤って書き換えられることを防げる。内外で名前が重複しない場合は従来通り
        // (センチネルがlookupで見つからず素通りするだけなので)動作は変わらない。
        if ((rest & TAG_MASK) != TAG_CONS) {
            *ok = 0;
            return form;
        }
        lisp_val_t bindings = cc_car(rest);
        lisp_val_t inner_body = cc_cdr(rest);

        za_fn_scope_t inner_scope;
        inner_scope.count = 0;
        inner_scope.parent = scope;
        for (lisp_val_t b = bindings; b != nil; b = cc_cdr(b)) {
            if ((b & TAG_MASK) != TAG_CONS) {
                *ok = 0;
                return form;
            }
            lisp_val_t binding = cc_car(b);
            if ((binding & TAG_MASK) != TAG_CONS) {
                *ok = 0;
                return form;
            }
            lisp_val_t name = cc_car(binding);
            if (inner_scope.count >= ZA_MAX_FLET_BINDINGS) {
                *ok = 0;
                return form;
            }
            inner_scope.bindings[inner_scope.count].orig_name = name;
            inner_scope.bindings[inner_scope.count].gensym_slot_addr = 0;
            inner_scope.count++;
        }
        lisp_val_t new_bindings = za_rewrite_binding_list(bindings, env, &inner_scope, ok);
        if (!*ok) {
            return form;
        }
        GC_PROTECT(new_bindings);
        lisp_val_t new_inner_body = za_rewrite_body_list(inner_body, env, &inner_scope, ok);
        if (!*ok) {
            return form;
        }
        return os_make_cons(head, os_make_cons(new_bindings, new_inner_body));
    }

    // 一般呼び出し、あるいはif/block/catch/tagbody等bodyを持つその他の特殊形式。
    // headがシンボルでscope内の名前ならgensymへ差し替える(呼び出しhead位置のみ)。
    // headがシンボルでない場合(letが展開された即時呼び出しlambda等、head自体が式)は
    // 差し替えず、他の要素と同様に再帰する。
    lisp_val_t new_head;
    if ((head & TAG_MASK) == TAG_SYMBOL) {
        new_head = head;
        const za_fn_binding_t *binding;
        // gensym_slot_addr==0(センチネル)は入れ子flet/labels自身の局所束縛名で、
        // まだ実gensymへ解決していないため書き換えない(下の入れ子flet/labels分岐参照)。
        if (za_fn_scope_lookup(scope, head, &binding) && binding->gensym_slot_addr != 0) {
            new_head = *binding->gensym_slot_addr;
        }
    } else {
        new_head = za_rewrite_fn_refs(head, env, scope, ok);
        if (!*ok) {
            return form;
        }
    }
    GC_PROTECT(new_head);
    lisp_val_t new_rest = za_rewrite_body_list(rest, env, scope, ok);
    if (!*ok) {
        return form;
    }
    return os_make_cons(new_head, new_rest);
}

/** GC_PROTECTは1個の名前付きローカル変数専用(トークン連結で内部変数名を作るため
 * 配列添字`arr[i]`のような式を渡せない)。za_compile_flet_labelsのname_syms/
 * binding_params/binding_bodies配列は、束縛数(binding_count、実行時に決まる)分の
 * 要素をgensym確保・za_rewrite_fn_refs呼び出し(いずれもos_make_cons等の実アロケーション
 * を伴う)の間、生存させ続ける必要があるため、この汎用ヘルパーで1要素ずつ手動で
 * shadow stackへlinkする(GC_PROTECTの内部実装と同じ「node->var_ptr、現在のgc_rootsを
 * nextに繋いで先頭を差し替える」手順を、配列分だけ繰り返すだけ)。 */
typedef struct {
    gc_rootnode *saved_head;
} za_gc_protect_batch_t;

static inline void za_gc_protect_batch_cleanup(za_gc_protect_batch_t *batch) {
    get_current_process()->gc_roots = batch->saved_head;
}

static inline void za_gc_protect_batch_push(gc_rootnode *node, lisp_val_t *var_ptr) {
    node->var_ptr = var_ptr;
    node->next = get_current_process()->gc_roots;
    get_current_process()->gc_roots = node;
}

/**
 * `(flet bindings . body)`/`(labels bindings . body)`。各bindingは`(name params . body)`。
 * 束縛関数の名前空間解決は常にgensym+global_environment経由(Design B、za_fn_scope_tの
 * コメント参照)で行い、変数namespaceは外側関数のenv/localsをそのまま使う。束縛関数の
 * 本体は常にインタプリタ実行(JIT再帰しない、za_compile_lambdaと同じ)なので、この
 * 関数がg_za_fn_scopeを読むのは実質za_compile_call/za_classify_operandがin-body側の
 * 呼び出しをgensym解決する場合のみであり、束縛body自身はここでコンパイルしない
 * (labelsの場合のみ、束縛body側の自己/相互再帰呼び出しをza_rewrite_fn_refsで
 * gensymへ書き換える。fletは束縛関数の本体が外側スコープをそのまま見るのが
 * 正しい意味論なので書き換えない、上のis_labels分岐参照)。
 *
 * 各bindingのクロージャは、外側の固定引数・let-IIFEローカルをコピーした1個の共有
 * キャプチャenv(za_emit_build_capture_env)を使い回す。eval_flet/eval_labelsは
 * bindingごとに別のnew_env(flet)/共通のnew_env(labels)を作るが、そのenvは元々
 * 「(1)束縛関数同士の関数namespace解決」と「(2)束縛関数から見える変数namespace」の
 * 両方を兼ねていた。(1)はgensym+global_environmentへ完全に置き換えたため、
 * (2)だけならbinding間で共有しても意味的な差が無い(どのクロージャもJITが再入
 * しないため、他のbindingのgensymを直接読む必要が無い)。
 *
 * 復元(save/restore)はza_compile_unwind_protectと同じnlx_depthカウンタ・スロットを
 * 共有する(return-from/throw/goがこの区間を飛び越えてもcleanupが必ず実行される
 * 必要があるのはunwind-protectと同一の要求のため)。cleanupは制御転送の種類に
 * 関わらず無条件に実行し、bodyの最後の要素をトランポリンへ末尾jmpさせるとこの
 * cleanupをバイパスしてしまうため、この関数はis_tailを取らない(za_compile_block/
 * za_compile_catch/za_compile_throw/za_compile_tagbodyと同じ理由)。
 * @return 対応できれば1、できなければ0
 */
static int za_compile_flet_labels(lisp_val_t form, int is_labels, lisp_val_t params, UINT64 fixed_count,
                                   const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env,
                                   UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth,
                                   UINT64 trampoline_offset, UINT64 arith_depth) {
    /* paramsはこの関数の後段(za_emit_build_capture_env内のコピーループ)で
     * cc_car/cc_cdrにより辿られるが、その前にgensym確保(os_make_uninterned_symbol)
     * ・lambdaスロット用os_make_cons等の実アロケーションを伴うコンパイル時呼び出しを
     * 行っており、GCが発火するとparamsの指す先が再配置される。呼び出し元が保持する
     * paramsはこの関数にとって値渡しの別コピーであり、呼び出し元側の保護はこの関数の
     * ローカル変数までは更新しないため、za_compile_call/za_compile_lambdaと同じ理由で
     * ここで明示的に保護する。 */
    GC_PROTECT(params);

    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    lisp_val_t bindings = cc_car(rest);
    lisp_val_t body = cc_cdr(rest);

    if (nlx_depth >= ZA_MAX_NLX_DEPTH) {
        return 0;
    }

    // bindings検証: 「(name params . body)」の形を持つ、重複を許す平坦なリスト
    // (仮引数リスト自体の検証はza_compile_lambdaと同じくここでは行わない — 束縛body
    // は常にインタプリタ実行であり、呼び出し時にインタプリタ側が検証するため)。
    lisp_val_t name_syms[ZA_MAX_FLET_BINDINGS];
    lisp_val_t binding_params[ZA_MAX_FLET_BINDINGS];
    lisp_val_t binding_bodies[ZA_MAX_FLET_BINDINGS];
    UINT64 binding_count = 0;
    for (lisp_val_t b = bindings; b != nil; b = cc_cdr(b)) {
        if ((b & TAG_MASK) != TAG_CONS) {
            return 0;
        }
        lisp_val_t binding = cc_car(b);
        if ((binding & TAG_MASK) != TAG_CONS) {
            return 0;
        }
        lisp_val_t name = cc_car(binding);
        if ((name & TAG_MASK) != TAG_SYMBOL) {
            return 0;
        }
        lisp_val_t brest = cc_cdr(binding);
        if (brest == nil || (brest & TAG_MASK) != TAG_CONS) {
            return 0;
        }
        if (binding_count >= ZA_MAX_FLET_BINDINGS) {
            return 0;
        }
        name_syms[binding_count] = name;
        binding_params[binding_count] = cc_car(brest);
        binding_bodies[binding_count] = cc_cdr(brest);
        binding_count++;
    }

    // ここから先はgensym確保(os_make_uninterned_symbol)・za_rewrite_fn_refs
    // (os_make_cons)等の実アロケーションを伴うコンパイル時呼び出しが続き、GCが
    // 起動する可能性がある。name_syms/binding_params/binding_bodiesはbinding_count個
    // すべてが以降の複数の処理(gensym割り当てループ、labelsのリライトループ、
    // クロージャ構築ループ)をまたいで生存する必要があるため、各要素を個別に
    // shadow stackへlinkして保護する(za_gc_protect_batch_tのコメント参照)。
    za_gc_protect_batch_t flet_gc_batch __attribute__((cleanup(za_gc_protect_batch_cleanup)));
    flet_gc_batch.saved_head = get_current_process()->gc_roots;
    gc_rootnode flet_gc_nodes[ZA_MAX_FLET_BINDINGS * 3];
    for (UINT64 i = 0; i < binding_count; i++) {
        za_gc_protect_batch_push(&flet_gc_nodes[i * 3 + 0], &name_syms[i]);
        za_gc_protect_batch_push(&flet_gc_nodes[i * 3 + 1], &binding_params[i]);
        za_gc_protect_batch_push(&flet_gc_nodes[i * 3 + 2], &binding_bodies[i]);
    }

    // 各bindingにgensymを確保し(g_za_quote_slotsプールを共有、za_fn_binding_tの
    // コメント参照)、in-body側の呼び出し解決用の新しいza_fn_scope_tを構築する。
    za_fn_scope_t new_scope;
    new_scope.count = binding_count;
    new_scope.parent = g_za_fn_scope;
    for (UINT64 i = 0; i < binding_count; i++) {
        lisp_val_t gensym = os_make_uninterned_symbol("FLET-FN");
        UINT64 slot_idx;
        if (!za_alloc_quote_slot(gensym, &slot_idx)) {
            return 0;
        }
        new_scope.bindings[i].orig_name = name_syms[i];
        new_scope.bindings[i].gensym_slot_addr = &g_za_quote_slots[slot_idx];
    }

    // labelsの場合のみ、各bindingのbodyをnew_scope(自分自身・兄弟bindingを含む)で
    // リライトし、本体内部の自己/相互再帰呼び出しをgensym経由で解決可能にする
    // (za_rewrite_fn_refsのコメント参照)。fletでは束縛関数の本体は外側スコープを
    // そのまま見るのが正しい意味論(eval_flet/eval_labelsの違い、この関数のdoc
    // comment冒頭参照)なのでリライトしない。
    if (is_labels) {
        int rewrite_ok = 1;
        for (UINT64 i = 0; i < binding_count; i++) {
            binding_bodies[i] = za_rewrite_body_list(binding_bodies[i], env, &new_scope, &rewrite_ok);
            if (!rewrite_ok) {
                return 0;
            }
        }
    }

    // 1./2. 全bindingで共有する1個のキャプチャenvを構築する(za_compile_lambdaと同型、
    // za_emit_build_capture_envのコメント参照)。関数namespaceはgensym+
    // global_environment経由なので、このenvには一切登録しない(変数namespace専用)。
    za_emit_build_capture_env(params, fixed_count, locals, ZA_OFF_FLET_SAVED_HEAD, ZA_OFF_FLET_ENV_VAL,
                               ZA_OFF_FLET_ENV_NODE, ZA_OFF_FLET_TMP_VAL, ZA_OFF_FLET_TMP_NODE);

    // 3. 各bindingにつき、クロージャを構築し(za_compile_lambda末尾のos_make_instance
    // 部分と同型)、gensymの現在のグローバル束縛を退避してから新しいクロージャへ
    // 差し替える。束縛関数は1個ずつ逐次構築して即os_set_functionで消費するため、
    // ZA_OFF_FLET_TMP_VAL/NODE(すでにza_emit_build_capture_env内でlink済み)を
    // そのままクロージャ退避用スクラッチとして使い回せる(za_compile_lambdaの
    // 兄弟lambdaが同じTMPスロットを再利用できる理由と同じ)。
    for (UINT64 i = 0; i < binding_count; i++) {
        UINT64 lambda_slot_idx;
        if (g_za_lambda_slot_free_count > 0) {
            lambda_slot_idx = g_za_lambda_slot_free[--g_za_lambda_slot_free_count];
        } else if (g_za_lambda_slot_count < ZA_MAX_LAMBDA_SLOTS) {
            lambda_slot_idx = g_za_lambda_slot_count++;
        } else {
            return 0;
        }
        g_za_lambda_slots[lambda_slot_idx] = os_make_cons(binding_params[i], binding_bodies[i]);
        os_gc_register_root(&g_za_lambda_slots[lambda_slot_idx]);
        lisp_val_t *lambda_slot_addr = &g_za_lambda_slots[lambda_slot_idx];
        za_track_literal_slot_alloc(lambda_slot_addr);
        lisp_val_t *gensym_slot_addr = new_scope.bindings[i].gensym_slot_addr;

        // クロージャ本体: os_make_instance(MAGIC_FUNCTION_INTERPRETED, binding_params,
        // binding_body, 共有キャプチャenv)。
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)lambda_slot_addr);
        jit_mov_reg_from_mem_disp8(ZA_REG_R13, ZA_REG_R11, 0); /* r13 = (binding_params . binding_body) 現在値 */
        jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_R13);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)cc_car);
        jit_call_r11();
        za_store_slot(ZA_REG_RAX, ZA_OFF_FLET_TMP_VAL); /* binding_paramsを一時退避 */

        jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_R13);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)cc_cdr);
        jit_call_r11();
        jit_mov_reg_reg(ZA_REG_R8, ZA_REG_RAX);          /* r8 = binding_body(=w2) */

        za_load_slot(ZA_REG_RDX, ZA_OFF_FLET_TMP_VAL);   /* rdx = binding_params(=w1) */
        za_load_slot(ZA_REG_R9, ZA_OFF_FLET_ENV_VAL);    /* r9 = 共有キャプチャenv(=w3) */
        jit_movabs_reg(ZA_REG_RCX, MAGIC_FUNCTION_INTERPRETED);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_instance);
        jit_call_r11();
        za_store_slot(ZA_REG_RAX, ZA_OFF_FLET_TMP_VAL); /* クロージャを退避(TMP_NODEでlink済み) */

        // 旧バインディング値を取得し、退避スロットへ保存・linkする(解決は常に
        // global_environmentに対して行う、Design B。gensymは名前再解決に乗せられない
        // ため、確保したスロットのアドレスをmovabsで埋め込み都度現在値を読む —
        // za_compile_callのfn_binding分岐と同じ手口)。
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)gensym_slot_addr);
        jit_mov_reg_from_mem_disp8(ZA_REG_RCX, ZA_REG_R11, 0);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)&global_environment);
        jit_mov_reg_from_mem_disp8(ZA_REG_RDX, ZA_REG_R11, 0);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_get_function);
        jit_call_r11();
        UINT64 old_val_off = za_flet_old_val_off(nlx_depth, i);
        UINT64 old_node_off = za_flet_old_node_off(nlx_depth, i);
        za_store_slot(ZA_REG_RAX, old_val_off);
        za_emit_gc_link_slot(old_val_off, old_node_off);

        // 新しいクロージャをgensym経由でglobal_environmentへ登録する。
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)gensym_slot_addr);
        jit_mov_reg_from_mem_disp8(ZA_REG_RCX, ZA_REG_R11, 0);
        za_load_slot(ZA_REG_RDX, ZA_OFF_FLET_TMP_VAL);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)&global_environment);
        jit_mov_reg_from_mem_disp8(ZA_REG_R8, ZA_REG_R11, 0);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_set_function);
        jit_call_r11();
    }

    // 4. in-body(bodyの評価)は新しいfn_scopeの下でコンパイルする。g_za_fn_scopeは
    // コンパイル時のみ読まれるC変数であり、このbody評価呼び出しの前後だけ差し替えて
    // 元に戻せば、Cの再帰呼び出しがレキシカルネストと1対1対応するため
    // パラメータスレッディングと意味的に等価になる(za_fn_scope_tのコメント参照)。
    // cleanupは制御転送の種類に関わらず無条件に実行するため(eval_unwind_protectと
    // 同じ簡略化)、nlx_depth+1でza_compile_body_formsを呼ぶ(unwind-protectの
    // cleanup-formsと同じ配線)。
    const za_fn_scope_t *saved_fn_scope = g_za_fn_scope;
    g_za_fn_scope = &new_scope;
    int body_ok = za_compile_body_forms(body, params, fixed_count, locals, syms, env, nlx_depth + 1, tb_ctx,
                                         call_depth, trampoline_offset, arith_depth);
    g_za_fn_scope = saved_fn_scope;
    if (!body_ok) {
        return 0;
    }

    UINT64 nlx_val_off = za_nlx_val_off(nlx_depth);
    UINT64 nlx_node_off = za_nlx_node_off(nlx_depth);
    za_store_slot(ZA_REG_RAX, nlx_val_off);
    za_emit_gc_link_slot(nlx_val_off, nlx_node_off);

    // 5. cleanup: 各bindingにつき、退避しておいた旧バインディング値でgensymの
    // global_environment登録を無条件に復元する(cleanup-formsの結果は捨てる、
    // eval_unwind_protectの既知の簡略化に合わせる)。
    for (UINT64 i = 0; i < binding_count; i++) {
        lisp_val_t *gensym_slot_addr = new_scope.bindings[i].gensym_slot_addr;
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)gensym_slot_addr);
        jit_mov_reg_from_mem_disp8(ZA_REG_RCX, ZA_REG_R11, 0);
        za_load_slot(ZA_REG_RDX, za_flet_old_val_off(nlx_depth, i));
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)&global_environment);
        jit_mov_reg_from_mem_disp8(ZA_REG_R8, ZA_REG_R11, 0);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_set_function);
        jit_call_r11();
    }

    // 6. bodyの結果を読み直し、flet/labelsスコープ開始時点までgc_rootsを一括unlinkする
    // (za_compile_letと同じ「saved_headを1回だけ捕捉し、末尾で一括unlink」パターン。
    // 共有キャプチャenvの2ノード+bindingごとの旧値ノード+このnlxノードが、いずれも
    // ZA_OFF_FLET_SAVED_HEAD捕捉後にlinkされた同じGCルートスタック上に積まれている
    // ため、1回のza_gc_unlinkでまとめて解除できる)。
    za_load_slot(ZA_REG_RAX, nlx_val_off);
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    za_load_slot(ZA_REG_RCX, ZA_OFF_FLET_SAVED_HEAD);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_unlink);
    jit_call_r11();
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);
    return 1;
}

static int za_compile_unwind_protect(lisp_val_t form, lisp_val_t params, UINT64 fixed_count,
                                      const za_local_scope_t *locals, const za_syms_t *syms, lisp_val_t env,
                                      UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx, UINT64 call_depth, UINT64 trampoline_offset,
                                      UINT64 arith_depth) {
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    lisp_val_t protected_form = cc_car(rest);
    lisp_val_t cleanup_forms = cc_cdr(rest);
    if (nlx_depth >= ZA_MAX_NLX_DEPTH) {
        return 0;
    }

    // protected-formの評価結果は制御転送かどうかに関わらずcleanupへ進む
    // (eval_unwind_protectと同じ。ここでは即時チェックを行わない)。
    // nlx_depth+1で評価する: protected_form内のgoがこのunwind-protectのスパンを
    // 飛び越えてcleanup-formsの実行をバイパスすることをbase_nlx_depthチェックで
    // 確実に拒否させるため(cleanup-formsと同じ深さで評価する)。
    if (!za_compile_expr(protected_form, params, fixed_count, locals, syms, env, 0, trampoline_offset, nlx_depth + 1,
                          tb_ctx, call_depth, arith_depth)) {
        return 0;
    }

    UINT64 val_off = za_nlx_val_off(nlx_depth);
    UINT64 node_off = za_nlx_node_off(nlx_depth);
    za_store_slot(ZA_REG_RAX, val_off);
    za_emit_gc_link_slot(val_off, node_off);

    // cleanup-formsの結果は捨てる(cleanup内で新たな脱出が起きてもここでは無視する —
    // eval_unwind_protectの既知の簡略化に合わせる)。
    if (!za_compile_body_forms(cleanup_forms, params, fixed_count, locals, syms, env, nlx_depth + 1, tb_ctx,
                                call_depth, trampoline_offset, arith_depth)) {
        return 0;
    }

    za_load_slot(ZA_REG_RAX, val_off);
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    za_emit_gc_unlink_slot(node_off);
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);
    return 1;
}

/** tagbody本体1個あたりの(タグを除く)form要素数の上限。goが無いプレーンなbody要素も
 * 含めて数える(コード量を有限に保つための実装上の上限。ZA_MAX_OPERANDS等と同じ考え方)。 */
#define ZA_MAX_TAGBODY_FORMS 64

static int za_compile_tagbody(lisp_val_t form, lisp_val_t params, UINT64 fixed_count, const za_local_scope_t *locals,
                               const za_syms_t *syms, lisp_val_t env, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx,
                               UINT64 call_depth, UINT64 trampoline_offset, UINT64 arith_depth) {
    lisp_val_t body = cc_cdr(form);

    za_tagbody_ctx_t new_ctx;
    new_ctx.tag_count = 0;
    new_ctx.base_nlx_depth = nlx_depth;

    // 各form要素(タグを除く)の評価結果が制御転送なら(自分のタグへのgoはza_compile_go
    // が直接jmpで解決済みなので、ここに到達するのは他のcatch/block/goのみ)tagbody
    // 全体の結果としてそのまま伝播する(eval_tagbodyの「一致しなければ伝播」と同じ)。
    UINT64 end_patches[ZA_MAX_TAGBODY_FORMS];
    UINT64 end_patch_count = 0;

    for (lisp_val_t rest = body; rest != nil; rest = cc_cdr(rest)) {
        if ((rest & TAG_MASK) != TAG_CONS) {
            return 0;
        }
        lisp_val_t elem = cc_car(rest);
        if (elem != nil && (elem & TAG_MASK) == TAG_SYMBOL) {
            int idx = -1;
            for (int i = 0; i < new_ctx.tag_count; i++) {
                if (new_ctx.tags[i].tag == elem) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0) {
                if (new_ctx.tag_count >= ZA_MAX_TAGBODY_TAGS) {
                    return 0;
                }
                idx = new_ctx.tag_count++;
                new_ctx.tags[idx].tag = elem;
                new_ctx.tags[idx].resolved = 0;
                new_ctx.tags[idx].pending_count = 0;
            }
            new_ctx.tags[idx].offset = g_jit_used;
            new_ctx.tags[idx].resolved = 1;
            for (int i = 0; i < new_ctx.tags[idx].pending_count; i++) {
                jit_patch_rel32_target(new_ctx.tags[idx].pending[i], new_ctx.tags[idx].offset);
            }
            new_ctx.tags[idx].pending_count = 0;
            continue;
        }

        if (end_patch_count >= ZA_MAX_TAGBODY_FORMS) {
            return 0;
        }
        if (!za_compile_expr(elem, params, fixed_count, locals, syms, env, 0, trampoline_offset, nlx_depth, &new_ctx,
                              call_depth, arith_depth)) {
            return 0;
        }
        end_patches[end_patch_count++] = za_emit_ct_check_and_jmp_if_transfer();
    }

    // 未解決の前方参照が残っていたら(=body中に無いタグへのgo)コンパイル断念。
    for (int i = 0; i < new_ctx.tag_count; i++) {
        if (new_ctx.tags[i].pending_count > 0) {
            return 0;
        }
    }

    // 最後まで正常終了した場合は常にnil(eval_tagbodyと同じ)。
    jit_movabs_rax(nil);

    for (UINT64 i = 0; i < end_patch_count; i++) {
        jit_patch_rel32(end_patches[i]);
    }
    return 1;
}

static int za_compile_go(lisp_val_t form, UINT64 nlx_depth, za_tagbody_ctx_t *tb_ctx) {
    if (tb_ctx == 0 || nlx_depth != tb_ctx->base_nlx_depth) {
        return 0;
    }
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS || cc_cdr(rest) != nil) {
        return 0;
    }
    lisp_val_t tag = cc_car(rest);
    if ((tag & TAG_MASK) != TAG_SYMBOL) {
        return 0;
    }

    int idx = -1;
    for (int i = 0; i < tb_ctx->tag_count; i++) {
        if (tb_ctx->tags[i].tag == tag) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        if (tb_ctx->tag_count >= ZA_MAX_TAGBODY_TAGS) {
            return 0;
        }
        idx = tb_ctx->tag_count++;
        tb_ctx->tags[idx].tag = tag;
        tb_ctx->tags[idx].resolved = 0;
        tb_ctx->tags[idx].pending_count = 0;
    }

    if (tb_ctx->tags[idx].resolved) {
        jit_emit_jmp_to(tb_ctx->tags[idx].offset);
    } else {
        if (tb_ctx->tags[idx].pending_count >= ZA_MAX_TAGBODY_GOTOS_PER_TAG) {
            return 0;
        }
        tb_ctx->tags[idx].pending[tb_ctx->tags[idx].pending_count++] = jit_emit_jmp_rel32_placeholder();
    }
    return 1;
}

lisp_val_t za_try_compile_defun(lisp_val_t params, lisp_val_t body, lisp_val_t env) {
#ifdef ISIKIOS_UNIT_TEST
    // za.cが出力する機械語は実機ビルド(mingw-gcc, MS x64 ABI, 実行可能メモリ)を前提と
    // しており、ネイティブgccでビルドするユニットテストではABI/メモリ保護が異なるため
    // 常にインタプリタへフォールバックする。コンパイル結果の検証はtest/lisp/za_test.lisp
    // (make test-qemu)で実機上で行う。
    return nil;
#endif

    UINT64 fixed_count;
    if (!za_validate_params(params, &fixed_count)) {
        return nil;
    }

    if (body == nil || (body & TAG_MASK) != TAG_CONS || cc_cdr(body) != nil) {
        return nil;
    }
    lisp_val_t form = cc_car(body);
    za_syms_t syms;
    syms.plus = os_make_symbol("+");
    syms.minus = os_make_symbol("-");
    syms.star = os_make_symbol("*");
    syms.lt = os_make_symbol("<");
    syms.eq = os_make_symbol("=");
    syms.gt = os_make_symbol(">");
    syms.le = os_make_symbol("<=");
    syms.ge = os_make_symbol(">=");
    syms.eqp = os_make_symbol("EQ");
    syms.nullsym = os_make_symbol("NULL");
    syms.atom = os_make_symbol("ATOM");

    g_jit_overflow = 0;
    g_za_saw_setq_local = 0;
    g_za_saw_escaping_lambda = 0;
    g_za_saw_flet_labels_escape = 0;
    g_jit_reloc_count = 0;
    g_jit_trampoline_jmp_count = 0;
    // Phase3.6: このコンパイル試行で確保するリテラルスロット(quote/number/lambda)の
    // 一覧をリセットする。失敗exitではza_release_literal_slot_allocsで即座にフリーリストへ
    // 返却し、成功時はos_environment_register_literal_slotでenvへ登録する。
    g_za_literal_slot_alloc_count = 0;
    // トランポリンは全JIT関数で共有するため、今回のコンパイル対象用にentryを記録する
    // より前に確定させる(コンパイル失敗時のg_jit_used巻き戻しでスタブ自体が失われない
    // ようにするため)。
    UINT64 trampoline_offset = za_ensure_trampoline();
    if (g_jit_overflow) {
        za_release_literal_slot_allocs();
        return nil;
    }

    UINT64 entry = g_jit_used;

    // プロローグ: rbx/r13を退避し、MS x64呼び出し規約のシャドウスペース+拡張3用の
    // フレーム(env/args/fn/acc/引数スロットとそれぞれのgc_rootnode)を確保する。
    jit_push_rbx();
    jit_push_r13();
    jit_sub_rsp_imm32(ZA_FRAME_TOTAL);
    za_store_slot(ZA_REG_RCX, ZA_OFF_ARGS_VAL);
    za_store_slot(ZA_REG_RDX, ZA_OFF_ENV_VAL);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_current_head);
    jit_call_r11();
    za_store_slot(ZA_REG_RAX, ZA_OFF_ENV_SAVED_HEAD);
    za_emit_gc_link_slot(ZA_OFF_ENV_VAL, ZA_OFF_ENV_NODE);
    za_emit_gc_link_slot(ZA_OFF_ARGS_VAL, ZA_OFF_ARGS_NODE);

    if (!za_compile_expr(form, params, fixed_count, 0, &syms, env, 1, trampoline_offset, 0, 0, 0, 0)) {
        za_release_literal_slot_allocs();
        g_jit_used = entry;
        return nil;
    }

    // エピローグ(末尾呼び出しでトランポリンへjmpせずここへ流れ落ちた場合のみ通る経路)。
    // za_gc_unlinkの呼び出し自体がrcxを使うため、本体の結果(rax)は先にr13へ退避してから
    // 呼び、戻ってから復元する。
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);
    za_load_slot(ZA_REG_RCX, ZA_OFF_ENV_SAVED_HEAD);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_unlink);
    jit_call_r11();
    jit_mov_reg_reg(ZA_REG_RAX, ZA_REG_R13);
    jit_add_rsp_imm32(ZA_FRAME_TOTAL);
    jit_pop_r13();
    jit_pop_rbx();
    jit_ret();

    // setqとエスケープするlambdaが同一defun内に両方存在する場合、クロージャキャプチャの
    // コピー後setqが反映されない食い違いを避けるため無条件にfallbackさせる(粗い過大近似、
    // g_za_saw_setq_local/g_za_saw_escaping_lambdaのコメント参照)。同様に、flet/labels
    // 束縛関数を(function name)で取り出したケースも無条件にfallbackさせる
    // (g_za_saw_flet_labels_escapeのコメント参照)。
    if (g_jit_overflow || (g_za_saw_setq_local && g_za_saw_escaping_lambda) || g_za_saw_flet_labels_escape) {
        za_release_literal_slot_allocs();
        g_jit_used = entry;
        return nil;
    }

    // ここまでコンパイル成功。g_jit_code[entry..g_jit_used)をImmobilized Spaceの
    // 環境所有ページへコピーし、自己参照movabs(g_jit_reloc_patch_offsets)を新しい
    // 配置先アドレスに基づきパッチする。コピー元(g_jit_code)は関数間で共有される
    // ステージング用スクラッチバッファのため、この関数の呼び出しごとにg_jit_usedを
    // entryへロールバックして再利用する(呼び出し元が確保する512KBの予算は
    // コンパイル済み関数の総量ではなく「1関数分の最悪サイズ」だけで済む)。
    UINT64 code_len = g_jit_used - entry;
    UINT64 page_count = (code_len + IMM_PAGE_SIZE - 1) / IMM_PAGE_SIZE;
    void *dest = os_imm_pages_alloc_contiguous(page_count);
    if (dest == 0) {
        za_release_literal_slot_allocs();
        g_jit_used = entry;
        return nil;
    }

    UINT8 *dest_bytes = (UINT8 *)dest;
    for (UINT64 i = 0; i < code_len; i++) {
        dest_bytes[i] = g_jit_code[entry + i];
    }

    for (UINT32 i = 0; i < g_jit_reloc_count; i++) {
        UINT64 patch_offset = g_jit_reloc_patch_offsets[i];
        UINT64 old_imm = 0;
        for (UINT64 b = 0; b < 8; b++) {
            old_imm |= ((UINT64)g_jit_code[patch_offset + b]) << (b * 8);
        }
        UINT64 target_off = old_imm - (UINT64)(void *)g_jit_code;
        UINT64 new_imm = (UINT64)(void *)dest_bytes + (target_off - entry);
        UINT64 dest_patch_offset = patch_offset - entry;
        for (UINT64 b = 0; b < 8; b++) {
            dest_bytes[dest_patch_offset + b] = (UINT8)(new_imm >> (b * 8));
        }
    }

    // 末尾呼び出しの共有トランポリンへのjmp(jit_emit_jmp_to、g_jit_trampoline_jmp_patch_
    // offsets)は、上のmovabs自己参照とは異なりrel32(その場のIP相対距離)でエンコードされて
    // いる。トランポリン自身はg_jit_code内に固定(コピーされない)なので、jmp命令だけが
    // dest_bytesへ移動した後は、g_jit_code内の距離としてbakeされていた古いrel32値は
    // 無意味になる。dest_bytes側の新しい位置から見たトランポリンへの絶対距離で再計算する。
    UINT64 trampoline_abs = (UINT64)(void *)(g_jit_code + trampoline_offset);
    for (UINT32 i = 0; i < g_jit_trampoline_jmp_count; i++) {
        UINT64 patch_offset = g_jit_trampoline_jmp_patch_offsets[i];
        UINT64 dest_patch_offset = patch_offset - entry;
        UINT64 next_instr_addr = (UINT64)(void *)(dest_bytes + dest_patch_offset + 4);
        INT64 rel = (INT64)trampoline_abs - (INT64)next_instr_addr;
        UINT32 rel32 = (UINT32)rel;
        dest_bytes[dest_patch_offset] = (UINT8)(rel32);
        dest_bytes[dest_patch_offset + 1] = (UINT8)(rel32 >> 8);
        dest_bytes[dest_patch_offset + 2] = (UINT8)(rel32 >> 16);
        dest_bytes[dest_patch_offset + 3] = (UINT8)(rel32 >> 24);
    }

    jit_serialize_icache();

    os_environment_register_pages(env, dest, page_count);

    // Phase3.6: このコンパイル試行で確保したリテラルスロットをenvの所有物として登録する
    // (pagesスロットと同じタイミング)。環境破棄時にos_environment_reclaim_literal_slotsが
    // za_free_literal_slotを呼んでフリーリストへ返却する。
    for (UINT32 i = 0; i < g_za_literal_slot_alloc_count; i++) {
        os_environment_register_literal_slot(env, g_za_literal_slot_allocs[i]);
    }
    g_za_literal_slot_alloc_count = 0;

    g_jit_used = entry;

    return os_make_jit_function((lisp_addr_t)(void *)dest_bytes);
}

/**
 * 組み込み関数%%DESTROY-ENVIRONMENT-RECLAIM(documents/environment.md Phase4)。
 * 対象環境が所有するImmobilized Page(Phase3)とリテラルスロット(Phase3.6)を回収する。
 * runtime.cはza.cのプール構造を知らないため、za_free_literal_slotをコールバックとして
 * 渡す一方向依存(za.c→runtime.c)を保つ。呼び出し元のdestroy-environment(Lisp、
 * init.lisp)が対象環境が現在の環境自身/祖先でないことを確認済みであることを前提とする。
 * @param args 評価済みの引数リスト(第一引数: 破棄対象の環境)
 * @param env 呼び出し時の環境(未使用)
 * @return g_sym_t
 */
static lisp_val_t primitive_destroy_environment_reclaim(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t target_env = cc_car(args);
    os_environment_reclaim_pages(target_env);
    os_environment_reclaim_literal_slots(target_env, za_free_literal_slot);
    return g_sym_t;
}

/** za.c実装のネイティブ関数をglobal_environmentへ登録する。kernel_mainのブート列で
 * os_bootstrap()以降に呼ぶ(za.c→runtime.cの一方向依存を保つため、os_bootstrap()
 * 自体には登録しない)。 */
void os_register_za_primitives(void) {
    os_set_function(os_make_symbol("%%DESTROY-ENVIRONMENT-RECLAIM"), os_make_native_function((lisp_addr_t)(void *)primitive_destroy_environment_reclaim), global_environment);
}

#else /* !defined(__x86_64__) */

/**
 * 非x86_64ビルドではza_try_compile_defunが常にnilを返しJITコンパイルが発生しないため、
 * リテラルスロットプール自体が存在しない。Immobilized Pageの回収のみ行う。
 */
static lisp_val_t primitive_destroy_environment_reclaim(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t target_env = cc_car(args);
    os_environment_reclaim_pages(target_env);
    return g_sym_t;
}

void os_register_za_primitives(void) {
    os_set_function(os_make_symbol("%%DESTROY-ENVIRONMENT-RECLAIM"), os_make_native_function((lisp_addr_t)(void *)primitive_destroy_environment_reclaim), global_environment);
}

lisp_val_t za_try_compile_defun(lisp_val_t params, lisp_val_t body, lisp_val_t env) {
    (void)params;
    (void)body;
    (void)env;
    return nil;
}

#endif /* defined(__x86_64__) */
