#include "types.h"
#include "lisp.h"
#include "runtime.h"
#include "za.h"

#if defined(__x86_64__)

#define JIT_CODE_SIZE 65536
/** コンパイル1回あたりに書き込む最大バイト数の目安。あふれる場合はコンパイルを諦める */
#define ZA_MAX_EMIT_BYTES 700
/** サポートする仮引数の最大個数(コード量とコンパイル時間を有限に保つための上限) */
#define ZA_MAX_PARAMS 16

static UINT8 g_jit_code[JIT_CODE_SIZE] __attribute__((aligned(16)));
static UINT64 g_jit_used = 0;

static void jit_emit8(UINT8 b) {
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

static void jit_call_r11(void) { jit_emit8(0x41); jit_emit8(0xFF); jit_emit8(0xD3); }
static void jit_ret(void) { jit_emit8(0xC3); }

// mov rbx, rcx (evaluated_argsの先頭を呼び出し中保持するレジスタへ退避)
static void jit_mov_rbx_rcx(void) { jit_emit8(0x48); jit_emit8(0x89); jit_emit8(0xCB); }
// mov rcx, rbx (argsの先頭からcc_car/cc_cdrを辿り直す準備)
static void jit_mov_rcx_rbx(void) { jit_emit8(0x48); jit_emit8(0x89); jit_emit8(0xD9); }
// mov rcx, rax (呼び出し結果を次呼び出しのMS ABI第1引数へ)
static void jit_mov_rcx_rax(void) { jit_emit8(0x48); jit_emit8(0x89); jit_emit8(0xC1); }
// mov rdx, rax (第2オペランドの値をMS ABI第2引数へ)
static void jit_mov_rdx_rax(void) { jit_emit8(0x48); jit_emit8(0x89); jit_emit8(0xC2); }
// mov r13, rax (第1オペランドの値を第2オペランド計算中も保持するレジスタへ退避)
static void jit_mov_r13_rax(void) { jit_emit8(0x49); jit_emit8(0x89); jit_emit8(0xC5); }
// mov rcx, r13 (退避していた第1オペランドの値をMS ABI第1引数へ戻す)
static void jit_mov_rcx_r13(void) { jit_emit8(0x4C); jit_emit8(0x89); jit_emit8(0xE9); }

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
 * paramsの中からsymと同じシンボルの位置(0始まり)を探す。
 * @return 見つかれば1、見つからなければ0
 */
static int za_param_index(lisp_val_t params, lisp_val_t sym, UINT64 *out_index) {
    UINT64 idx = 0;
    for (lisp_val_t cur = params; cur != nil; cur = cc_cdr(cur), idx++) {
        if (cc_car(cur) == sym) {
            *out_index = idx;
            return 1;
        }
    }
    return 0;
}

/**
 * 仮引数リストが「重複のない、平坦なシンボルのみのリスト」であり、&restを含まず
 * ZA_MAX_PARAMS以下であることを検証する。
 * @return 検証できれば1、できなければ0
 */
static int za_validate_params(lisp_val_t params) {
    UINT64 count = 0;
    for (lisp_val_t cur = params; cur != nil; cur = cc_cdr(cur)) {
        if ((cur & TAG_MASK) != TAG_CONS) {
            return 0;
        }
        lisp_val_t sym = cc_car(cur);
        if ((sym & TAG_MASK) != TAG_SYMBOL || sym == g_sym_rest) {
            return 0;
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
    return 1;
}

/**
 * +のオペランド1個を分類する。paramsに含まれるシンボル参照か、即値fixnumリテラルのみ許可する。
 * @return 分類できれば1(outに書く)、できなければ0
 */
static int za_classify_operand(lisp_val_t form, lisp_val_t params, za_operand_t *out) {
    if ((form & TAG_MASK) == TAG_FIXNUM) {
        out->is_literal = 1;
        out->literal = form;
        return 1;
    }
    if ((form & TAG_MASK) == TAG_SYMBOL) {
        UINT64 idx;
        if (!za_param_index(params, form, &idx)) {
            return 0;
        }
        out->is_literal = 0;
        out->param_index = idx;
        return 1;
    }
    return 0;
}

/**
 * bodyが「(+ operand operand)」ぴったり1フォームであることを検証し、2個のオペランドを分類する。
 * @return 検証できれば1(op1/op2に書く)、できなければ0
 */
static int za_validate_body(lisp_val_t body, lisp_val_t params, lisp_val_t sym_plus, za_operand_t *op1, za_operand_t *op2) {
    if (body == nil || (body & TAG_MASK) != TAG_CONS || cc_cdr(body) != nil) {
        return 0;
    }
    lisp_val_t form = cc_car(body);
    if ((form & TAG_MASK) != TAG_CONS || cc_car(form) != sym_plus) {
        return 0;
    }
    lisp_val_t rest = cc_cdr(form);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    lisp_val_t op1_form = cc_car(rest);
    rest = cc_cdr(rest);
    if (rest == nil || (rest & TAG_MASK) != TAG_CONS) {
        return 0;
    }
    lisp_val_t op2_form = cc_car(rest);
    if (cc_cdr(rest) != nil) {
        return 0;
    }
    if (!za_classify_operand(op1_form, params, op1)) {
        return 0;
    }
    if (!za_classify_operand(op2_form, params, op2)) {
        return 0;
    }
    return 1;
}

/**
 * オペランド1個の値をraxへ計算する機械語を出力する。paramsへの参照であれば
 * rbxに退避済みのevaluated_args先頭からcc_cdrをparam_index回・cc_carを1回呼ぶ。
 * この間アロケーションは発生しないため、まだ辿っていないリストの残りが
 * GCで無効化される心配はない。
 */
static void za_emit_operand(const za_operand_t *op) {
    if (op->is_literal) {
        jit_movabs_rax(op->literal);
        return;
    }
    jit_mov_rcx_rbx();
    for (UINT64 i = 0; i < op->param_index; i++) {
        jit_movabs_r11((UINT64)(void *)cc_cdr);
        jit_call_r11();
        jit_mov_rcx_rax();
    }
    jit_movabs_r11((UINT64)(void *)cc_car);
    jit_call_r11();
}

lisp_val_t za_try_compile_defun(lisp_val_t params, lisp_val_t body) {
    if (!za_validate_params(params)) {
        return nil;
    }

    lisp_val_t sym_plus = os_make_symbol("+");
    za_operand_t op1, op2;
    if (!za_validate_body(body, params, sym_plus, &op1, &op2)) {
        return nil;
    }

    if (g_jit_used + ZA_MAX_EMIT_BYTES > JIT_CODE_SIZE) {
        return nil;
    }

    UINT64 entry = g_jit_used;

    // プロローグ: rbx/r13を退避し、MS x64呼び出し規約に沿ってシャドウスペースを確保する
    jit_push_rbx();
    jit_push_r13();
    jit_sub_rsp_imm8(0x28);
    jit_mov_rbx_rcx();

    za_emit_operand(&op1);
    jit_mov_r13_rax();
    za_emit_operand(&op2);
    jit_mov_rdx_rax();
    jit_mov_rcx_r13();
    jit_movabs_r11((UINT64)(void *)primitive_add2);
    jit_call_r11();

    // エピローグ
    jit_add_rsp_imm8(0x28);
    jit_pop_r13();
    jit_pop_rbx();
    jit_ret();

    jit_serialize_icache();

    return os_make_jit_function((lisp_addr_t)(void *)(g_jit_code + entry));
}

#else /* !defined(__x86_64__) */

lisp_val_t za_try_compile_defun(lisp_val_t params, lisp_val_t body) {
    (void)params;
    (void)body;
    return nil;
}

#endif /* defined(__x86_64__) */
