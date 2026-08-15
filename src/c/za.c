#include "types.h"
#include "lisp.h"
#include "runtime.h"
#include "eval.h"
#include "za.h"

#if defined(__x86_64__)

#define JIT_CODE_SIZE 65536
/** サポートする仮引数(&restより手前の固定引数)の最大個数(コード量とコンパイル時間を有限に保つための上限) */
#define ZA_MAX_PARAMS 16
/** サポートする+のオペランドの最大個数(同上) */
#define ZA_MAX_OPERANDS 16

static UINT8 g_jit_code[JIT_CODE_SIZE] __attribute__((aligned(16)));
static UINT64 g_jit_used = 0;
/** jit_emit8がg_jit_codeの残り容量を使い切ったことを示すフラグ。立った場合は
 * za_try_compile_defunがg_jit_usedをコンパイル開始前の位置まで巻き戻してnilを返す */
static int g_jit_overflow = 0;

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

/**
 * 自己書き換えコード実行前の命令キャッシュ同期。cpuidはシリアライズ命令であり、
 * 直前に書き込んだ機械語がその後のフェッチで確実に見えるようにする。
 */
static void jit_serialize_icache(void) {
    UINT32 a = 0;
    __asm__ volatile("cpuid" : "=a"(a) : "a"(a) : "rbx", "rcx", "rdx", "memory");
    (void)a;
}

typedef struct {
    int is_literal;
    lisp_val_t literal;
    UINT64 param_index;
} za_operand_t;

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

/**
 * +のオペランド1個を分類する。paramsの固定引数部分に含まれるシンボル参照か、
 * 即値fixnumリテラルのみ許可する。
 * @return 分類できれば1(outに書く)、できなければ0
 */
