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

/**
 * patch_offsetにある4バイトのrel32フィールドへ、現在のg_jit_used(=着地点)までの
 * 相対距離を書き込む。オーバーフロー済み、またはpatch_offsetがバッファ範囲外の場合は
 * 何もしない(呼び出し元がg_jit_overflow経由で失敗を検出しロールバックする)。
 */
static void jit_patch_rel32(UINT64 patch_offset) {
    if (g_jit_overflow || patch_offset + 4 > JIT_CODE_SIZE) {
        return;
    }
    INT64 rel = (INT64)g_jit_used - (INT64)(patch_offset + 4);
    UINT32 rel32 = (UINT32)rel;
    g_jit_code[patch_offset] = (UINT8)(rel32);
    g_jit_code[patch_offset + 1] = (UINT8)(rel32 >> 8);
    g_jit_code[patch_offset + 2] = (UINT8)(rel32 >> 16);
    g_jit_code[patch_offset + 3] = (UINT8)(rel32 >> 24);
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

/** za_compile_expr/za_try_compile_defunが + - * < = を認識するためのシンボル束。
 * za_try_compile_defunで1度だけos_make_symbolして組み立てる。 */
typedef struct {
    lisp_val_t plus;
    lisp_val_t minus;
    lisp_val_t star;
    lisp_val_t lt;
    lisp_val_t eq;
} za_syms_t;

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
 * 「(op operand operand)」(ちょうど2オペランド)を検証しつつ、2引数ラッパー
 * (primitive_less_than2/primitive_num_equal2)を呼んでraxへ真値シンボル/nilを
 * 残す機械語を出力する。オペランドはleaf限定。3個以上の連鎖比較・1個以下は
 * 今回は非対応(フォールバックする)。
 * @return 対応できれば1、できなければ0
 */
static int za_compile_compare(lisp_val_t form, lisp_val_t params, UINT64 fixed_count, void *wrapper_fn) {
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
 * formを評価してraxに結果を残す機械語を出力する。対応するグラマーは
 * 「fixnumリテラル / params固定引数への参照 / (+ leaf leaf...) / (- leaf) /
 * (- leaf leaf...) / (* leaf leaf...) / (< leaf leaf) / (= leaf leaf) /
 * (if test then else?)」(if・+/-/*のtest/then/else/operandそれぞれの位置には
 * 再帰的に上記のいずれかを許容する。ただし+/-/*のoperandはleaf限定のまま、
 * それらの中にifを直接書くことはできない。</、=はちょうど2オペランドのみ対応)。
 * formおよびifのtest/then/elseは、上記のいずれにも分類する前にza_macroexpandで
 * fixpointまで展開する(andのようにif木へ完全展開されるマクロはこれで透過的に
 * コンパイル対象になる。+/-/*のoperand位置はleaf限定のままなので展開を挟まない)。
 * @return 対応できれば1、できなければ0(何バイト書き込んだかは呼び出し元がロールバックする)
 */
static int za_compile_expr(lisp_val_t form, lisp_val_t params, UINT64 fixed_count, const za_syms_t *syms, lisp_val_t env) {
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
        return za_compile_compare(form, params, fixed_count, (void *)primitive_less_than2);
    }
    if (head == syms->eq) {
        return za_compile_compare(form, params, fixed_count, (void *)primitive_num_equal2);
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

        if (!za_compile_expr(test_form, params, fixed_count, syms, env)) {
            return 0;
        }
        jit_movabs_r11(nil);
        jit_cmp_rax_r11();
        UINT64 je_patch = jit_emit_je_rel32_placeholder();

        if (!za_compile_expr(then_form, params, fixed_count, syms, env)) {
            return 0;
        }
        // then分岐の実行後は必ずelse/nilフォールバック側を飛び越える(elseが無い場合も
        // このjmpが無いと直後のjit_movabs_rax(nil)にそのまま流れ落ち、thenの結果が
        // 無条件にnilで上書きされてしまう)
        UINT64 jmp_patch = jit_emit_jmp_rel32_placeholder();

        jit_patch_rel32(je_patch);
        if (has_else) {
            if (!za_compile_expr(else_form, params, fixed_count, syms, env)) {
                return 0;
            }
        } else {
            jit_movabs_rax(nil);
        }
        jit_patch_rel32(jmp_patch);
        return 1;
    }
    return 0;
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

    UINT64 entry = g_jit_used;
    g_jit_overflow = 0;

    // プロローグ: rbx/r13を退避し、MS x64呼び出し規約に沿ってシャドウスペースを確保する
    jit_push_rbx();
    jit_push_r13();
    jit_sub_rsp_imm8(0x28);
    jit_mov_rbx_rcx();

    if (!za_compile_expr(form, params, fixed_count, &syms, env)) {
        g_jit_used = entry;
        return nil;
    }

    // エピローグ
    jit_add_rsp_imm8(0x28);
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