static int za_classify_operand(lisp_val_t form, lisp_val_t params, UINT64 fixed_count, za_operand_t *out) {
    if ((form & TAG_MASK) == TAG_FIXNUM) {
        out->is_literal = 1;
        out->literal = form;
        return 1;
    }
    if ((form & TAG_MASK) == TAG_SYMBOL) {
        UINT64 idx;
        if (!za_param_index(params, form, fixed_count, &idx)) {
            return 0;
        }
        out->is_literal = 0;
        out->param_index = idx;
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

/**
 * za_try_compile_defunが確保する追加フレームのレイアウト(すべてrsp相対オフセット)。
 * env用は関数本体の実行中ずっとリンクしたまま(プロローグでリンク、エピローグ/
 * 末尾呼び出し脱出直前でアンリンク)。fn/acc/引数用は呼び出しサイトごとに一時的に
 * リンク/アンリンクする(このプロジェクトのグラマー制約により呼び出しは常に1つずつ
 * しか実行中にならないため、呼び出しサイトが複数あっても同じスロットを使い回せる)。
 */
#define ZA_OFF_ENV_SAVED_HEAD  0    /* env/args link前のgc_roots(関数終了時に復元する) */
#define ZA_OFF_CALL_SAVED_HEAD 8    /* 呼び出しサイトごとのfn/acc/引数link前のgc_roots */
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
/* 168-175: 16バイト境界を保つためのpadding */
#define ZA_OFF_ARG_BASE        176
#define ZA_ARG_SLOT_SIZE       24   /* 値8バイト+gc_rootnode16バイト */
/* ZA_OFF_ARG_BASE + ZA_MAX_OPERANDS*ZA_ARG_SLOT_SIZE(=560) は既に16バイト境界 */
#define ZA_FRAME_EXTRA         560
/* 既存のシャドウスペース(0x28=40)に追加分を足した、プロローグでsub rspする総量 */
#define ZA_FRAME_TOTAL         (0x28 + ZA_FRAME_EXTRA)

static UINT32 za_arg_val_off(UINT64 i) { return ZA_OFF_ARG_BASE + (UINT32)i * ZA_ARG_SLOT_SIZE; }
static UINT32 za_arg_node_off(UINT64 i) { return za_arg_val_off(i) + 8; }

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

/**
 * オペランド1個の値をraxへ計算する機械語を出力する。paramsへの参照であれば
 * ARGS_VALスロット(プロローグでリンク済みの元のevaluated_args先頭)から毎回読み直し、
 * cc_cdrをparam_index回・cc_carを1回呼ぶ。呼び出しコード生成(za_compile_call)が
 * os_make_cons等でヒープ確保する間にこの先頭が移動する可能性があるため、レジスタに
 * 生ポインタとしてキャッシュせず、リンク済みスロットから都度読み直す。
 */
static void za_emit_operand(const za_operand_t *op) {
    if (op->is_literal) {
        jit_movabs_rax(op->literal);
        return;
    }
    za_load_slot(ZA_REG_RCX, ZA_OFF_ARGS_VAL);
    for (UINT64 i = 0; i < op->param_index; i++) {
        jit_movabs_r11((UINT64)(void *)cc_cdr);
        jit_call_r11();
        jit_mov_rcx_rax();
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
 * r8=fn(タグ付きINSTANCE)。呼び出し元はここへjmpする前に自分のフレームを完全に
 * 畳んでおくこと(呼び出し元のレジスタ/フレームはここでは一切保存しない)。
 * このスタブ自体は毎回のza_try_compile_defun呼び出しの冒頭、コンパイル対象の
 * 関数用にg_jit_usedを記録する(=ロールバック時に巻き戻る)より前に確定させる
 * ことで、コンパイル失敗によるロールバックでスタブ自体が失われないようにする。
 */
static UINT64 za_ensure_trampoline(void) {
    if (g_za_trampoline_ready) {
        return g_za_trampoline_offset;
    }
    UINT64 offset = g_jit_used;

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
 * 「(op operand operand...)」(オペランド2個以上、ZA_MAX_OPERANDS以下)を検証しつつ、
 * pairwise foldでraxへ計算結果を残す機械語を出力する。各オペランドはleaf限定
 * (fixnumリテラルまたはparams固定引数への参照)のまま。呼び出す2引数ラッパー
 * (primitive_add2/primitive_subtract2/primitive_multiply2)はwrapper_fnで指定する。
 * @return 対応できれば1、できなければ0(この場合何バイト書き込んだかは呼び出し元が
 * ロールバックするので気にしなくてよい)
 */
static int za_compile_fold(lisp_val_t form, lisp_val_t params, UINT64 fixed_count, void *wrapper_fn) {
    za_operand_t ops[ZA_MAX_OPERANDS];
    UINT64 count = 0;
    for (lisp_val_t rest = cc_cdr(form); rest != nil; rest = cc_cdr(rest)) {
        if ((rest & TAG_MASK) != TAG_CONS) {
            return 0;
        }
        if (count >= ZA_MAX_OPERANDS) {
            return 0;
        }
        if (!za_classify_operand(cc_car(rest), params, fixed_count, &ops[count])) {
            return 0;
        }
        count++;
    }
    if (count < 2) {
        return 0;
    }

    za_emit_operand(&ops[0]);
    jit_mov_r13_rax();
    for (UINT64 i = 1; i < count; i++) {
        za_emit_operand(&ops[i]);
        jit_mov_rdx_rax();
        jit_mov_rcx_r13();
        jit_movabs_r11((UINT64)wrapper_fn);
        jit_call_r11();
        if (i != count - 1) {
            jit_mov_r13_rax();
        }
    }
    return 1;
}

/**
 * 「(- operand)」(単項マイナス、0-operandとして符号を反転)または
 * 「(- operand operand...)」(オペランド2個以上、za_compile_foldと同じ左からのfold)
 * を検証しemitする。オペランドはleaf限定。単項マイナスはコンパイル時に先頭へ即値
 * fixnum 0を挿した一時formを組み立ててza_compile_foldへ渡すことで、既存のfold処理を
 * そのまま再利用する(os_make_cons自体はコンパイル時に1度呼ぶだけで、実行時アロケー
 * ションではない)。
 * @return 対応できれば1、できなければ0
 */
static int za_compile_minus(lisp_val_t form, lisp_val_t params, UINT64 fixed_count) {
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    if (cc_cdr(rest) == nil) {
        lisp_val_t zero_form = os_make_cons(cc_car(form),
                                    os_make_cons(os_make_fixnum(0), os_make_cons(cc_car(rest), nil)));
        return za_compile_fold(zero_form, params, fixed_count, (void *)primitive_subtract2);
    }
    return za_compile_fold(form, params, fixed_count, (void *)primitive_subtract2);
}

/**
 * 「(op leaf)」(ちょうど1オペランド)を検証しつつ、1引数ラッパー(cc_car/cc_cdr/
 * primitive_null1/primitive_atom1)を呼んでraxへ結果を残す機械語を出力する。
 * オペランドはleaf限定。
 * @return 対応できれば1、できなければ0
 */
static int za_compile_unary(lisp_val_t form, lisp_val_t params, UINT64 fixed_count, void *wrapper_fn) {
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS || cc_cdr(rest) != nil) {
        return 0;
    }
    za_operand_t op0;
    if (!za_classify_operand(cc_car(rest), params, fixed_count, &op0)) {
        return 0;
    }
    za_emit_operand(&op0);
    jit_mov_rcx_rax();
    jit_movabs_r11((UINT64)wrapper_fn);
    jit_call_r11();
    return 1;
}

/**
 * 「(op operand operand)」(ちょうど2オペランド)を検証しつつ、2引数関数(wrapper_fn、
 * rcx=第一オペランド, rdx=第二オペランド)を呼んでraxへ結果を残す機械語を出力する。
 * オペランドはleaf限定。比較(primitive_less_than2/primitive_num_equal2/
 * primitive_eq2)に限らず、2引数を取る任意の関数(os_make_cons等)に使える汎用の形。
 * 3個以上の連鎖・1個以下は今回は非対応(フォールバックする)。
 * @return 対応できれば1、できなければ0
 */
static int za_compile_binary(lisp_val_t form, lisp_val_t params, UINT64 fixed_count, void *wrapper_fn) {
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    za_operand_t op0;
    if (!za_classify_operand(cc_car(rest), params, fixed_count, &op0)) {
        return 0;
    }
    lisp_val_t rest2 = cc_cdr(rest);
    if (rest2 == nil || (rest2 & TAG_MASK) != TAG_CONS || cc_cdr(rest2) != nil) {
        return 0;
    }
    za_operand_t op1;
    if (!za_classify_operand(cc_car(rest2), params, fixed_count, &op1)) {
        return 0;
    }

    za_emit_operand(&op0);
    jit_mov_r13_rax();
    za_emit_operand(&op1);
    jit_mov_rdx_rax();
    jit_mov_rcx_r13();
    jit_movabs_r11((UINT64)wrapper_fn);
    jit_call_r11();
    return 1;
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
 * eval.cのos_eval特殊形式ディスパッチ表と同じ集合(quote・if・+・-・*・<・=はza_compile_expr側で
 * 先に個別に処理済みなのでここには含めない)。
 */
static int za_is_excluded_special_form(lisp_val_t head) {
    return head == g_sym_quote || head == g_sym_progn || head == g_sym_setq ||
           head == g_sym_defun || head == g_sym_lambda || head == g_sym_defmacro ||
           head == g_sym_quasiquote || head == g_sym_block || head == g_sym_return_from ||
           head == g_sym_unwind_protect || head == g_sym_function || head == g_sym_flet ||
           head == g_sym_labels || head == g_sym_defvar || head == g_sym_defconstant ||
           head == g_sym_defdynamic || head == g_sym_defglobal || head == g_sym_dynamic ||
           head == g_sym_catch || head == g_sym_throw || head == g_sym_tagbody || head == g_sym_go;
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

static int za_compile_call(lisp_val_t form, lisp_val_t fn_sym, lisp_val_t params, UINT64 fixed_count,
                            const za_syms_t *syms, lisp_val_t env, int is_tail, UINT64 trampoline_offset);

/**
 * 「(lambda (lambda_params...) . lambda_body)」から、外側のJIT関数と同じ表現
 * (MAGIC_FUNCTION_INTERPRETED、eval.cのmake_interpreted_function/apply_functionが
 * 素で扱える形)のクロージャを組み立てる機械語を出力する。lambda本体自体はzaが
 * コンパイルせず、呼び出し時は常にインタプリタ経路(apply_function)が実行する。
 * 単一レベルのネストのみ対応: 自由変数解析はせず、外側関数の固定引数
 * (params[0..fixed_count-1]、&restを除く)を呼ばれるたびに全て新規environmentへ
 * os_set_variableでコピーし、それをクロージャのenvとする(setqはza対象外なので
 * コピー後の再代入で共有が破れる心配は無い)。
 * @return 対応できれば1、できなければ0(lambdaスロット枯渇時も含む)
 */
static int za_compile_lambda(lisp_val_t form, lisp_val_t params, UINT64 fixed_count) {
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

    if (g_za_lambda_slot_count >= ZA_MAX_LAMBDA_SLOTS) {
        return 0;
    }
    UINT64 slot_idx = g_za_lambda_slot_count++;
    g_za_lambda_slots[slot_idx] = os_make_cons(lambda_params, lambda_body);
    os_gc_register_root(&g_za_lambda_slots[slot_idx]);
    lisp_val_t *slot_addr = &g_za_lambda_slots[slot_idx];

    // 1. lambda専用スコープ開始前のgc_rootsを保存する。
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_current_head);
    jit_call_r11();
    za_store_slot(ZA_REG_RAX, ZA_OFF_LAMBDA_SAVED_HEAD);

    // 2. 新規env = os_make_environment("LAMBDA-ENV", 現在のenv)を構築し、linkする。
    UINT64 env_name_off = za_emit_symbol_name(os_make_symbol("LAMBDA-ENV"));
    jit_movabs_reg(ZA_REG_RCX, (UINT64)(void *)(g_jit_code + env_name_off));
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_symbol);
    jit_call_r11();
    jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_RAX);
    za_load_slot(ZA_REG_RDX, ZA_OFF_ENV_VAL);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_environment);
    jit_call_r11();
    za_store_slot(ZA_REG_RAX, ZA_OFF_LAMBDA_ENV_VAL);
    za_emit_gc_link_slot(ZA_OFF_LAMBDA_ENV_VAL, ZA_OFF_LAMBDA_ENV_NODE);

    // 3. TMPスロットを一度だけlinkし、外側の固定引数を1つずつos_set_variableで
    // 新規envへコピーする。
    jit_movabs_rax(nil);
    za_store_slot(ZA_REG_RAX, ZA_OFF_LAMBDA_TMP_VAL);
    za_emit_gc_link_slot(ZA_OFF_LAMBDA_TMP_VAL, ZA_OFF_LAMBDA_TMP_NODE);

    lisp_val_t p = params;
    for (UINT64 i = 0; i < fixed_count; i++) {
        lisp_val_t param_sym = cc_car(p);
        p = cc_cdr(p);

        za_operand_t op;
        op.is_literal = 0;
        op.param_index = i;
        za_emit_operand(&op); /* rax = 外側param[i]の現在値 */
        za_store_slot(ZA_REG_RAX, ZA_OFF_LAMBDA_TMP_VAL);

        UINT64 psym_off = za_emit_symbol_name(param_sym);
        jit_movabs_reg(ZA_REG_RCX, (UINT64)(void *)(g_jit_code + psym_off));
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_symbol);
        jit_call_r11();
        jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_RAX);
        za_load_slot(ZA_REG_RDX, ZA_OFF_LAMBDA_TMP_VAL);
        za_load_slot(ZA_REG_R8, ZA_OFF_LAMBDA_ENV_VAL);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_set_variable);
        jit_call_r11();
    }

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
 * formを評価してraxに結果を残す機械語を出力する。対応するグラマーは
 * 「fixnumリテラル / params固定引数への参照 / (+ leaf leaf...) / (- leaf) /
 * (- leaf leaf...) / (* leaf leaf...) / (< leaf leaf) / (= leaf leaf) /
 * (if test then else?) / (fn-sym arg arg...)」(if・+/-/*のtest/then/else/operand
 * それぞれの位置には再帰的に上記のいずれかを許容する。ただし+/-/*のoperand、および
 * 一般呼び出しの引数(allow_call=0)はleaf/算術/比較/if限定のまま、それらの中に
 * さらに一般呼び出しを直接書くことはできない。</、=はちょうど2オペランドのみ対応)。
 * formおよびifのtest/then/elseは、上記のいずれにも分類する前にza_macroexpandで
 * fixpointまで展開する(andのようにif木へ完全展開されるマクロはこれで透過的に
 * コンパイル対象になる。+/-/*のoperand位置・呼び出しの引数位置はleaf/算術/比較/if
 * 限定のままなので展開を挟まない)。
 * @param allow_call この位置で一般呼び出しを許容するか(body直下・ifのthen/elseのみ1、
 * ifのtestと呼び出しの引数位置は0)
 * @param is_tail この位置が末尾位置かどうか(body直下・ifのthen/else<ifが末尾の場合>で
 * 呼び出し側から継承。ifのtestと呼び出しの引数位置は常に0)
 * @param trampoline_offset 末尾呼び出しが共有トランポリンへjmpする際の着地先オフセット
 * @return 対応できれば1、できなければ0(何バイト書き込んだかは呼び出し元がロールバックする)
 */
static int za_compile_expr(lisp_val_t form, lisp_val_t params, UINT64 fixed_count, const za_syms_t *syms,
                            lisp_val_t env, int allow_call, int is_tail, UINT64 trampoline_offset) {
    form = za_macroexpand(form, env);

    za_operand_t leaf;
    if (za_classify_operand(form, params, fixed_count, &leaf)) {
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
    if (head == syms->plus) {
        return za_compile_fold(form, params, fixed_count, (void *)primitive_add2);
    }
    if (head == syms->minus) {
        return za_compile_minus(form, params, fixed_count);
    }
    if (head == syms->star) {
        return za_compile_fold(form, params, fixed_count, (void *)primitive_multiply2);
    }
    if (head == syms->lt) {
        return za_compile_binary(form, params, fixed_count, (void *)primitive_less_than2);
    }
    if (head == syms->eq) {
        return za_compile_binary(form, params, fixed_count, (void *)primitive_num_equal2);
    }
    if (head == g_sym_car) {
        return za_compile_unary(form, params, fixed_count, (void *)cc_car);
    }
    if (head == g_sym_cdr) {
        return za_compile_unary(form, params, fixed_count, (void *)cc_cdr);
    }
    if (head == syms->nullsym) {
        return za_compile_unary(form, params, fixed_count, (void *)primitive_null1);
    }
    if (head == syms->atom) {
        return za_compile_unary(form, params, fixed_count, (void *)primitive_atom1);
    }
    if (head == syms->eqp) {
        return za_compile_binary(form, params, fixed_count, (void *)primitive_eq2);
    }
    if (head == g_sym_cons) {
        return za_compile_binary(form, params, fixed_count, (void *)os_make_cons);
    }
    if (head == g_sym_lambda) {
        return za_compile_lambda(form, params, fixed_count);
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

        if (!za_compile_expr(test_form, params, fixed_count, syms, env, 0, 0, trampoline_offset)) {
            return 0;
        }
        jit_movabs_r11(nil);
        jit_cmp_rax_r11();
        UINT64 je_patch = jit_emit_je_rel32_placeholder();

        if (!za_compile_expr(then_form, params, fixed_count, syms, env, allow_call, is_tail, trampoline_offset)) {
            return 0;
        }
        // then分岐の実行後は必ずelse/nilフォールバック側を飛び越える(elseが無い場合も
        // このjmpが無いと直後のjit_movabs_rax(nil)にそのまま流れ落ち、thenの結果が
        // 無条件にnilで上書きされてしまう)
        UINT64 jmp_patch = jit_emit_jmp_rel32_placeholder();

        jit_patch_rel32(je_patch);
        if (has_else) {
            if (!za_compile_expr(else_form, params, fixed_count, syms, env, allow_call, is_tail, trampoline_offset)) {
                return 0;
            }
        } else {
            jit_movabs_rax(nil);
        }
        jit_patch_rel32(jmp_patch);
        return 1;
    }

    // 一般呼び出し: headがシンボルで、除外リストの特殊形式でなければ関数呼び出しとして
    // 扱う。引数位置・ifのtest位置(allow_call=0)からここに来た場合はネスト呼び出しに
    // なるのでコンパイルを断念する(+/-/*のoperandがleaf限定なのと同じ形の制約)。
    if (!allow_call) {
        return 0;
    }
    if ((head & TAG_MASK) != TAG_SYMBOL || za_is_excluded_special_form(head)) {
        return 0;
    }
    return za_compile_call(form, head, params, fixed_count, syms, env, is_tail, trampoline_offset);
}

/**
 * 一般呼び出し「(fn_sym arg arg...)」をコンパイルする(呼び出しごとに以下の順で
 * 機械語を出力する)。
 *   1. 呼び出し前のgc_rootsをCALL_SAVED_HEADスロットへ保存する。
 *   2. 引数を左から順にza_compile_expr(allow_call=0, is_tail=0)で評価し、対応する
 *      引数スロットへ書き込み、その都度linkする。
 *   3. os_make_symbolでfn_symの名前から現在のタグ付きシンボルポインタを再解決し、
 *      os_get_function(sym, env)をfnスロットへ書き込み、linkする(envはENV_VALスロット
 *      から読み直す)。
 *   4. accスロットをnilで初期化しlinkした後、引数スロットを右から左へ
 *      os_make_cons(argslot[i], accslot)で辿ってconsし、その都度accスロットを書き換える。
 *   5. CALL_SAVED_HEADでfn/acc/引数のリンクをまとめて外す。
 *   6. 非末尾ならos_apply_function(fn, evaluated_args, env)を通常のcallで呼ぶ。末尾なら
 *      envのリンクも外し、自分のフレームを完全に畳んだ上で共有トランポリンへjmpする。
 * @return 対応できれば1、できなければ0(何バイト書き込んだかは呼び出し元がロールバックする)
 */
static int za_compile_call(lisp_val_t form, lisp_val_t fn_sym, lisp_val_t params, UINT64 fixed_count,
                            const za_syms_t *syms, lisp_val_t env, int is_tail, UINT64 trampoline_offset) {
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
    za_store_slot(ZA_REG_RAX, ZA_OFF_CALL_SAVED_HEAD);

    // 2. 引数を左から順に評価し、引数スロットへ書き込み、linkする。
    for (UINT64 i = 0; i < argc; i++) {
        if (!za_compile_expr(arg_forms[i], params, fixed_count, syms, env, 0, 0, trampoline_offset)) {
            return 0;
        }
        za_store_slot(ZA_REG_RAX, za_arg_val_off(i));
        za_emit_gc_link_slot(za_arg_val_off(i), za_arg_node_off(i));
    }

    // 3. os_get_function(fn_sym, env)をfnスロットへ書き込み、linkする。
    UINT64 name_off = za_emit_symbol_name(fn_sym);
    jit_movabs_reg(ZA_REG_RCX, (UINT64)(void *)(g_jit_code + name_off));
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_symbol);
    jit_call_r11();
    jit_mov_reg_reg(ZA_REG_R13, ZA_REG_RAX);

    jit_mov_reg_reg(ZA_REG_RCX, ZA_REG_R13);
    za_load_slot(ZA_REG_RDX, ZA_OFF_ENV_VAL);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_get_function);
    jit_call_r11();
    za_store_slot(ZA_REG_RAX, ZA_OFF_FN_VAL);
    za_emit_gc_link_slot(ZA_OFF_FN_VAL, ZA_OFF_FN_NODE);

    // 4. accスロットをnilで初期化しlinkした後、右から左へos_make_consでfoldする。
    jit_movabs_rax(nil);
    za_store_slot(ZA_REG_RAX, ZA_OFF_ACC_VAL);
    za_emit_gc_link_slot(ZA_OFF_ACC_VAL, ZA_OFF_ACC_NODE);
    for (UINT64 i = argc; i > 0; i--) {
        za_load_slot(ZA_REG_RCX, za_arg_val_off(i - 1));
        za_load_slot(ZA_REG_RDX, ZA_OFF_ACC_VAL);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_make_cons);
        jit_call_r11();
        za_store_slot(ZA_REG_RAX, ZA_OFF_ACC_VAL);
    }

    // 5. fn/acc/引数のリンクをまとめて外す。
    za_load_slot(ZA_REG_RCX, ZA_OFF_CALL_SAVED_HEAD);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_unlink);
    jit_call_r11();

    if (!is_tail) {
        // 6a. 非末尾: os_apply_function(fn, evaluated_args, env)を通常のcallで呼ぶ。
        za_load_slot(ZA_REG_RCX, ZA_OFF_FN_VAL);
        za_load_slot(ZA_REG_RDX, ZA_OFF_ACC_VAL);
        za_load_slot(ZA_REG_R8, ZA_OFF_ENV_VAL);
        jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)os_apply_function);
        jit_call_r11();
        return 1;
    }

    // 6b. 末尾: envのリンクも外す。
    za_load_slot(ZA_REG_RCX, ZA_OFF_ENV_SAVED_HEAD);
    jit_movabs_reg(ZA_REG_R11, (UINT64)(void *)za_gc_unlink);
    jit_call_r11();

    // トランポリンの呼び出し規約(rcx=evaluated_args, rdx=env, r8=fn)に沿って、
    // フレームを解体する前に値スロットからレジスタへ読み出しておく。
    za_load_slot(ZA_REG_RCX, ZA_OFF_ACC_VAL);
    za_load_slot(ZA_REG_RDX, ZA_OFF_ENV_VAL);
    za_load_slot(ZA_REG_R8, ZA_OFF_FN_VAL);
    jit_add_rsp_imm32(ZA_FRAME_TOTAL);
    jit_pop_r13();
    jit_pop_rbx();
    jit_emit_jmp_to(trampoline_offset);
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
    syms.eqp = os_make_symbol("EQ");
    syms.nullsym = os_make_symbol("NULL");
    syms.atom = os_make_symbol("ATOM");

    g_jit_overflow = 0;
    // トランポリンは全JIT関数で共有するため、今回のコンパイル対象用にentryを記録する
    // より前に確定させる(コンパイル失敗時のg_jit_used巻き戻しでスタブ自体が失われない
    // ようにするため)。
    UINT64 trampoline_offset = za_ensure_trampoline();
    if (g_jit_overflow) {
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

    if (!za_compile_expr(form, params, fixed_count, &syms, env, 1, 1, trampoline_offset)) {
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

    if (g_jit_overflow) {
        g_jit_used = entry;
        return nil;
    }

    jit_serialize_icache();

    return os_make_jit_function((lisp_addr_t)(void *)(g_jit_code + entry));
}

#else /* !defined(__x86_64__) */

lisp_val_t za_try_compile_defun(lisp_val_t params, lisp_val_t body, lisp_val_t env) {
    (void)params;
    (void)body;
    (void)env;
    return nil;
}

#endif /* defined(__x86_64__) */
