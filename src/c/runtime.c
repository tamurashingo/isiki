#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "lisp.h"

/*
 * ---- タグ付きポインタによるLispオブジェクトの表現 ----
 *
 * タグ位置: 64bit値の下位3bit (LSB側)
 *   obj & TAG_MASK       -> タグを取り出す
 *   obj & ~TAG_MASK      -> 実体アドレス(またはデコード前の値)を取り出す
 *
 * ヒープ確保は常に8byte境界になるため、確保したアドレスの下位3bitは常に0となり、
 * そこにタグを詰め込む。
 *
 * #define TAG_MASK     0x7ULL
 * #define TAG_FIXNUM   0x0ULL   // 000 即値
 * #define TAG_CONS     0x1ULL   // 001 アドレス
 * #define TAG_SYMBOL   0x2ULL   // 010 アドレス
 * #define TAG_CHAR     0x3ULL   // 011 即値
 * #define TAG_STRING   0x4ULL   // 100 アドレス
 * #define TAG_INSTANCE 0x5ULL   // 101 アドレス
 *                               // 110 予約
 *                               // 111 予約
 *
 * ---- タグ別メモリ配置 ----
 *
 * TAG_FIXNUM: 即値、ヒープなし(符号付き、60bitマグニチュード+1bit符号)
 *  [s][ magnitude(60bit) .............................. ][0 0 0]
 *    最上位bit(bit63)が符号(1:負、0は0を含む非負に正規化)、残り60bitがマグニチュード。
 *    表現範囲は-(2^60-1)〜2^60-1。マグニチュードが60bitを超える場合はMAGIC_BIGNUM
 *    (TAG_INSTANCE)に昇格する。
 *
 * TAG_CONS: アドレス、ヒープ16byte
 *  [ car-addr(61bit) ................................. ][0 0 1]
 *    carのアドレスが入っている
 *    consはLispオブジェクト格納用に連続した16byteを確保している
 *    - word0: car
 *    - word1: cdr
 *
 * TAG_SYMBOL: アドレス、ヒープ8Byte
 *  [ sym-addr(61bit) ................................. ][0 1 0]
 *    symbol用に確保した8byteへのアドレスが入っている
 *    - word0: 名前のstringへのポインタ(STRINGオブジェクト)
 *             symbol の値や関数は環境(environment)から取得する
 *
 * TAG_CHAR: 即値、ヒープなし
 *  [ val(61bit) ...................................... ][0 1 1]
 *    61bitの中に値を埋め込む
 *
 * TAG_STRING: アドレス、ヒープ可変長(8 + lenbyte, 8byte境界に整列)
 *  [ string-addr(61bit) .............................. ][1 0 0]
 *    string情報へのアドレスが入っている
 *    - word0:       文字列長さ(タグなしの整数)
 *    - byte[8....]: 文字のデータ本体
 *
 * TAG_INSTANCE: アドレス、ヒープ32byte
 *  [ inst-addr(61bit) ................................ ][1 0 1]
 *    instanceへのアドレス
 *    - word0: このinstanceの種別を表わすMAGIC NUMBER
 *    - word1: MAGIC_FUNCTION_NATIVEの場合: cの関数のアドレス
 *             MAGIC_FUNCTION_INTERPRETEDの場合: 仮引数リスト(未評価のシンボルリスト)
 *    - word2: MAGIC_FUNCTION_INTERPRETEDの場合: 本体(未評価のフォーム列)
 *    - word3: MAGIC_FUNCTION_INTERPRETEDの場合: 定義時の環境のアドレス
 *             MAGIC_VECTORの場合: word1に多次元配列(general array)本体への生ポインタを
 *             持つ(ヒープ可変長、8*(1+rank+total)byte、8byte境界に整列)。
 *             本体のレイアウトは以下の通り:
 *               - word0:          次元数(rank、タグなしの整数)
 *               - word[1..rank]:  各次元のサイズ(タグなしの整数)
 *               - word[rank+1..]: 要素本体(タグ付きのlisp_val_t、行優先(row-major)順)
 *
 */

extern frame_buffer* get_active_frame_buffer(void);


/** NIL */
lisp_val_t nil;
/** T */
lisp_val_t g_sym_t;

/** プロセスの状態を表すシンボル:READY(入力待ち) */
lisp_val_t g_sym_process_ready;
/** プロセスの状態を表すシンボル:RUNNING(実行中) */
lisp_val_t g_sym_process_running;
/** プロセスの状態を表すシンボル:DEAD(終了済み) */
lisp_val_t g_sym_process_dead;

/** 実行中のPCBを登録する自己参照する循環consリストを保持するシンボル(*RUN-QUEUE*) */
lisp_val_t g_sym_run_queue;
/** *RUN-QUEUE*上の現在実行中のプロセスを指すセルを保持するシンボル(*CURRENT-PROCESS*) */
lisp_val_t g_sym_current_process;

/** quote特殊形式を表すシンボル */
lisp_val_t g_sym_quote;
/** if特殊形式を表すシンボル */
lisp_val_t g_sym_if;
/** progn特殊形式を表すシンボル */
lisp_val_t g_sym_progn;
/** setq特殊形式を表すシンボル */
lisp_val_t g_sym_setq;
/** defun特殊形式を表すシンボル */
lisp_val_t g_sym_defun;
/** lambda特殊形式を表すシンボル */
lisp_val_t g_sym_lambda;
/** defmacro特殊形式を表すシンボル */
lisp_val_t g_sym_defmacro;
/** block特殊形式を表すシンボル */
lisp_val_t g_sym_block;
/** return-from特殊形式を表すシンボル */
lisp_val_t g_sym_return_from;
/** unwind-protect特殊形式を表すシンボル */
lisp_val_t g_sym_unwind_protect;
/** function特殊形式を表すシンボル */
lisp_val_t g_sym_function;
/** flet特殊形式を表すシンボル */
lisp_val_t g_sym_flet;
/** labels特殊形式を表すシンボル */
lisp_val_t g_sym_labels;
/** defvar特殊形式を表すシンボル */
lisp_val_t g_sym_defvar;
/** defconstant特殊形式を表すシンボル */
lisp_val_t g_sym_defconstant;
/** defdynamic特殊形式を表すシンボル */
lisp_val_t g_sym_defdynamic;
/** defglobal特殊形式を表すシンボル */
lisp_val_t g_sym_defglobal;
/** dynamic特殊形式を表すシンボル */
lisp_val_t g_sym_dynamic;
/** 仮引数リストで残りの実引数をリストとしてまとめて受け取ることを示すマーカーシンボル(&rest) */
lisp_val_t g_sym_rest;
/** quasiquote(`)を表すシンボル。reader.cが`x`を(QUASIQUOTE x)へ読むために使う */
lisp_val_t g_sym_quasiquote;
/** unquote(,)を表すシンボル。reader.cが,xを(UNQUOTE x)へ読むために使う */
lisp_val_t g_sym_unquote;
/** unquote-splicing(,@)を表すシンボル。reader.cが,@xを(UNQUOTE-SPLICING x)へ読むために使う */
lisp_val_t g_sym_unquote_splicing;
/** car関数を表すシンボル */
lisp_val_t g_sym_car;
/** cdr関数を表すシンボル */
lisp_val_t g_sym_cdr;
/** cons関数を表すシンボル */
lisp_val_t g_sym_cons;

/** 構文エラーを表すシンボル */
lisp_val_t g_sym_read_error;
/** 評価エラーを表すシンボル */
lisp_val_t g_sym_eval_error;

/** os_eval_top_levelが張るblockの名前を表すシンボル(%TOP-LEVEL) */
lisp_val_t g_sym_top_level_block;

/** catch特殊形式を表すシンボル */
lisp_val_t g_sym_catch;
/** throw特殊形式を表すシンボル */
lisp_val_t g_sym_throw;
/** tagbody特殊形式を表すシンボル */
lisp_val_t g_sym_tagbody;
/** go特殊形式を表すシンボル */
lisp_val_t g_sym_go;

/** ルートの環境(全プロセスの環境が最終的にこれを親として辿る) */
lisp_val_t global_environment;

/** defdynamicで定義された動的変数の値を保持するグローバルなフラットalist(sym . val)。レキシカルなenvの親子関係とは無関係 */
lisp_val_t g_dynamic_bindings;

/** internされたsymbolを保持できる最大数 */
#define MAX_SYMBOLS 1024
/** internされたsymbol一覧(os_make_symbolでの重複チェック用) */
static lisp_val_t g_symbol_table[MAX_SYMBOLS];
/** g_symbol_tableに登録済みのsymbol数 */
static int g_symbol_count = 0;


/** From空間(現在割り当てに使っている側)の先頭アドレス */
static UINT8 *g_from_start;
/** From空間の次の割り当て位置 */
static UINT8 *g_from_ptr;
/** From空間の終端アドレス */
static UINT8 *g_from_end;
/** To空間(将来のCOPY GCの退避先)の先頭アドレス */
static UINT8 *g_to_start;
/** To空間の次の割り当て位置(現状未使用) */
static UINT8 *g_to_ptr;
/** To空間の終端アドレス */
static UINT8 *g_to_end;

/**
 * From空間からnバイト(8byte境界に整列)を割り当てる。枯渇した場合は停止する。
 * @param n 割り当てるバイト数
 * @return 割り当てたメモリの先頭アドレス
 */
static lisp_addr_t os_alloc_bytes(UINT64 n) {
    UINT64 aligned = (n + 7) & ~7ULL;
#ifndef ISIKIOS_UNIT_TEST
    asm volatile ("cli");
#endif
    UINT8 *p = g_from_ptr;
    if (p +aligned > g_from_end) {
        // From空間が枯渇
        // TODO: GCを呼ぶ
        frame_buffer *fb = get_active_frame_buffer();
        fb->write_string(fb, "out of memory...");
        for (;;) {
        }
    }
    g_from_ptr = p + aligned;
#ifndef ISIKIOS_UNIT_TEST
    asm volatile ("sti");
#endif
    return (UINT64)p;
}

lisp_addr_t os_alloc_raw(UINT64 n) {
    return os_alloc_bytes(n);
}

/**
 * s1とs2の先頭size文字を、小文字/大文字の違いを無視して比較する。
 * @param s1 比較対象の文字列
 * @param s2 比較対象の文字列
 * @param size 比較する文字数
 * @return 一致すれば0、そうでなければstrcmpと同様の符号
 */
static int strncmpignorecase(const char *s1, const char *s2, UINT64 size) {
    // 小文字は大文字に変換して当てる
    while (size > 0 && (*s1 >0x60 && *s1 < 0x7b ? *s1 -0x20 : *s1) == (*s2 > 0x60 && *s2 < 0x7b ? *s2 - 0x20 : *s2)) {
      size--;
      s1++;
      s2++;
    }

    if (size == 0) {
       return 0;
    } else {
       return *s1 - *s2;
    }
}

/**
 * sをコピーしてstringオブジェクトを作る。uppercase_flagが立っていれば小文字を大文字化する。
 * @param s 文字列(NUL終端)
 * @param uppercase_flag 非0なら小文字を大文字化する(symbol名の正規化用)
 * @return タグ付けされたSTRING
 */
static lisp_val_t os_make_string_for(const char *s, int uppercase_flag) {
    UINT64 len = 0;
    while (s[len]) {
        len++;
    }

    lisp_addr_t addr = os_alloc_bytes(8 + len);
    lisp_val_t *header = (lisp_val_t *)addr;
    header[0] = len;
    UINT8 *bytes = (UINT8 *)(addr + 8);
    for (UINT64 i = 0; i < len; i++) {
        bytes[i] = uppercase_flag && s[i] > 0x60 && s[i] < 0x7b ? s[i] - 0x20 : s[i];
    }
    return (lisp_val_t)(addr | TAG_STRING);
}


/**
 * ヒープをheap_base〜heap_base+heap_sizeで初期化する。
 * @param heap_base ヒープの先頭アドレス
 * @param heap_size ヒープのサイズ(バイト)
 */
void os_heap_init(UINT64 heap_base, UINT64 heap_size) {
    // 将来的にCOPY GCを実装するためヒープを同サイズのFrom/To 2領域に分割しておく
    UINT64 half = (heap_size / 2) & ~7ULL;
    g_from_start = (UINT8 *)heap_base;
    g_from_ptr   = g_from_start;
    g_from_end   = g_from_start + half;
    g_to_start   = g_from_end;
    g_to_end     = (UINT8 *)(heap_base + heap_size);
    g_to_ptr     = g_to_start;
}

/** NIL・global_environment・組み込みシンボル/関数を構築し、Lisp実行環境を起動する */
void os_bootstrap() {
    // NIL の作成
    {
        lisp_addr_t addr = os_alloc_bytes(16);
        lisp_val_t tagged = (lisp_val_t)(addr | TAG_CONS);
        lisp_val_t *cell = (lisp_val_t *)addr;
        cell[0] = tagged;
        cell[1] = tagged;
        nil = tagged;
    }

    g_dynamic_bindings = nil;

    // global_environment の作成
    {
       global_environment = os_make_environment(os_make_symbol("GLOBAL-ENV"), nil);
    }

    // run queue の作成
    {
        g_sym_process_ready = os_make_symbol(":READY"); 
        os_set_variable(g_sym_process_ready, g_sym_process_ready, global_environment);

        g_sym_process_running = os_make_symbol(":RUNNING");
        os_set_variable(g_sym_process_running, g_sym_process_running, global_environment);

        g_sym_process_dead = os_make_symbol(":DEAD");
        os_set_variable(g_sym_process_dead, g_sym_process_dead, global_environment);

        g_sym_run_queue = os_make_symbol("*RUN-QUEUE*");
        os_set_variable(g_sym_run_queue, nil, global_environment);

        g_sym_current_process = os_make_symbol("*CURRENT-PROCESS*");
        os_set_variable(g_sym_current_process, nil, global_environment);
    }

    // global_environment に初期のシンボルを登録(intern)
    {
        
        g_sym_t = os_make_symbol("T");
        os_set_variable(g_sym_t, g_sym_t, global_environment);

        g_sym_quote = os_make_symbol("QUOTE");
        g_sym_if = os_make_symbol("IF");
        g_sym_progn = os_make_symbol("PROGN");
        g_sym_setq = os_make_symbol("SETQ");
        g_sym_defun = os_make_symbol("DEFUN");
        g_sym_lambda = os_make_symbol("LAMBDA");
        g_sym_defmacro = os_make_symbol("DEFMACRO");
        g_sym_block = os_make_symbol("BLOCK");
        g_sym_return_from = os_make_symbol("RETURN-FROM");
        g_sym_unwind_protect = os_make_symbol("UNWIND-PROTECT");
        g_sym_function = os_make_symbol("FUNCTION");
        g_sym_flet = os_make_symbol("FLET");
        g_sym_labels = os_make_symbol("LABELS");
        g_sym_defvar = os_make_symbol("DEFVAR");
        g_sym_defconstant = os_make_symbol("DEFCONSTANT");
        g_sym_defdynamic = os_make_symbol("DEFDYNAMIC");
        g_sym_defglobal = os_make_symbol("DEFGLOBAL");
        g_sym_dynamic = os_make_symbol("DYNAMIC");
        g_sym_rest = os_make_symbol("&REST");
        g_sym_quasiquote = os_make_symbol("QUASIQUOTE");
        g_sym_unquote = os_make_symbol("UNQUOTE");
        g_sym_unquote_splicing = os_make_symbol("UNQUOTE-SPLICING");
        g_sym_car = os_make_symbol("CAR");
        g_sym_cdr = os_make_symbol("CDR");
        g_sym_cons = os_make_symbol("CONS");

        g_sym_read_error = os_make_symbol("READ-ERROR");
        g_sym_eval_error = os_make_symbol("EVAL-ERROR");

        g_sym_top_level_block = os_make_symbol("%TOP-LEVEL");

        g_sym_catch = os_make_symbol("CATCH");
        g_sym_throw = os_make_symbol("THROW");
        g_sym_tagbody = os_make_symbol("TAGBODY");
        g_sym_go = os_make_symbol("GO");


        os_set_function(g_sym_car, os_make_native_function((lisp_addr_t)(void *)primitive_car), global_environment);
        os_set_function(g_sym_cdr, os_make_native_function((lisp_addr_t)(void *)primitive_cdr), global_environment);
        os_set_function(os_make_symbol("+"), os_make_native_function((lisp_addr_t)(void *)primitive_add), global_environment);
        os_set_function(os_make_symbol("-"), os_make_native_function((lisp_addr_t)(void *)primitive_subtract), global_environment);
        os_set_function(g_sym_cons, os_make_native_function((lisp_addr_t)(void *)primitive_cons), global_environment);
        os_set_function(os_make_symbol("EQ"), os_make_native_function((lisp_addr_t)(void *)primitive_eq), global_environment);
        os_set_function(os_make_symbol("NULL"), os_make_native_function((lisp_addr_t)(void *)primitive_null), global_environment);
        os_set_function(os_make_symbol("*"), os_make_native_function((lisp_addr_t)(void *)primitive_multiply), global_environment);
        os_set_function(os_make_symbol("/"), os_make_native_function((lisp_addr_t)(void *)primitive_divide), global_environment);
        os_set_function(os_make_symbol("<"), os_make_native_function((lisp_addr_t)(void *)primitive_less_than), global_environment);
        os_set_function(os_make_symbol(">"), os_make_native_function((lisp_addr_t)(void *)primitive_greater_than), global_environment);
        os_set_function(os_make_symbol("="), os_make_native_function((lisp_addr_t)(void *)primitive_num_equal), global_environment);
        os_set_function(os_make_symbol("/="), os_make_native_function((lisp_addr_t)(void *)primitive_num_not_equal), global_environment);
        os_set_function(os_make_symbol(">="), os_make_native_function((lisp_addr_t)(void *)primitive_greater_equal), global_environment);
        os_set_function(os_make_symbol("<="), os_make_native_function((lisp_addr_t)(void *)primitive_less_equal), global_environment);
        os_set_function(os_make_symbol("MAX"), os_make_native_function((lisp_addr_t)(void *)primitive_max), global_environment);
        os_set_function(os_make_symbol("MIN"), os_make_native_function((lisp_addr_t)(void *)primitive_min), global_environment);
        os_set_function(os_make_symbol("ABS"), os_make_native_function((lisp_addr_t)(void *)primitive_abs), global_environment);
        os_set_function(os_make_symbol("DIV"), os_make_native_function((lisp_addr_t)(void *)primitive_div), global_environment);
        os_set_function(os_make_symbol("MOD"), os_make_native_function((lisp_addr_t)(void *)primitive_mod), global_environment);
        os_set_function(os_make_symbol("GCD"), os_make_native_function((lisp_addr_t)(void *)primitive_gcd), global_environment);
        os_set_function(os_make_symbol("LCM"), os_make_native_function((lisp_addr_t)(void *)primitive_lcm), global_environment);
        os_set_function(os_make_symbol("ISQRT"), os_make_native_function((lisp_addr_t)(void *)primitive_isqrt), global_environment);
        os_set_function(os_make_symbol("NUMBERP"), os_make_native_function((lisp_addr_t)(void *)primitive_numberp), global_environment);
        os_set_function(os_make_symbol("FIXNUMP"), os_make_native_function((lisp_addr_t)(void *)primitive_fixnump), global_environment);
        os_set_function(os_make_symbol("BIGNUMP"), os_make_native_function((lisp_addr_t)(void *)primitive_bignump), global_environment);
        os_set_function(os_make_symbol("SYMBOLP"), os_make_native_function((lisp_addr_t)(void *)primitive_symbolp), global_environment);
        os_set_function(os_make_symbol("CONSP"), os_make_native_function((lisp_addr_t)(void *)primitive_consp), global_environment);
        os_set_function(os_make_symbol("EQL"), os_make_native_function((lisp_addr_t)(void *)primitive_eql), global_environment);
        os_set_function(os_make_symbol("EQUAL"), os_make_native_function((lisp_addr_t)(void *)primitive_equal), global_environment);
        // NOTはnullと仕様上完全に同一(「objがnilならt、それ以外はnil」)なので実体を共用する
        os_set_function(os_make_symbol("NOT"), os_make_native_function((lisp_addr_t)(void *)primitive_null), global_environment);
        os_set_function(os_make_symbol("LISTP"), os_make_native_function((lisp_addr_t)(void *)primitive_listp), global_environment);
        os_set_function(os_make_symbol("CHARACTERP"), os_make_native_function((lisp_addr_t)(void *)primitive_characterp), global_environment);
        os_set_function(os_make_symbol("CHAR="), os_make_native_function((lisp_addr_t)(void *)primitive_char_equal), global_environment);
        os_set_function(os_make_symbol("CHAR/="), os_make_native_function((lisp_addr_t)(void *)primitive_char_not_equal), global_environment);
        os_set_function(os_make_symbol("CHAR<"), os_make_native_function((lisp_addr_t)(void *)primitive_char_less_than), global_environment);
        os_set_function(os_make_symbol("CHAR>"), os_make_native_function((lisp_addr_t)(void *)primitive_char_greater_than), global_environment);
        os_set_function(os_make_symbol("CHAR<="), os_make_native_function((lisp_addr_t)(void *)primitive_char_less_equal), global_environment);
        os_set_function(os_make_symbol("CHAR>="), os_make_native_function((lisp_addr_t)(void *)primitive_char_greater_equal), global_environment);
        os_set_function(os_make_symbol("STRINGP"), os_make_native_function((lisp_addr_t)(void *)primitive_stringp), global_environment);
        os_set_function(os_make_symbol("FUNCTIONP"), os_make_native_function((lisp_addr_t)(void *)primitive_functionp), global_environment);
        os_set_function(os_make_symbol("GENERIC-FUNCTION-P"), os_make_native_function((lisp_addr_t)(void *)primitive_generic_function_p), global_environment);
        os_set_function(os_make_symbol("BASIC-ARRAY-P"), os_make_native_function((lisp_addr_t)(void *)primitive_basic_array_p), global_environment);
        // basic-array*-pとgeneral-array*-pは本実装では外延が一致するため実体を共用する
        os_set_function(os_make_symbol("BASIC-ARRAY*-P"), os_make_native_function((lisp_addr_t)(void *)primitive_array_star_p), global_environment);
        os_set_function(os_make_symbol("GENERAL-ARRAY*-P"), os_make_native_function((lisp_addr_t)(void *)primitive_array_star_p), global_environment);
        os_set_function(os_make_symbol("BASIC-VECTOR-P"), os_make_native_function((lisp_addr_t)(void *)primitive_basic_vector_p), global_environment);
        os_set_function(os_make_symbol("GENERAL-VECTOR-P"), os_make_native_function((lisp_addr_t)(void *)primitive_general_vector_p), global_environment);
        os_set_function(os_make_symbol("STREAMP"), os_make_native_function((lisp_addr_t)(void *)primitive_streamp), global_environment);
        os_set_function(os_make_symbol("SYMBOL-NAME"), os_make_native_function((lisp_addr_t)(void *)primitive_symbol_name), global_environment);
        os_set_function(os_make_symbol("STRING-TO-SYMBOL"), os_make_native_function((lisp_addr_t)(void *)primitive_string_to_symbol), global_environment);
        os_set_function(os_make_symbol("GENSYM"), os_make_native_function((lisp_addr_t)(void *)primitive_gensym), global_environment);
        os_set_function(os_make_symbol("MAKE-ARRAY"), os_make_native_function((lisp_addr_t)(void *)primitive_make_array), global_environment);
        os_set_function(os_make_symbol("AREF"), os_make_native_function((lisp_addr_t)(void *)primitive_aref), global_environment);
        os_set_function(os_make_symbol("ARRAY-DIMENSIONS"), os_make_native_function((lisp_addr_t)(void *)primitive_array_dimensions), global_environment);
        os_set_function(os_make_symbol("SET-CAR"), os_make_native_function((lisp_addr_t)(void *)primitive_set_car), global_environment);
        os_set_function(os_make_symbol("SET-CDR"), os_make_native_function((lisp_addr_t)(void *)primitive_set_cdr), global_environment);
        os_set_function(os_make_symbol("SET-AREF"), os_make_native_function((lisp_addr_t)(void *)primitive_set_aref), global_environment);
        os_set_function(os_make_symbol("VECTOR"), os_make_native_function((lisp_addr_t)(void *)primitive_vector), global_environment);
        os_set_function(os_make_symbol("CREATE-VECTOR"), os_make_native_function((lisp_addr_t)(void *)primitive_create_vector), global_environment);
        // GAREF/SET-GAREFはgeneral-array限定のaref/set-arefだが、本実装には
        // 配列のサブタイプが無く外延が一致するため、同じ実体を別シンボル名で共用する
        os_set_function(os_make_symbol("GAREF"), os_make_native_function((lisp_addr_t)(void *)primitive_aref), global_environment);
        os_set_function(os_make_symbol("SET-GAREF"), os_make_native_function((lisp_addr_t)(void *)primitive_set_aref), global_environment);
        os_set_function(os_make_symbol("CREATE-STRING"), os_make_native_function((lisp_addr_t)(void *)primitive_create_string), global_environment);
        os_set_function(os_make_symbol("STRING-ELT"), os_make_native_function((lisp_addr_t)(void *)primitive_string_elt), global_environment);
        os_set_function(os_make_symbol("STRING="), os_make_native_function((lisp_addr_t)(void *)primitive_string_equal), global_environment);
        os_set_function(os_make_symbol("STRING/="), os_make_native_function((lisp_addr_t)(void *)primitive_string_not_equal), global_environment);
        os_set_function(os_make_symbol("STRING<"), os_make_native_function((lisp_addr_t)(void *)primitive_string_less_than), global_environment);
        os_set_function(os_make_symbol("STRING>"), os_make_native_function((lisp_addr_t)(void *)primitive_string_greater_than), global_environment);
        os_set_function(os_make_symbol("STRING<="), os_make_native_function((lisp_addr_t)(void *)primitive_string_less_equal), global_environment);
        os_set_function(os_make_symbol("STRING>="), os_make_native_function((lisp_addr_t)(void *)primitive_string_greater_equal), global_environment);
        os_set_function(os_make_symbol("CHAR-INDEX"), os_make_native_function((lisp_addr_t)(void *)primitive_char_index), global_environment);
        os_set_function(os_make_symbol("STRING-INDEX"), os_make_native_function((lisp_addr_t)(void *)primitive_string_index), global_environment);
        os_set_function(os_make_symbol("STRING-APPEND"), os_make_native_function((lisp_addr_t)(void *)primitive_string_append), global_environment);
        os_set_function(os_make_symbol("LENGTH"), os_make_native_function((lisp_addr_t)(void *)primitive_length), global_environment);
        os_set_function(os_make_symbol("%%MAKE-CLASS-RAW"), os_make_native_function((lisp_addr_t)(void *)primitive_make_class_raw), global_environment);
        os_set_function(os_make_symbol("%%CLASS-NAME"), os_make_native_function((lisp_addr_t)(void *)primitive_class_name), global_environment);
        os_set_function(os_make_symbol("%%CLASS-SUPERS"), os_make_native_function((lisp_addr_t)(void *)primitive_class_supers), global_environment);
        os_set_function(os_make_symbol("%%CLASS-SLOTS"), os_make_native_function((lisp_addr_t)(void *)primitive_class_slots), global_environment);
        os_set_function(os_make_symbol("%%CLASSP"), os_make_native_function((lisp_addr_t)(void *)primitive_classp), global_environment);
        os_set_function(os_make_symbol("%%MAKE-INSTANCE-RAW"), os_make_native_function((lisp_addr_t)(void *)primitive_make_instance_raw), global_environment);
        os_set_function(os_make_symbol("%%INSTANCE-CLASS"), os_make_native_function((lisp_addr_t)(void *)primitive_instance_class), global_environment);
        os_set_function(os_make_symbol("%%INSTANCE-SLOTS"), os_make_native_function((lisp_addr_t)(void *)primitive_instance_slots), global_environment);
        os_set_function(os_make_symbol("%%CLASS-INSTANCE-P"), os_make_native_function((lisp_addr_t)(void *)primitive_class_instance_p), global_environment);
        os_set_function(os_make_symbol("%%SET-DYNAMIC"), os_make_native_function((lisp_addr_t)(void *)primitive_set_dynamic), global_environment);
    }
}


/**
 * envおよびその親を順に辿り、symの変数の値を取得する。
 * @param sym 検索するsymbol
 * @param env 検索を開始する環境
 * @return 見つかった値。未定義の場合はnil
 */
lisp_val_t os_get_variable(lisp_val_t sym, lisp_val_t env) {
    lisp_val_t current_env = env;

    /*
     * env の構造
     * '((name . env-name)
     *   (variables . ((sym2 . val1)
     *                 (sym2 . val2)
     *                 (sym3 . val3)))
     *   (functions . ((sym1 . fn1)
     *                 (sym2 . fn2)
     *                 (sym3 . fn3)))
     *   (parent . ((name . parent-env-name)
     *              (variables . ((p-sym1 . p-val1)
     *                            (p-sym2 . p-val2)
     *                            (p-sym3 . p-val3)))
     *              (functions . ((p-sym1 . p-fn1)
     *                            (p-sym2 . p-fn2)
     *                            (p-sym3 . p-fn3)))
     *              (parent . ()))))
     */

    while (current_env != nil) {
        // 現在の環境から variables slot (cadr) を取得
        lisp_val_t va_slot = cc_car(cc_cdr(current_env));

        // variables slot の cdr にある alist を取得
        lisp_val_t alist = cc_cdr(va_slot);

        // alist に対して assoc
        lisp_val_t pair = cc_assoc_eq(sym, alist);

        if (pair != nil) {
            // みつかった pair の cdr を返す
            return cc_cdr(pair);
        }


        // 現在の環境で見つからなければ parent の環境で探す
        lisp_val_t cell1 = cc_cdr(current_env); // cdr
        lisp_val_t cell2 = cc_cdr(cell1); // cddr
        lisp_val_t cell3 = cc_cdr(cell2); // cdddr
        lisp_val_t par_slot = cc_car(cell3); // cadddr (parent . env)

        current_env = cc_cdr(par_slot); // parent の値
    }


    // TODO: UNBOUND VARIABLE 等を返し、評価のタイミングでエラーとする
    return nil;
}


/**
 * envおよびその親を順に辿り、symの関数定義を取得する。
 * @param sym 検索するsymbol
 * @param env 検索を開始する環境
 * @return 見つかった関数オブジェクト。未定義の場合はnil
 */
lisp_val_t os_get_function(lisp_val_t sym, lisp_val_t env) {
    lisp_val_t current_env = env;

    /*
     * env の構造
     * '((name . env-name)
     *   (variables . ((sym2 . val1)
     *                 (sym2 . val2)
     *                 (sym3 . val3)))
     *   (functions . ((sym1 . fn1)
     *                 (sym2 . fn2)
     *                 (sym3 . fn3)))
     *   (parent . ((name . parent-env-name)
     *              (variables . ((p-sym1 . p-val1)
     *                            (p-sym2 . p-val2)
     *                            (p-sym3 . p-val3)))
     *              (functions . ((p-sym1 . p-fn1)
     *                            (p-sym2 . p-fn2)
     *                            (p-sym3 . p-fn3)))
     *              (parent . ()))))
     */

     while (current_env != nil) {
         // 現在の環境から functions slot (caddr) を取得
         lisp_val_t func_slot = cc_car(cc_cdr(cc_cdr(current_env)));

         // functions slot の cdr にある alist を取得
         lisp_val_t alist = cc_cdr(func_slot);

         // alist に対して assoc
         lisp_val_t pair = cc_assoc_eq(sym, alist);

         if (pair != nil) {
             // みつかった pair の cdr を返す
             return cc_cdr(pair);
         }

         // 現在の環境で見つからなければ parent の環境で探す
         lisp_val_t cell1 = cc_cdr(current_env); // cdr
         lisp_val_t cell2 = cc_cdr(cell1); // cddr
         lisp_val_t cell3 = cc_cdr(cell2); // cdddr
         lisp_val_t par_slot = cc_car(cell3); // cadddr (parent . env)

         current_env = cc_cdr(par_slot); // parent の値
     }

     // 未定義の関数
     return nil;
}

/**
 * 非負のfixnumオブジェクトを作る(即値、ヒープ確保なし、符号は常に0)。
 * @param fixnum 表現する値(0〜2^60-1)
 * @return タグ付けされたFIXNUM
 */
lisp_val_t os_make_fixnum(const UINT64 fixnum) {
    return (lisp_val_t)(fixnum << 3);
}

/**
 * 符号付きのfixnumオブジェクトを作る(即値、ヒープ確保なし)。
 * @param negative 0以外を渡すと負数として作る
 * @param magnitude 絶対値(0〜2^60-1)
 * @return タグ付けされたFIXNUM
 */
lisp_val_t os_make_fixnum_signed(int negative, UINT64 magnitude) {
    lisp_val_t val = (lisp_val_t)(magnitude << 3);
    if (negative && magnitude != 0) {
        val |= FIXNUM_SIGN_BIT;
    }
    return val;
}

/**
 * FIXNUMのマグニチュード(絶対値)を取り出す。
 * @param val タグ付けされたFIXNUM
 * @return 0〜2^60-1のマグニチュード
 */
UINT64 os_fixnum_magnitude(lisp_val_t val) {
    return (val >> 3) & FIXNUM_MAGNITUDE_MASK;
}

/**
 * FIXNUMが負数かどうかを判定する。
 * @param val タグ付けされたFIXNUM
 * @return 負数なら0以外、そうでなければ0
 */
int os_fixnum_is_negative(lisp_val_t val) {
    return (val & FIXNUM_SIGN_BIT) != 0;
}


/**
 * cons cellをヒープに確保する。
 * @param car carに入れる値
 * @param cdr cdrに入れる値
 * @return タグ付けされたCONS
 */
lisp_val_t os_make_cons(const lisp_val_t car, const lisp_val_t cdr) {
    lisp_addr_t cons_addr = os_alloc_bytes(16);
    lisp_val_t *cell = (lisp_val_t *)cons_addr;
    cell[0] = car;
    cell[1] = cdr;
    return (lisp_val_t)(cons_addr | TAG_CONS);
}


/**
 * name(大文字化される)のsymbolを返す。既に同名のsymbolがinternされていればそれを返す(interning)。
 * @param name symbol名
 * @return タグ付けされたSYMBOL
 */
lisp_val_t os_make_symbol(const char *name) {
    for (int i = 0; i < g_symbol_count; i++) {
        lisp_val_t sym = g_symbol_table[i];
        lisp_addr_t sym_addr = sym & ~TAG_MASK;
   
        lisp_val_t str_obj = ((lisp_val_t *)sym_addr)[0];
        lisp_addr_t str_addr = str_obj & ~TAG_MASK;
        UINT64 len = ((UINT64 *)str_addr)[0];
        const char *sym_name = (const char *)(str_addr + 8);

        if (strncmpignorecase(sym_name, name, len) == 0 && name[len] == '\0') {
            return sym;
        }
   }

    lisp_val_t name_str = os_make_string_for(name, 1 /* uppercase */);
    lisp_addr_t addr = os_alloc_bytes(32);
    lisp_val_t *sym = (lisp_val_t *)addr;
    sym[0] = name_str; // string へのポインタ
    sym[1] = nil;      // value
    sym[2] = nil;      // function
    sym[3] = nil;      // reserved
    lisp_val_t tagged = (lisp_val_t)(addr | TAG_SYMBOL);

    if (g_symbol_count < MAX_SYMBOLS) {
        g_symbol_table[g_symbol_count++] = tagged;
    }

    return tagged;
}


/**
 * charオブジェクトを作る(即値、ヒープ確保なし)。
 * @param c 表現する文字
 * @return タグ付けされたCHAR
 */
lisp_val_t os_make_char(const char c) {
    return ((lisp_val_t)c) << 3 | TAG_CHAR;
}

/**
 * sをコピーしてstringオブジェクトを作る。
 * @param s 文字列(NUL終端)
 * @return タグ付けされたSTRING
 */
lisp_val_t os_make_string(const char *s) {
    return os_make_string_for(s, 0 /* normal */);
}

void os_string_to_cstr(lisp_val_t str, char *out, UINT32 out_cap) {
    lisp_addr_t addr = (lisp_addr_t)(str & ~TAG_MASK);
    UINT64 len = ((UINT64 *)addr)[0];
    const UINT8 *bytes = (const UINT8 *)(addr + 8);

    UINT32 copy_len = (UINT32)len;
    if (out_cap > 0 && copy_len > out_cap - 1) {
        copy_len = out_cap - 1;
    }
    for (UINT32 i = 0; i < copy_len; i++) {
        out[i] = (char)bytes[i];
    }
    if (out_cap > 0) {
        out[copy_len] = '\0';
    }
}


/**
 * TAG_INSTANCE(32byte、magic+3word)のオブジェクトをヒープに確保する。
 * @param magic インスタンスの種別を表すMAGIC NUMBER
 * @param w1 word1に入れる値
 * @param w2 word2に入れる値
 * @param w3 word3に入れる値
 * @return タグ付けされたINSTANCE
 */
lisp_val_t os_make_instance(UINT64 magic, UINT64 w1, UINT64 w2, UINT64 w3) {
    lisp_addr_t addr = os_alloc_bytes(32);
    UINT64 *obj = (UINT64 *)addr;
    obj[0] = magic;
    obj[1] = w1;
    obj[2] = w2;
    obj[3] = w3;
    return (lisp_val_t)(addr | TAG_INSTANCE);
}


/**
 * parent_envを親とする新しい環境(name/variables/functions/parentの4slotを持つリスト)を作る。
 * TODO: 特定の環境を一発で取得するにはnameで指定するしかないが、呼び出し元が渡すenv_symbolは
 * ユニークであることが保証されていない。将来的に多値を返せるようにしたうえで、ここでユニークな
 * symbolを生成し、環境本体とそのユニークなsymbolの両方を返すようにしたい。
 * @param env_symbol 環境の名前を表すsymbol
 * @param parent_env 親環境。ルート環境の場合はnil
 * @return 作成した環境
 */
lisp_val_t os_make_environment(lisp_val_t env_symbol, lisp_val_t parent_env) {
    // TODO: env_name が TAG_SYMBOL のチェック


    /*-
     * (defun make-environment (env-name parent-env)
     *   (list
     *     (cons 'name env-symbol)
     *     (cons 'variables nil)
     *     (cons 'functions nil)
     *     (cons 'parent parent-env)
     *     (cons 'constants nil)))
     *
     * constantsは既存のos_get_variable/os_get_function/os_set_variable/os_set_functionが
     * 4番目(parent)までしか固定位置参照しないため、末尾に追加するだけで既存コードに影響しない
     */
    lisp_val_t name_symbol = os_make_symbol("name");
    lisp_val_t variables_symbol = os_make_symbol("variables");
    lisp_val_t functions_symbol = os_make_symbol("functions");
    lisp_val_t parent_symbol = os_make_symbol("parent");
    lisp_val_t constants_symbol = os_make_symbol("constants");

    lisp_val_t name_slot = os_make_cons(name_symbol, env_symbol);
    lisp_val_t variables_slot = os_make_cons(variables_symbol, nil);
    lisp_val_t functions_slot = os_make_cons(functions_symbol, nil);
    lisp_val_t parent_slot = os_make_cons(parent_symbol, parent_env);
    lisp_val_t constants_slot = os_make_cons(constants_symbol, nil);

    lisp_val_t list_step4 = os_make_cons(constants_slot, nil);
    lisp_val_t list_step3 = os_make_cons(parent_slot, list_step4);
    lisp_val_t list_step2 = os_make_cons(functions_slot, list_step3);
    lisp_val_t list_step1 = os_make_cons(variables_slot, list_step2);
    lisp_val_t env_obj = os_make_cons(name_slot, list_step1);

    return env_obj;
}

/**
 * symがenv自身(親は辿らない)のconstantsスロットに登録されているかどうかを判定する。
 * @param sym 判定するsymbol
 * @param env 判定対象の環境(このenv自身のスロットのみを見る)
 * @return 定数として登録されていればnon-zero
 */
int os_is_constant(lisp_val_t sym, lisp_val_t env) {
    lisp_val_t const_slot = cc_car(cc_cdr(cc_cdr(cc_cdr(cc_cdr(env)))));
    lisp_val_t alist = cc_cdr(const_slot);
    return cc_assoc_eq(sym, alist) != nil;
}

/**
 * envのconstantsスロットにsymを定数として登録する(既に登録済みなら何もしない)。
 * @param sym 登録するsymbol
 * @param env 登録先の環境
 */
void os_mark_constant(lisp_val_t sym, lisp_val_t env) {
    lisp_val_t const_slot = cc_car(cc_cdr(cc_cdr(cc_cdr(cc_cdr(env)))));
    lisp_addr_t const_slot_addr = const_slot & ~TAG_MASK;
    lisp_val_t alist = cc_cdr(const_slot);

    if (cc_assoc_eq(sym, alist) != nil) {
        return;
    }
    lisp_val_t new_pair = os_make_cons(sym, g_sym_t);
    ((lisp_val_t *)const_slot_addr)[1] = os_make_cons(new_pair, alist);
}

/**
 * g_dynamic_bindingsからsymの動的変数の値を取得する(レキシカルなenvの親子関係とは無関係)。
 * @param sym 検索するsymbol
 * @return 見つかった値。未定義の場合はnil
 */
lisp_val_t os_get_dynamic(lisp_val_t sym) {
    lisp_val_t pair = cc_assoc_eq(sym, g_dynamic_bindings);
    return (pair != nil) ? cc_cdr(pair) : nil;
}

/**
 * g_dynamic_bindingsにsymの動的変数の値としてvalを設定する(既存なら破壊的に上書き、無ければ新規追加)。
 * @param sym 設定するsymbol
 * @param val 設定する値
 * @return val 自身
 */
lisp_val_t os_set_dynamic(lisp_val_t sym, lisp_val_t val) {
    lisp_val_t existing_pair = cc_assoc_eq(sym, g_dynamic_bindings);
    if (existing_pair != nil) {
        ((lisp_val_t *)(existing_pair & ~TAG_MASK))[1] = val;
    } else {
        lisp_val_t new_pair = os_make_cons(sym, val);
        g_dynamic_bindings = os_make_cons(new_pair, g_dynamic_bindings);
    }
    return val;
}


/**
 * fnptrをネイティブ(C)関数として呼び出すTAG_INSTANCEオブジェクトを作る。
 * @param fnptr 呼び出すC関数のアドレス
 * @return MAGIC_FUNCTION_NATIVEのINSTANCE
 */
lisp_val_t os_make_native_function(UINT64 fnptr) {
    return os_make_instance(MAGIC_FUNCTION_NATIVE, fnptr, nil, nil);
}


/**
 * envの変数slotにsymの値としてvalを設定する(既存なら破壊的に上書き、無ければ新規追加)。
 * @param sym 設定するsymbol
 * @param val 設定する値
 * @param env 設定先の環境
 * @return val 自身
 */
lisp_val_t os_set_variable(lisp_val_t sym, lisp_val_t val, lisp_val_t env) {
    // current environment の (variables . alist) のペアを取り出す(cadr)
    lisp_val_t var_slot = cc_car(cc_cdr(env));
    lisp_addr_t var_slot_addr = var_slot & ~TAG_MASK;

    lisp_val_t alist = cc_cdr(var_slot); // cdr (alist)
    lisp_val_t existing_pair = cc_assoc_eq(sym, alist);

    if (existing_pair != nil) {
        // すでに存在する場合は cdr を破壊的に書き換える
        ((lisp_val_t *)(existing_pair & ~TAG_MASK))[1] = val;
    } else {
        // 新規追加
        lisp_val_t new_pair = os_make_cons(sym, val);
        // (push new-pair alist)
        ((lisp_val_t *)var_slot_addr)[1] = os_make_cons(new_pair, alist);
    }

    return val;
}


/**
 * envの関数slotにsymの関数定義としてfn_objを設定する(既存なら破壊的に上書き、無ければ新規追加)。
 * @param sym 設定するsymbol
 * @param fn_obj 設定する関数オブジェクト
 * @param env 設定先の環境
 * @return fn_obj 自身
 */
lisp_val_t os_set_function(lisp_val_t sym, lisp_val_t fn_obj, lisp_val_t env) {
    // current environment から (functions . alist) のペアを取り出すため、 cddr を取る
    lisp_val_t next_cell = cc_cdr(cc_cdr(env));

    // (functions . alist) のペアを取り出す
    lisp_val_t func_slot = cc_car(next_cell);
    lisp_addr_t func_slot_addr = func_slot & ~TAG_MASK;

    lisp_val_t alist = cc_cdr(func_slot); // cdr (alist)
    lisp_val_t existing_pair = cc_assoc_eq(sym, alist);

    if (existing_pair != nil) {
        // すでに存在する場合は cdr を破壊的に書き換える
        ((lisp_val_t *)(existing_pair & ~TAG_MASK))[1] = fn_obj;
    } else {
        // 新規追加
        lisp_val_t new_pair = os_make_cons(sym, fn_obj);
        // (push new-pair alist)
        ((lisp_val_t *)func_slot_addr)[1] = os_make_cons(new_pair, alist);
    }

    return fn_obj;
}



/*
 * ---- 符号なしマグニチュード(limb配列、基数2^32)の下位ヘルパー ----
 *
 * 各limbはUINT64配列の下位32bitのみを使用する。limbs[0]が最下位(リトルエンディアン)。
 * 正しさ・実装の単純さを速度より優先した素朴な実装(乗算はO(n*m)、除算は1bitずつの
 * シフト&サブトラクトによる長除算)。
 */

/**
 * limbsの末尾(上位側)の0limbを無視した実効長を返す(最低1)。
 * @param limbs limb配列
 * @param count limbsの要素数
 * @return 実効長
 */
static UINT64 mag_len(const UINT64 *limbs, UINT64 count) {
    while (count > 1 && limbs[count - 1] == 0) {
        count--;
    }
    return count;
}

/**
 * aとbの大小を比較する(末尾の0limbは無視する)。
 * @return a<bなら負、a==bなら0、a>bなら正
 */
static int mag_compare(const UINT64 *a, UINT64 alen, const UINT64 *b, UINT64 blen) {
    alen = mag_len(a, alen);
    blen = mag_len(b, blen);
    if (alen != blen) {
        return alen < blen ? -1 : 1;
    }
    for (UINT64 i = alen; i > 0; i--) {
        if (a[i - 1] != b[i - 1]) {
            return a[i - 1] < b[i - 1] ? -1 : 1;
        }
    }
    return 0;
}

/**
 * out = a + b (符号なし)。outはmax(alen,blen)+1以上の容量を持つこと。
 * @return outの実効長
 */
static UINT64 mag_add(const UINT64 *a, UINT64 alen, const UINT64 *b, UINT64 blen, UINT64 *out) {
    UINT64 n = alen > blen ? alen : blen;
    UINT64 carry = 0;
    for (UINT64 i = 0; i < n; i++) {
        UINT64 av = i < alen ? a[i] : 0;
        UINT64 bv = i < blen ? b[i] : 0;
        UINT64 sum = av + bv + carry;
        out[i] = sum & 0xFFFFFFFFULL;
        carry = sum >> 32;
    }
    out[n] = carry;
    return mag_len(out, n + 1);
}

/**
 * out = a - b (符号なし、a>=b前提)。outはalen以上の容量を持つこと。
 * @return outの実効長
 */
static UINT64 mag_sub(const UINT64 *a, UINT64 alen, const UINT64 *b, UINT64 blen, UINT64 *out) {
    UINT64 borrow = 0;
    for (UINT64 i = 0; i < alen; i++) {
        UINT64 av = a[i];
        UINT64 bv = i < blen ? b[i] : 0;
        UINT64 diff = av - bv - borrow;
        out[i] = diff & 0xFFFFFFFFULL;
        borrow = (av < bv + borrow) ? 1 : 0;
    }
    return mag_len(out, alen);
}

/**
 * out = a * b (符号なし、素朴なO(n*m)乗算)。outはalen+blen以上の容量を持つこと。
 * @return outの実効長
 */
static UINT64 mag_mul(const UINT64 *a, UINT64 alen, const UINT64 *b, UINT64 blen, UINT64 *out) {
    UINT64 n = alen + blen;
    for (UINT64 i = 0; i < n; i++) {
        out[i] = 0;
    }
    for (UINT64 i = 0; i < alen; i++) {
        UINT64 carry = 0;
        for (UINT64 j = 0; j < blen; j++) {
            UINT64 prod = a[i] * b[j] + out[i + j] + carry;
            out[i + j] = prod & 0xFFFFFFFFULL;
            carry = prod >> 32;
        }
        out[i + blen] += carry;
    }
    return mag_len(out, n);
}

/**
 * aをbで割った商をquot、余りをremに格納する(符号なし、1bitずつのシフト&サブトラクトによる
 * 素朴な長除算)。bは0でないこと。quot/remはいずれもalen以上の容量を持つこと。
 * @param quot_len 商の実効長の格納先
 * @param rem_len 余りの実効長の格納先
 */
static void mag_divmod(const UINT64 *a, UINT64 alen, const UINT64 *b, UINT64 blen,
                        UINT64 *quot, UINT64 *quot_len, UINT64 *rem, UINT64 *rem_len) {
    alen = mag_len(a, alen);
    blen = mag_len(b, blen);
    UINT64 total_bits = alen * 32;

    for (UINT64 i = 0; i < alen; i++) {
        quot[i] = 0;
        rem[i] = 0;
    }

    for (UINT64 bit = total_bits; bit > 0; bit--) {
        UINT64 idx = bit - 1;

        // rem = rem << 1 (limb間の桁上がりを伝播する)
        UINT64 carry = 0;
        for (UINT64 i = 0; i < alen; i++) {
            UINT64 v = (rem[i] << 1) | carry;
            carry = (rem[i] >> 31) & 1;
            rem[i] = v & 0xFFFFFFFFULL;
        }
        // remの最下位bitに、aのidxビット目を立てる
        rem[0] |= (a[idx / 32] >> (idx % 32)) & 1;

        UINT64 rlen = mag_len(rem, alen);
        if (mag_compare(rem, rlen, b, blen) >= 0) {
            mag_sub(rem, rlen, b, blen, rem);
            quot[idx / 32] |= (1ULL << (idx % 32));
        }
    }

    *quot_len = mag_len(quot, alen);
    *rem_len = mag_len(rem, alen);
}

/**
 * limbs(count個)を「limbs*mul + add」に置き換える(mul/addは小さい正数を想定、桁上がり分は
 * limbs[count]以降に書き込む。呼び出し側はその分の容量を確保しておくこと)。
 * リーダの10進リテラル桁蓄積で使う。
 * @return 更新後の実効長
 */
UINT64 mag_mul_small_add_small(UINT64 *limbs, UINT64 count, UINT64 mul, UINT64 add) {
    UINT64 carry = add;
    UINT64 i;
    for (i = 0; i < count; i++) {
        UINT64 prod = limbs[i] * mul + carry;
        limbs[i] = prod & 0xFFFFFFFFULL;
        carry = prod >> 32;
    }
    while (carry != 0) {
        limbs[i] = carry & 0xFFFFFFFFULL;
        carry >>= 32;
        i++;
    }
    return mag_len(limbs, i > count ? i : count);
}

/**
 * limbs(count個)をdiv(小さい正数)で割った商をlimbsに書き戻し、余りを*remに格納する。
 * プリンタの10進変換(10で割ったあまりを繰り返し取り出す)で使う。
 * @return 商の実効長(mag_len適用後)
 */
UINT64 mag_divmod_small(UINT64 *limbs, UINT64 count, UINT64 div, UINT64 *rem) {
    UINT64 r = 0;
    for (UINT64 i = count; i > 0; i--) {
        UINT64 cur = (r << 32) | limbs[i - 1];
        limbs[i - 1] = cur / div;
        r = cur % div;
    }
    *rem = r;
    return mag_len(limbs, count);
}

/** FIXNUM/bignumを同じ形で扱うための符号付きマグニチュードのビュー */
typedef struct {
    int sign;
    UINT64 *limbs;
    UINT64 count;
    UINT64 fixnum_buf[2]; /* FIXNUM(60bit、最大2limb)を展開する場合の受け皿 */
} signed_mag_t;

/**
 * FIXNUMまたはMAGIC_BIGNUMのINSTANCEを符号付きマグニチュードのビューに分解する。
 * bignumのlimb配列はコピーせずオブジェクト自身の配列を直接指す。
 */
static void decompose(lisp_val_t v, signed_mag_t *out) {
    if ((v & TAG_MASK) == TAG_FIXNUM) {
        UINT64 magnitude = os_fixnum_magnitude(v);
        out->sign = os_fixnum_is_negative(v);
        out->fixnum_buf[0] = magnitude & 0xFFFFFFFFULL;
        out->fixnum_buf[1] = magnitude >> 32;
        out->limbs = out->fixnum_buf;
        out->count = mag_len(out->fixnum_buf, 2);
    } else {
        UINT64 *obj = (UINT64 *)(v & ~TAG_MASK);
        out->sign = (int)obj[1];
        out->limbs = (UINT64 *)obj[3];
        out->count = obj[2];
    }
}

/**
 * 符号付きマグニチュード(limbs, count)から整数オブジェクトを作る。
 * 正規化後マグニチュードが60bit以内に収まる場合はFIXNUM(即値)に降格し、
 * それ以外はlimb配列をコピーしてヒープに確保しMAGIC_BIGNUMのINSTANCEを返す。
 */
lisp_val_t os_make_integer(int sign, UINT64 *limbs, UINT64 count) {
    count = mag_len(limbs, count);
    if (count == 1 && limbs[0] == 0) {
        sign = 0;
    }

    if (count <= 2) {
        UINT64 magnitude = limbs[0];
        if (count == 2) {
            magnitude |= limbs[1] << 32;
        }
        if (magnitude <= FIXNUM_MAGNITUDE_MASK) {
            return os_make_fixnum_signed(sign, magnitude);
        }
    }

    lisp_addr_t limb_addr = os_alloc_bytes(8 * count);
    UINT64 *dst = (UINT64 *)limb_addr;
    for (UINT64 i = 0; i < count; i++) {
        dst[i] = limbs[i];
    }
    return os_make_instance(MAGIC_BIGNUM, (UINT64)sign, count, (UINT64)limb_addr);
}

/**
 * 2つのMAGIC_BIGNUMオブジェクトのsign+limb内容を比較する(構造上の同値性の判定に使う)。
 * @return 同値なら0以外、そうでなければ0
 */
static int bignum_equal(const UINT64 *obj_a, const UINT64 *obj_b) {
    if (obj_a[1] != obj_b[1] || obj_a[2] != obj_b[2]) {
        return 0;
    }
    const UINT64 *limbs_a = (const UINT64 *)obj_a[3];
    const UINT64 *limbs_b = (const UINT64 *)obj_b[3];
    for (UINT64 i = 0; i < obj_a[2]; i++) {
        if (limbs_a[i] != limbs_b[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * 2つの整数(FIXNUM/bignum)の大小を比較する。両方FIXNUMの場合はヒープ確保なしの
 * 高速パスを使う。
 * @return a<bなら負、a==bなら0、a>bなら正
 */
static int number_compare(lisp_val_t a, lisp_val_t b) {
    if ((a & TAG_MASK) == TAG_FIXNUM && (b & TAG_MASK) == TAG_FIXNUM) {
        int neg_a = os_fixnum_is_negative(a);
        int neg_b = os_fixnum_is_negative(b);
        if (neg_a != neg_b) {
            return neg_a ? -1 : 1;
        }
        UINT64 mag_a = os_fixnum_magnitude(a);
        UINT64 mag_b = os_fixnum_magnitude(b);
        int cmp = mag_a < mag_b ? -1 : (mag_a > mag_b ? 1 : 0);
        return neg_a ? -cmp : cmp;
    }

    signed_mag_t ma, mb;
    decompose(a, &ma);
    decompose(b, &mb);
    if (ma.sign != mb.sign) {
        return ma.sign ? -1 : 1;
    }
    int cmp = mag_compare(ma.limbs, ma.count, mb.limbs, mb.count);
    return ma.sign ? -cmp : cmp;
}

/**
 * 整数z1をz2で除した「floor除算」の商と余りを求める(ISLisp仕様のdiv/mod。素朴な
 * 切り捨て除算である/とは異なり、商は-∞方向へ切り捨て、余りの符号は常にz2の符号に一致する)。
 * 既存のmag_divmod(切り捨て除算)の結果を符号に応じて調整することで実現する。
 * z1 = (*div_out)*z2 + (*mod_out) となる。
 * @param div_out 商(floor)の格納先
 * @param mod_out 余り(符号はz2に一致)の格納先
 * @param div_by_zero z2が0の場合に1を設定する(このときdiv_out/mod_outは未定義)
 */
static void floor_divmod(lisp_val_t z1, lisp_val_t z2, lisp_val_t *div_out, lisp_val_t *mod_out, int *div_by_zero) {
    signed_mag_t m1, m2;
    decompose(z1, &m1);
    decompose(z2, &m2);

    if (m2.count == 1 && m2.limbs[0] == 0) {
        *div_by_zero = 1;
        return;
    }
    *div_by_zero = 0;

    UINT64 *quot_buf = (UINT64 *)os_alloc_bytes(8 * m1.count);
    UINT64 *rem_buf = (UINT64 *)os_alloc_bytes(8 * m1.count);
    UINT64 quot_len, rem_len;
    mag_divmod(m1.limbs, m1.count, m2.limbs, m2.count, quot_buf, &quot_len, rem_buf, &rem_len);

    if (rem_len == 1 && rem_buf[0] == 0) {
        // 割り切れる: mod=0、divの符号はオペランドの符号のXOR
        int div_sign = (m1.sign != m2.sign);
        *div_out = os_make_integer(div_sign, quot_buf, quot_len);
        *mod_out = os_make_fixnum(0);
        return;
    }

    if (m1.sign == m2.sign) {
        // 同符号: truncate除算とfloor除算が一致する
        *div_out = os_make_integer(0, quot_buf, quot_len);
        *mod_out = os_make_integer(m2.sign, rem_buf, rem_len);
        return;
    }

    // 異符号: div = -(q_trunc+1)、mod = |z2| - r_trunc (符号はz2に一致)
    UINT64 one[1] = {1};
    UINT64 *div_mag_buf = (UINT64 *)os_alloc_bytes(8 * (quot_len + 1));
    UINT64 div_mag_len = mag_add(quot_buf, quot_len, one, 1, div_mag_buf);
    *div_out = os_make_integer(1, div_mag_buf, div_mag_len);

    UINT64 *mod_mag_buf = (UINT64 *)os_alloc_bytes(8 * m2.count);
    UINT64 mod_mag_len = mag_sub(m2.limbs, m2.count, rem_buf, rem_len, mod_mag_buf);
    *mod_out = os_make_integer(m2.sign, mod_mag_buf, mod_mag_len);
}

/**
 * aとbの最大公約数のマグニチュードを求める(符号なし、ユークリッドの互除法。
 * mag_divmodの繰り返しで実装する)。gcd(0,0)=0、gcd(a,0)=aとなる。
 * @param out_limbs 結果のlimb配列(新規にヒープ確保したもの)の格納先
 * @param out_len 結果の実効長の格納先
 */
static void mag_gcd(UINT64 *a, UINT64 alen, UINT64 *b, UINT64 blen, UINT64 **out_limbs, UINT64 *out_len) {
    alen = mag_len(a, alen);
    blen = mag_len(b, blen);

    UINT64 *cur_a = (UINT64 *)os_alloc_bytes(8 * alen);
    for (UINT64 i = 0; i < alen; i++) {
        cur_a[i] = a[i];
    }
    UINT64 cur_alen = alen;

    UINT64 *cur_b = (UINT64 *)os_alloc_bytes(8 * blen);
    for (UINT64 i = 0; i < blen; i++) {
        cur_b[i] = b[i];
    }
    UINT64 cur_blen = blen;

    while (!(cur_blen == 1 && cur_b[0] == 0)) {
        UINT64 *quot_buf = (UINT64 *)os_alloc_bytes(8 * cur_alen);
        UINT64 *rem_buf = (UINT64 *)os_alloc_bytes(8 * cur_alen);
        UINT64 quot_len, rem_len;
        mag_divmod(cur_a, cur_alen, cur_b, cur_blen, quot_buf, &quot_len, rem_buf, &rem_len);

        cur_a = cur_b;
        cur_alen = cur_blen;
        cur_b = rem_buf;
        cur_blen = rem_len;
    }

    *out_limbs = cur_a;
    *out_len = cur_alen;
}

/**
 * nのマグニチュードの整数平方根floor(sqrt(n))を求める(符号なし、ニュートン法。
 * x=n, y=(x+1)/2 から始め、y<xである限りx=y, y=(x+n/x)/2を繰り返す)。
 * mag_add/mag_divmod/mag_divmod_small/mag_compareのみで実装し、bignumでも正しく動作する。
 * @param out_limbs 結果のlimb配列(新規にヒープ確保したもの)の格納先
 * @param out_len 結果の実効長の格納先
 */
static void mag_isqrt(UINT64 *n, UINT64 nlen, UINT64 **out_limbs, UINT64 *out_len) {
    nlen = mag_len(n, nlen);

    UINT64 *x = (UINT64 *)os_alloc_bytes(8 * nlen);
    for (UINT64 i = 0; i < nlen; i++) {
        x[i] = n[i];
    }
    UINT64 xlen = nlen;

    UINT64 one[1] = {1};
    UINT64 dummy_rem;

    UINT64 *xp1 = (UINT64 *)os_alloc_bytes(8 * (xlen + 1));
    UINT64 xp1_len = mag_add(x, xlen, one, 1, xp1);
    UINT64 ylen = mag_divmod_small(xp1, xp1_len, 2, &dummy_rem);
    UINT64 *y = xp1;

    while (mag_compare(y, ylen, x, xlen) < 0) {
        x = y;
        xlen = ylen;

        UINT64 *quot_buf = (UINT64 *)os_alloc_bytes(8 * nlen);
        UINT64 *rem_buf = (UINT64 *)os_alloc_bytes(8 * nlen);
        UINT64 quot_len, rem_len;
        mag_divmod(n, nlen, x, xlen, quot_buf, &quot_len, rem_buf, &rem_len);

        UINT64 cap = (xlen > quot_len ? xlen : quot_len) + 1;
        UINT64 *sum_buf = (UINT64 *)os_alloc_bytes(8 * cap);
        UINT64 sum_len = mag_add(x, xlen, quot_buf, quot_len, sum_buf);

        ylen = mag_divmod_small(sum_buf, sum_len, 2, &dummy_rem);
        y = sum_buf;
    }

    *out_limbs = x;
    *out_len = xlen;
}



/**
 * 組み込み関数CAR。argsの第一引数のcarを返す。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 第一引数のcar
 */
lisp_val_t primitive_car(lisp_val_t args, lisp_val_t env) {
    lisp_val_t target = cc_car(args); // 第一引数
    return cc_car(target);
}

/**
 * 組み込み関数CDR。argsの第一引数のcdrを返す。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 第一引数のcdr
 */
lisp_val_t primitive_cdr(lisp_val_t args, lisp_val_t env) {
    lisp_val_t target = cc_car(args); // 第一引数
    return cc_cdr(target);
}

/**
 * 組み込み関数+。argsの全整数(FIXNUM/bignum、負数も可)を合計する。
 * 全オペランドが非負FIXNUMかつ桁あふれの恐れがない場合はヒープ確保なしの高速パスを使い、
 * それ以外(負数・bignumが絡む、桁あふれの恐れがある)は符号付きマグニチュードによる
 * 一般パスにフォールバックする。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 合計値の整数(60bit以内ならFIXNUM、それを超えるならbignum)
 */
lisp_val_t primitive_add(lisp_val_t args, lisp_val_t env) {
    int fast = 1;
    UINT64 sum = 0;
    for (lisp_val_t cur = args; cur != nil; cur = cc_cdr(cur)) {
        lisp_val_t v = cc_car(cur);
        if ((v & TAG_MASK) != TAG_FIXNUM || os_fixnum_is_negative(v)) {
            fast = 0;
            break;
        }
        sum += os_fixnum_magnitude(v);
        if (sum > FIXNUM_MAGNITUDE_MASK) {
            fast = 0;
            break;
        }
    }
    if (fast) {
        return os_make_fixnum(sum);
    }

    signed_mag_t acc;
    UINT64 acc_zero[1] = {0};
    acc.sign = 0;
    acc.limbs = acc_zero;
    acc.count = 1;

    for (lisp_val_t cur = args; cur != nil; cur = cc_cdr(cur)) {
        signed_mag_t operand;
        decompose(cc_car(cur), &operand);

        UINT64 cap = (acc.count > operand.count ? acc.count : operand.count) + 1;
        UINT64 *result = (UINT64 *)os_alloc_bytes(8 * cap);
        UINT64 result_len;
        int result_sign;

        if (acc.sign == operand.sign) {
            result_len = mag_add(acc.limbs, acc.count, operand.limbs, operand.count, result);
            result_sign = acc.sign;
        } else if (mag_compare(acc.limbs, acc.count, operand.limbs, operand.count) >= 0) {
            result_len = mag_sub(acc.limbs, acc.count, operand.limbs, operand.count, result);
            result_sign = acc.sign;
        } else {
            result_len = mag_sub(operand.limbs, operand.count, acc.limbs, acc.count, result);
            result_sign = operand.sign;
        }

        acc.sign = result_sign;
        acc.limbs = result;
        acc.count = result_len;
    }

    return os_make_integer(acc.sign, acc.limbs, acc.count);
}

/**
 * 組み込み関数-。argsの第一引数から残りを順に減算する。1引数の場合は単項マイナス(0-x)として
 * 符号を反転する。全オペランドが非負FIXNUMかつ結果が負にならない場合はヒープ確保なしの
 * 高速パスを使い、それ以外は符号付きマグニチュードによる一般パスにフォールバックする。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 減算結果の整数(60bit以内ならFIXNUM、それを超えるならbignum)
 */
lisp_val_t primitive_subtract(lisp_val_t args, lisp_val_t env) {
    lisp_val_t first = cc_car(args);

    if (cc_cdr(args) == nil) {
        // 単項マイナス: 0 - x
        if ((first & TAG_MASK) == TAG_FIXNUM) {
            return os_make_fixnum_signed(!os_fixnum_is_negative(first), os_fixnum_magnitude(first));
        }
        signed_mag_t operand;
        decompose(first, &operand);
        return os_make_integer(!operand.sign, operand.limbs, operand.count);
    }

    int fast = (first & TAG_MASK) == TAG_FIXNUM && !os_fixnum_is_negative(first);
    UINT64 result = fast ? os_fixnum_magnitude(first) : 0;
    if (fast) {
        for (lisp_val_t rest = cc_cdr(args); rest != nil; rest = cc_cdr(rest)) {
            lisp_val_t v = cc_car(rest);
            if ((v & TAG_MASK) != TAG_FIXNUM || os_fixnum_is_negative(v)) {
                fast = 0;
                break;
            }
            UINT64 mag = os_fixnum_magnitude(v);
            if (mag > result) {
                fast = 0;
                break;
            }
            result -= mag;
        }
    }
    if (fast) {
        return os_make_fixnum(result);
    }

    signed_mag_t acc;
    decompose(first, &acc);
    for (lisp_val_t rest = cc_cdr(args); rest != nil; rest = cc_cdr(rest)) {
        signed_mag_t operand;
        decompose(cc_car(rest), &operand);

        UINT64 cap = (acc.count > operand.count ? acc.count : operand.count) + 1;
        UINT64 *result_buf = (UINT64 *)os_alloc_bytes(8 * cap);
        UINT64 result_len;
        int result_sign;

        if (acc.sign != operand.sign) {
            result_len = mag_add(acc.limbs, acc.count, operand.limbs, operand.count, result_buf);
            result_sign = acc.sign;
        } else if (mag_compare(acc.limbs, acc.count, operand.limbs, operand.count) >= 0) {
            result_len = mag_sub(acc.limbs, acc.count, operand.limbs, operand.count, result_buf);
            result_sign = acc.sign;
        } else {
            result_len = mag_sub(operand.limbs, operand.count, acc.limbs, acc.count, result_buf);
            result_sign = !acc.sign;
        }

        acc.sign = result_sign;
        acc.limbs = result_buf;
        acc.count = result_len;
    }

    return os_make_integer(acc.sign, acc.limbs, acc.count);
}

/**
 * 組み込み関数CONS。第一引数をcar、第二引数をcdrとするconsを返す。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 新しく作られたCONS
 */
lisp_val_t primitive_cons(lisp_val_t args, lisp_val_t env) {
    lisp_val_t car = cc_car(args);
    lisp_val_t cdr = cc_car(cc_cdr(args));
    return os_make_cons(car, cdr);
}

/**
 * 組み込み関数EQ。第一引数と第二引数が同一(==)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 同一ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_eq(lisp_val_t args, lisp_val_t env) {
    lisp_val_t a = cc_car(args);
    lisp_val_t b = cc_car(cc_cdr(args));
    return a == b ? g_sym_t : nil;
}

/**
 * 組み込み関数NULL。第一引数がnilかどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return nilならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_null(lisp_val_t args, lisp_val_t env) {
    return cc_car(args) == nil ? g_sym_t : nil;
}

/**
 * 組み込み関数*。argsの全整数(FIXNUM/bignum、負数も可)を乗算する。
 * 全オペランドが非負FIXNUMかつ桁あふれの恐れがない場合はヒープ確保なしの高速パスを使い、
 * それ以外は符号付きマグニチュードによる一般パス(素朴なO(n*m)乗算)にフォールバックする。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 積の整数(60bit以内ならFIXNUM、それを超えるならbignum)
 */
lisp_val_t primitive_multiply(lisp_val_t args, lisp_val_t env) {
    int fast = 1;
    UINT64 product = 1;
    for (lisp_val_t cur = args; cur != nil; cur = cc_cdr(cur)) {
        lisp_val_t v = cc_car(cur);
        if ((v & TAG_MASK) != TAG_FIXNUM || os_fixnum_is_negative(v)) {
            fast = 0;
            break;
        }
        UINT64 mag = os_fixnum_magnitude(v);
        if (mag != 0 && product > FIXNUM_MAGNITUDE_MASK / mag) {
            fast = 0;
            break;
        }
        product *= mag;
    }
    if (fast) {
        return os_make_fixnum(product);
    }

    signed_mag_t acc;
    UINT64 acc_one[1] = {1};
    acc.sign = 0;
    acc.limbs = acc_one;
    acc.count = 1;

    for (lisp_val_t cur = args; cur != nil; cur = cc_cdr(cur)) {
        signed_mag_t operand;
        decompose(cc_car(cur), &operand);

        UINT64 cap = acc.count + operand.count;
        UINT64 *result = (UINT64 *)os_alloc_bytes(8 * cap);
        UINT64 result_len = mag_mul(acc.limbs, acc.count, operand.limbs, operand.count, result);

        acc.sign = (acc.sign != operand.sign);
        acc.limbs = result;
        acc.count = result_len;
    }

    return os_make_integer(acc.sign, acc.limbs, acc.count);
}

/**
 * 組み込み関数/。argsの第一引数から残りを順に除算する(整数除算、商のみ返す)。
 * 全オペランドが非負FIXNUMの場合はヒープ確保なしの高速パスを使い、それ以外(負数・bignumが
 * 絡む)は符号付きマグニチュードによる一般パス(1bitずつのシフト&サブトラクトによる
 * 長除算)にフォールバックする。商の符号は絶対値の商にオペランドの符号のXORを付与して決める。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 除算結果の整数。0除算の場合はg_sym_eval_error
 */
lisp_val_t primitive_divide(lisp_val_t args, lisp_val_t env) {
    lisp_val_t first = cc_car(args);

    int fast = (first & TAG_MASK) == TAG_FIXNUM && !os_fixnum_is_negative(first);
    UINT64 result = fast ? os_fixnum_magnitude(first) : 0;
    if (fast) {
        for (lisp_val_t rest = cc_cdr(args); rest != nil; rest = cc_cdr(rest)) {
            lisp_val_t v = cc_car(rest);
            if ((v & TAG_MASK) != TAG_FIXNUM || os_fixnum_is_negative(v)) {
                fast = 0;
                break;
            }
            UINT64 divisor = os_fixnum_magnitude(v);
            if (divisor == 0) {
                return g_sym_eval_error;
            }
            result /= divisor;
        }
    }
    if (fast) {
        return os_make_fixnum(result);
    }

    signed_mag_t acc;
    decompose(first, &acc);
    for (lisp_val_t rest = cc_cdr(args); rest != nil; rest = cc_cdr(rest)) {
        signed_mag_t operand;
        decompose(cc_car(rest), &operand);

        if (operand.count == 1 && operand.limbs[0] == 0) {
            return g_sym_eval_error;
        }

        UINT64 *quot_buf = (UINT64 *)os_alloc_bytes(8 * acc.count);
        UINT64 *rem_buf = (UINT64 *)os_alloc_bytes(8 * acc.count);
        UINT64 quot_len, rem_len;
        mag_divmod(acc.limbs, acc.count, operand.limbs, operand.count, quot_buf, &quot_len, rem_buf, &rem_len);

        acc.sign = (acc.sign != operand.sign);
        acc.limbs = quot_buf;
        acc.count = quot_len;
    }

    return os_make_integer(acc.sign, acc.limbs, acc.count);
}

/**
 * 組み込み関数<。argsが単調増加(a<b<c<...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調増加ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_less_than(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (number_compare(cc_car(rest), cc_car(cc_cdr(rest))) >= 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数>。argsが単調減少(a>b>c>...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調減少ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_greater_than(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (number_compare(cc_car(rest), cc_car(cc_cdr(rest))) <= 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数=。argsがすべて等しいかどうかを判定する。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return すべて等しいならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_num_equal(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (number_compare(cc_car(rest), cc_car(cc_cdr(rest))) != 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数/=。argsの隣接する要素同士がすべて等しくないかどうかを判定する
 * (厳密な仕様の「全要素が相異なる」ではなく、既存の</=/>と同様に隣接ペア判定に
 * 簡略化している点に注意)。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 隣接ペアがすべて等しくないならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_num_not_equal(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (number_compare(cc_car(rest), cc_car(cc_cdr(rest))) == 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数>=。argsが単調非増加(a>=b>=c>=...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調非増加ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_greater_equal(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (number_compare(cc_car(rest), cc_car(cc_cdr(rest))) < 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数<=。argsが単調非減少(a<=b<=c<=...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調非減少ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_less_equal(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (number_compare(cc_car(rest), cc_car(cc_cdr(rest))) > 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数MAX。argsのうち最大の要素を返す(number_compareのみ、ヒープ確保なし)。
 * @param args 評価済みの引数リスト(すべて整数、1個以上)
 * @param env 呼び出し時の環境(未使用)
 * @return 最大の要素
 */
lisp_val_t primitive_max(lisp_val_t args, lisp_val_t env) {
    lisp_val_t best = cc_car(args);
    for (lisp_val_t rest = cc_cdr(args); rest != nil; rest = cc_cdr(rest)) {
        lisp_val_t v = cc_car(rest);
        if (number_compare(v, best) > 0) {
            best = v;
        }
    }
    return best;
}

/**
 * 組み込み関数MIN。argsのうち最小の要素を返す(number_compareのみ、ヒープ確保なし)。
 * @param args 評価済みの引数リスト(すべて整数、1個以上)
 * @param env 呼び出し時の環境(未使用)
 * @return 最小の要素
 */
lisp_val_t primitive_min(lisp_val_t args, lisp_val_t env) {
    lisp_val_t best = cc_car(args);
    for (lisp_val_t rest = cc_cdr(args); rest != nil; rest = cc_cdr(rest)) {
        lisp_val_t v = cc_car(rest);
        if (number_compare(v, best) < 0) {
            best = v;
        }
    }
    return best;
}

/**
 * 組み込み関数ABS。第一引数の絶対値を返す。すでに非負の場合はヒープ確保なしで
 * そのまま返す。
 * @param args 評価済みの引数リスト(整数1個)
 * @param env 呼び出し時の環境(未使用)
 * @return 絶対値の整数
 */
lisp_val_t primitive_abs(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) == TAG_FIXNUM) {
        if (!os_fixnum_is_negative(val)) {
            return val;
        }
        return os_make_fixnum(os_fixnum_magnitude(val));
    }

    signed_mag_t m;
    decompose(val, &m);
    if (!m.sign) {
        return val;
    }
    return os_make_integer(0, m.limbs, m.count);
}

/**
 * 組み込み関数DIV。z1をz2で除した「floor除算」の商を返す(切り捨て除算の/とは異なり、
 * 商は-∞方向へ切り捨てる)。floor_divmodを参照。
 * @param args 評価済みの引数リスト(整数2個)
 * @param env 呼び出し時の環境(未使用)
 * @return floor除算の商。z2が0の場合はg_sym_eval_error
 */
lisp_val_t primitive_div(lisp_val_t args, lisp_val_t env) {
    lisp_val_t z1 = cc_car(args);
    lisp_val_t z2 = cc_car(cc_cdr(args));

    lisp_val_t div_out, mod_out;
    int div_by_zero;
    floor_divmod(z1, z2, &div_out, &mod_out, &div_by_zero);
    if (div_by_zero) {
        return g_sym_eval_error;
    }
    return div_out;
}

/**
 * 組み込み関数MOD。z1をz2で除した「floor除算」の余りを返す(余りの符号は常にz2の符号に
 * 一致する)。floor_divmodを参照。
 * @param args 評価済みの引数リスト(整数2個)
 * @param env 呼び出し時の環境(未使用)
 * @return floor除算の余り。z2が0の場合はg_sym_eval_error
 */
lisp_val_t primitive_mod(lisp_val_t args, lisp_val_t env) {
    lisp_val_t z1 = cc_car(args);
    lisp_val_t z2 = cc_car(cc_cdr(args));

    lisp_val_t div_out, mod_out;
    int div_by_zero;
    floor_divmod(z1, z2, &div_out, &mod_out, &div_by_zero);
    if (div_by_zero) {
        return g_sym_eval_error;
    }
    return mod_out;
}

/**
 * 組み込み関数GCD。z1とz2の最大公約数を返す(結果は常に非負、mag_gcdを参照)。
 * @param args 評価済みの引数リスト(整数2個)
 * @param env 呼び出し時の環境(未使用)
 * @return 最大公約数(非負整数)
 */
lisp_val_t primitive_gcd(lisp_val_t args, lisp_val_t env) {
    signed_mag_t m1, m2;
    decompose(cc_car(args), &m1);
    decompose(cc_car(cc_cdr(args)), &m2);

    UINT64 *gcd_limbs;
    UINT64 gcd_len;
    mag_gcd(m1.limbs, m1.count, m2.limbs, m2.count, &gcd_limbs, &gcd_len);

    return os_make_integer(0, gcd_limbs, gcd_len);
}

/**
 * 組み込み関数LCM。z1とz2の最小公倍数を返す(結果は常に非負。gcd*lcm=|z1*z2|の関係を
 * 使い、|z1*z2|/gcd(z1,z2)として求める。gcdが0(z1=z2=0の場合のみ)ならlcmも0)。
 * @param args 評価済みの引数リスト(整数2個)
 * @param env 呼び出し時の環境(未使用)
 * @return 最小公倍数(非負整数)
 */
lisp_val_t primitive_lcm(lisp_val_t args, lisp_val_t env) {
    signed_mag_t m1, m2;
    decompose(cc_car(args), &m1);
    decompose(cc_car(cc_cdr(args)), &m2);

    UINT64 *gcd_limbs;
    UINT64 gcd_len;
    mag_gcd(m1.limbs, m1.count, m2.limbs, m2.count, &gcd_limbs, &gcd_len);

    if (gcd_len == 1 && gcd_limbs[0] == 0) {
        return os_make_fixnum(0);
    }

    UINT64 *prod_buf = (UINT64 *)os_alloc_bytes(8 * (m1.count + m2.count));
    UINT64 prod_len = mag_mul(m1.limbs, m1.count, m2.limbs, m2.count, prod_buf);

    UINT64 *quot_buf = (UINT64 *)os_alloc_bytes(8 * prod_len);
    UINT64 *rem_buf = (UINT64 *)os_alloc_bytes(8 * prod_len);
    UINT64 quot_len, rem_len;
    mag_divmod(prod_buf, prod_len, gcd_limbs, gcd_len, quot_buf, &quot_len, rem_buf, &rem_len);

    return os_make_integer(0, quot_buf, quot_len);
}

/**
 * 組み込み関数ISQRT。第一引数の整数平方根floor(sqrt(z))を返す(ニュートン法、
 * mag_isqrtを参照)。zが負の場合は定義域エラー。
 * @param args 評価済みの引数リスト(非負整数1個)
 * @param env 呼び出し時の環境(未使用)
 * @return floor(sqrt(z))。zが負の場合はg_sym_eval_error
 */
lisp_val_t primitive_isqrt(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    signed_mag_t m;
    decompose(val, &m);
    if (m.sign) {
        return g_sym_eval_error;
    }

    UINT64 *out_limbs;
    UINT64 out_len;
    mag_isqrt(m.limbs, m.count, &out_limbs, &out_len);

    return os_make_integer(0, out_limbs, out_len);
}

/**
 * 組み込み関数NUMBERP。第一引数が数値(FIXNUMまたはbignum)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 数値ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_numberp(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) == TAG_FIXNUM) {
        return g_sym_t;
    }
    if ((val & TAG_MASK) == TAG_INSTANCE && ((UINT64 *)(val & ~TAG_MASK))[0] == MAGIC_BIGNUM) {
        return g_sym_t;
    }
    return nil;
}

/**
 * 組み込み関数FIXNUMP。第一引数がFIXNUMかどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return FIXNUMならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_fixnump(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    return (val & TAG_MASK) == TAG_FIXNUM ? g_sym_t : nil;
}

/**
 * 組み込み関数BIGNUMP。第一引数が60bitを超える整数(bignum、MAGIC_BIGNUMのINSTANCE)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return bignumならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_bignump(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) == TAG_INSTANCE && ((UINT64 *)(val & ~TAG_MASK))[0] == MAGIC_BIGNUM) {
        return g_sym_t;
    }
    return nil;
}

/**
 * 組み込み関数SYMBOLP。第一引数がsymbolかどうかを判定する。
 * nilはTAG_CONS(自己参照cons)で表現されているが、ISLisp上はsymbolとして扱われるため
 * val == nil も真と判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return symbolならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_symbolp(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if (val == nil) {
        return g_sym_t;
    }
    return (val & TAG_MASK) == TAG_SYMBOL ? g_sym_t : nil;
}

/**
 * 組み込み関数CONSP。第一引数がconsかどうかを判定する。
 * nilは内部表現上TAG_CONSだが、ISLisp上はconsではないため val == nil は偽と判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return consならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_consp(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if (val == nil) {
        return nil;
    }
    return (val & TAG_MASK) == TAG_CONS ? g_sym_t : nil;
}

/**
 * 組み込み関数EQL。第一引数と第二引数が同一かどうかを判定する。
 * 仕様上eqとの違いは数値・文字の値比較だが、本実装のfixnum/charは即値表現のため
 * eqのポインタ比較のままで正しく判定できる。ただしbignumは同じ値でも異なるヒープ
 * オブジェクトになりうるため、両者がMAGIC_BIGNUMの場合はsign+limb内容を比較する
 * (floatは未実装のため、それ以外はeqと同じ判定になる)。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 同一ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_eql(lisp_val_t args, lisp_val_t env) {
    lisp_val_t a = cc_car(args);
    lisp_val_t b = cc_car(cc_cdr(args));
    if (a == b) {
        return g_sym_t;
    }
    if ((a & TAG_MASK) == TAG_INSTANCE && (b & TAG_MASK) == TAG_INSTANCE) {
        UINT64 *obj_a = (UINT64 *)(a & ~TAG_MASK);
        UINT64 *obj_b = (UINT64 *)(b & ~TAG_MASK);
        if (obj_a[0] == MAGIC_BIGNUM && obj_b[0] == MAGIC_BIGNUM && bignum_equal(obj_a, obj_b)) {
            return g_sym_t;
        }
    }
    return nil;
}

// VECTOR(TAG_INSTANCE+MAGIC_VECTOR)からブロック先頭へのポインタを取り出す/valがVECTORか
// どうかを判定する。定義はprimitive_make_array直前だが、values_equalおよび
// basic-array-p等の各predicateから先に使うため前方宣言する
static lisp_val_t *vector_header(lisp_val_t vec);
static int is_vector(lisp_val_t val);

/**
 * a と b が構造的に同値かどうかを判定する(primitive_equalの再帰ヘルパー)。
 * CONS/STRING/VECTORは内容を再帰的に比較し、それ以外(FIXNUM/SYMBOL/CHAR/INSTANCE等)は
 * タグ一致のうえでa==bを要求する(実質eqと同じ)。
 * @param a 比較対象1
 * @param b 比較対象2
 * @return 同値なら非0
 */
static int values_equal(lisp_val_t a, lisp_val_t b) {
    if (a == b) {
        return 1;
    }

    UINT64 tag_a = a & TAG_MASK;
    UINT64 tag_b = b & TAG_MASK;
    if (tag_a != tag_b) {
        return 0;
    }

    switch (tag_a) {
        case TAG_CONS:
            return values_equal(cc_car(a), cc_car(b)) && values_equal(cc_cdr(a), cc_cdr(b));

        case TAG_STRING: {
            lisp_addr_t addr_a = a & ~TAG_MASK;
            lisp_addr_t addr_b = b & ~TAG_MASK;
            UINT64 len_a = ((lisp_val_t *)addr_a)[0];
            UINT64 len_b = ((lisp_val_t *)addr_b)[0];
            if (len_a != len_b) {
                return 0;
            }
            UINT8 *bytes_a = (UINT8 *)(addr_a + 8);
            UINT8 *bytes_b = (UINT8 *)(addr_b + 8);
            for (UINT64 i = 0; i < len_a; i++) {
                if (bytes_a[i] != bytes_b[i]) {
                    return 0;
                }
            }
            return 1;
        }

        case TAG_INSTANCE: {
            UINT64 *obj_a = (UINT64 *)(a & ~TAG_MASK);
            UINT64 *obj_b = (UINT64 *)(b & ~TAG_MASK);
            if (obj_a[0] == MAGIC_BIGNUM && obj_b[0] == MAGIC_BIGNUM) {
                return bignum_equal(obj_a, obj_b);
            }
            if (obj_a[0] == MAGIC_VECTOR && obj_b[0] == MAGIC_VECTOR) {
                lisp_val_t *header_a = vector_header(a);
                lisp_val_t *header_b = vector_header(b);
                UINT64 rank_a = header_a[0];
                UINT64 rank_b = header_b[0];
                if (rank_a != rank_b) {
                    return 0;
                }
                UINT64 total = 1;
                for (UINT64 i = 0; i < rank_a; i++) {
                    if (header_a[1 + i] != header_b[1 + i]) {
                        return 0;
                    }
                    total *= header_a[1 + i];
                }
                lisp_val_t *data_a = (lisp_val_t *)((lisp_addr_t)header_a + 8 * (1 + rank_a));
                lisp_val_t *data_b = (lisp_val_t *)((lisp_addr_t)header_b + 8 * (1 + rank_b));
                for (UINT64 i = 0; i < total; i++) {
                    if (!values_equal(data_a[i], data_b[i])) {
                        return 0;
                    }
                }
                return 1;
            }
            return 0;
        }

        default:
            return 0;
    }
}

/**
 * 組み込み関数EQUAL。第一引数と第二引数の構造的な同値性を判定する。
 * CONS/STRING/VECTORは再帰的に内容を比較し、両者がbignum(MAGIC_BIGNUM)ならsign+limb内容を
 * 比較し、それ以外はeqと同じ判定にフォールバックする。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 構造的に同値ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_equal(lisp_val_t args, lisp_val_t env) {
    lisp_val_t a = cc_car(args);
    lisp_val_t b = cc_car(cc_cdr(args));
    return values_equal(a, b) ? g_sym_t : nil;
}

/**
 * 組み込み関数LISTP。第一引数がlist(nilまたはcons。ドットリストも含む)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return listならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_listp(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if (val == nil) {
        return g_sym_t;
    }
    return (val & TAG_MASK) == TAG_CONS ? g_sym_t : nil;
}

/**
 * 組み込み関数CHARACTERP。第一引数がcharacterかどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return characterならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_characterp(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    return (val & TAG_MASK) == TAG_CHAR ? g_sym_t : nil;
}

/**
 * 2つのCHARの大小を比較する(即値の上位ビットを(UINT8)にキャストしてASCIIコード同士を
 * 比較する。0-9/A-Z/a-zの範囲では仕様(§20)の順序と一致する)。
 * @return a<bなら負、a==bなら0、a>bなら正
 */
static int char_compare(lisp_val_t a, lisp_val_t b) {
    int code_a = (int)(UINT8)(a >> 3);
    int code_b = (int)(UINT8)(b >> 3);
    return code_a - code_b;
}

/**
 * 組み込み関数CHAR=。argsがすべて同じ文字かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてCHAR)
 * @param env 呼び出し時の環境(未使用)
 * @return すべて等しいならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_char_equal(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (char_compare(cc_car(rest), cc_car(cc_cdr(rest))) != 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数CHAR/=。argsの隣接する要素同士がすべて等しくないかどうかを判定する
 * (厳密な仕様の「全要素が相異なる」ではなく、既存の数値比較の/=と同様に隣接ペア判定に
 * 簡略化している点に注意)。
 * @param args 評価済みの引数リスト(すべてCHAR)
 * @param env 呼び出し時の環境(未使用)
 * @return 隣接ペアがすべて等しくないならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_char_not_equal(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (char_compare(cc_car(rest), cc_car(cc_cdr(rest))) == 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数CHAR<。argsが単調増加(a<b<c<...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてCHAR)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調増加ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_char_less_than(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (char_compare(cc_car(rest), cc_car(cc_cdr(rest))) >= 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数CHAR>。argsが単調減少(a>b>c>...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてCHAR)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調減少ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_char_greater_than(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (char_compare(cc_car(rest), cc_car(cc_cdr(rest))) <= 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数CHAR<=。argsが単調非減少(a<=b<=c<=...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてCHAR)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調非減少ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_char_less_equal(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (char_compare(cc_car(rest), cc_car(cc_cdr(rest))) > 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数CHAR>=。argsが単調非増加(a>=b>=c>=...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてCHAR)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調非増加ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_char_greater_equal(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (char_compare(cc_car(rest), cc_car(cc_cdr(rest))) < 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 2つのSTRINGを辞書式に比較する(最初に異なるバイト位置での差、全バイト一致なら
 * 長さの差を返す。これにより「短い方が長い方の真の接頭辞なら短い方が小さい」という
 * 仕様(§24)のルールも自然に満たされる)。
 * @return a<bなら負、a==bなら0、a>bなら正
 */
static int string_compare(lisp_val_t a, lisp_val_t b) {
    lisp_addr_t addr_a = a & ~TAG_MASK;
    lisp_addr_t addr_b = b & ~TAG_MASK;
    UINT64 len_a = ((lisp_val_t *)addr_a)[0];
    UINT64 len_b = ((lisp_val_t *)addr_b)[0];
    UINT8 *bytes_a = (UINT8 *)(addr_a + 8);
    UINT8 *bytes_b = (UINT8 *)(addr_b + 8);
    UINT64 min_len = len_a < len_b ? len_a : len_b;
    for (UINT64 i = 0; i < min_len; i++) {
        if (bytes_a[i] != bytes_b[i]) {
            return (int)bytes_a[i] - (int)bytes_b[i];
        }
    }
    return (int)len_a - (int)len_b;
}

/**
 * 組み込み関数STRING=。argsがすべて同じ文字列かどうかを判定する。
 * CHAR=同様、仕様上は2引数だが本実装では隣接ペア連鎖のN項関数として実装する。
 * @param args 評価済みの引数リスト(すべてSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return すべて等しいならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_string_equal(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (string_compare(cc_car(rest), cc_car(cc_cdr(rest))) != 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数STRING/=。argsの隣接する要素同士がすべて等しくないかどうかを判定する
 * (CHAR/=と同様、隣接ペア判定に簡略化している)。
 * @param args 評価済みの引数リスト(すべてSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return 隣接ペアがすべて等しくないならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_string_not_equal(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (string_compare(cc_car(rest), cc_car(cc_cdr(rest))) == 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数STRING<。argsが単調増加(a<b<c<...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調増加ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_string_less_than(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (string_compare(cc_car(rest), cc_car(cc_cdr(rest))) >= 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数STRING>。argsが単調減少(a>b>c>...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調減少ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_string_greater_than(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (string_compare(cc_car(rest), cc_car(cc_cdr(rest))) <= 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数STRING<=。argsが単調非減少(a<=b<=c<=...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調非減少ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_string_less_equal(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (string_compare(cc_car(rest), cc_car(cc_cdr(rest))) > 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数STRING>=。argsが単調非増加(a>=b>=c>=...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調非増加ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_string_greater_equal(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        if (string_compare(cc_car(rest), cc_car(cc_cdr(rest))) < 0) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数CHAR-INDEX。第二引数のSTRING中で第一引数のCHARが最初に現れる位置を
 * 第三引数(省略可、FIXNUM、省略時0)から探して返す。見つからなければnil。
 * @param args 評価済みの引数リスト(CHAR, STRING, [FIXNUM])
 * @param env 呼び出し時の環境(未使用)
 * @return 見つかった位置(FIXNUM)、見つからなければnil
 */
lisp_val_t primitive_char_index(lisp_val_t args, lisp_val_t env) {
    lisp_val_t ch = cc_car(args);
    lisp_val_t str = cc_car(cc_cdr(args));
    lisp_val_t rest = cc_cdr(cc_cdr(args));
    UINT64 start = (rest != nil) ? (UINT64)(cc_car(rest) >> 3) : 0;

    lisp_addr_t addr = str & ~TAG_MASK;
    UINT64 len = ((lisp_val_t *)addr)[0];
    UINT8 *bytes = (UINT8 *)(addr + 8);
    UINT8 target = (UINT8)(ch >> 3);
    for (UINT64 i = start; i < len; i++) {
        if (bytes[i] == target) {
            return os_make_fixnum(i);
        }
    }
    return nil;
}

/**
 * 組み込み関数STRING-INDEX。第二引数のSTRING中で第一引数のSTRING(部分文字列)が
 * 最初に現れる位置を第三引数(省略可、FIXNUM、省略時0)から探して返す。
 * 見つからなければnil。空文字列は探索開始位置に即マッチする。
 * @param args 評価済みの引数リスト(STRING(部分文字列), STRING, [FIXNUM])
 * @param env 呼び出し時の環境(未使用)
 * @return 見つかった位置(FIXNUM)、見つからなければnil
 */
lisp_val_t primitive_string_index(lisp_val_t args, lisp_val_t env) {
    lisp_val_t sub = cc_car(args);
    lisp_val_t str = cc_car(cc_cdr(args));
    lisp_val_t rest = cc_cdr(cc_cdr(args));
    UINT64 start = (rest != nil) ? (UINT64)(cc_car(rest) >> 3) : 0;

    lisp_addr_t sub_addr = sub & ~TAG_MASK;
    lisp_addr_t str_addr = str & ~TAG_MASK;
    UINT64 sub_len = ((lisp_val_t *)sub_addr)[0];
    UINT64 str_len = ((lisp_val_t *)str_addr)[0];
    UINT8 *sub_bytes = (UINT8 *)(sub_addr + 8);
    UINT8 *str_bytes = (UINT8 *)(str_addr + 8);

    if (start > str_len || sub_len > str_len - start) {
        return nil;
    }
    for (UINT64 i = start; i + sub_len <= str_len; i++) {
        UINT64 j = 0;
        for (; j < sub_len; j++) {
            if (str_bytes[i + j] != sub_bytes[j]) {
                break;
            }
        }
        if (j == sub_len) {
            return os_make_fixnum(i);
        }
    }
    return nil;
}

/**
 * 組み込み関数STRING-APPEND。argsの各STRINGを連結した新しいSTRINGを返す
 * (引数が無ければ空文字列)。
 * @param args 評価済みの引数リスト(すべてSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return 連結結果のSTRING
 */
lisp_val_t primitive_string_append(lisp_val_t args, lisp_val_t env) {
    UINT64 total_len = 0;
    for (lisp_val_t cur = args; cur != nil; cur = cc_cdr(cur)) {
        lisp_addr_t addr = cc_car(cur) & ~TAG_MASK;
        total_len += ((lisp_val_t *)addr)[0];
    }

    lisp_addr_t out_addr = os_alloc_bytes(8 + total_len);
    ((lisp_val_t *)out_addr)[0] = total_len;
    UINT8 *out_bytes = (UINT8 *)(out_addr + 8);
    UINT64 offset = 0;
    for (lisp_val_t cur = args; cur != nil; cur = cc_cdr(cur)) {
        lisp_addr_t addr = cc_car(cur) & ~TAG_MASK;
        UINT64 len = ((lisp_val_t *)addr)[0];
        UINT8 *bytes = (UINT8 *)(addr + 8);
        for (UINT64 i = 0; i < len; i++) {
            out_bytes[offset + i] = bytes[i];
        }
        offset += len;
    }
    return (lisp_val_t)(out_addr | TAG_STRING);
}

/**
 * 組み込み関数STRINGP。第一引数がstringかどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return stringならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_stringp(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    return (val & TAG_MASK) == TAG_STRING ? g_sym_t : nil;
}

/**
 * 組み込み関数FUNCTIONP。第一引数が関数(MAGIC_FUNCTION_NATIVEまたはMAGIC_FUNCTION_INTERPRETED)
 * かどうかを判定する。MAGIC_MACROは関数ではないため偽と判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 関数ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_functionp(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) != TAG_INSTANCE) {
        return nil;
    }
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    return (obj[0] == MAGIC_FUNCTION_NATIVE || obj[0] == MAGIC_FUNCTION_INTERPRETED) ? g_sym_t : nil;
}

/**
 * 組み込み関数GENERIC-FUNCTION-P。defgeneric(src/lisp/init.lisp)はdispatch用の
 * 通常のinterpreted functionを生成するだけで、generic functionであることを示す
 * 専用のタグ付きオブジェクトを作らないため、区別する手段が無く常にnilを返す。
 * @param args 評価済みの引数リスト(未使用)
 * @param env 呼び出し時の環境(未使用)
 * @return 常にnil
 */
lisp_val_t primitive_generic_function_p(lisp_val_t args, lisp_val_t env) {
    return nil;
}

/**
 * 組み込み関数BASIC-ARRAY-P。第一引数がbasic-array(MAGIC_VECTORまたはTAG_STRING)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return basic-arrayならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_basic_array_p(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    UINT64 tag = val & TAG_MASK;
    return (is_vector(val) || tag == TAG_STRING) ? g_sym_t : nil;
}

/**
 * 組み込み関数BASIC-ARRAY*-P / GENERAL-ARRAY*-P。第一引数がrank!=1のMAGIC_VECTORかどうかを
 * 判定する。本実装では特殊化した配列型の区別が無く両クラスの外延が一致するため、
 * 同じ実体を両方のシンボルに登録して共用する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return rank!=1のVECTORならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_array_star_p(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if (!is_vector(val)) {
        return nil;
    }
    UINT64 rank = vector_header(val)[0];
    return rank != 1 ? g_sym_t : nil;
}

/**
 * 組み込み関数BASIC-VECTOR-P。第一引数がbasic-vector(rank==1のMAGIC_VECTORまたはTAG_STRING)
 * かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return basic-vectorならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_basic_vector_p(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) == TAG_STRING) {
        return g_sym_t;
    }
    if (!is_vector(val)) {
        return nil;
    }
    UINT64 rank = vector_header(val)[0];
    return rank == 1 ? g_sym_t : nil;
}

/**
 * 組み込み関数GENERAL-VECTOR-P。第一引数がgeneral-vector(rank==1のMAGIC_VECTOR)かどうかを
 * 判定する。STRINGはbasic-vectorだがgeneral-vectorではないため除外する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return general-vectorならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_general_vector_p(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if (!is_vector(val)) {
        return nil;
    }
    UINT64 rank = vector_header(val)[0];
    return rank == 1 ? g_sym_t : nil;
}

/**
 * 組み込み関数STREAMP。第一引数がstream(MAGIC_STREAM)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return streamならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_streamp(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) != TAG_INSTANCE) {
        return nil;
    }
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    return obj[0] == MAGIC_STREAM ? g_sym_t : nil;
}

/**
 * 組み込み関数SYMBOL-NAME。第一引数のsymbolの名前をSTRINGとして返す。
 * @param args 評価済みの引数リスト(第一引数はSYMBOL)
 * @param env 呼び出し時の環境(未使用)
 * @return symbol名のSTRING
 */
lisp_val_t primitive_symbol_name(lisp_val_t args, lisp_val_t env) {
    lisp_val_t sym = cc_car(args);
    lisp_addr_t addr = sym & ~TAG_MASK;
    return ((lisp_val_t *)addr)[0];
}

/**
 * 組み込み関数STRING-TO-SYMBOL。第一引数のSTRINGをsymbol名としてintern(既存の大文字化ルール)する。
 * @param args 評価済みの引数リスト(第一引数はSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return internされたSYMBOL
 */
lisp_val_t primitive_string_to_symbol(lisp_val_t args, lisp_val_t env) {
    lisp_val_t str = cc_car(args);
    char buf[256];
    os_string_to_cstr(str, buf, sizeof(buf));
    return os_make_symbol(buf);
}

/** gensymが生成する名前の連番カウンタ */
static UINT64 g_gensym_counter = 0;

/**
 * 組み込み関数GENSYM。呼ぶたびに"G"+連番の名前で新しいsymbolをintern して返す
 * (真の非intern symbolは未サポート。連番が一巡しない限り重複は起きない)。
 * @param args 未使用
 * @param env 呼び出し時の環境(未使用)
 * @return 新しくinternされたSYMBOL
 */
lisp_val_t primitive_gensym(lisp_val_t args, lisp_val_t env) {
    UINT64 n = g_gensym_counter++;
    char buf[24];
    buf[0] = 'G';
    int len = 1;
    if (n == 0) {
        buf[len++] = '0';
    } else {
        char digits[20];
        int dlen = 0;
        while (n > 0) {
            digits[dlen++] = '0' + (char)(n % 10);
            n /= 10;
        }
        while (dlen > 0) {
            buf[len++] = digits[--dlen];
        }
    }
    buf[len] = '\0';
    return os_make_symbol(buf);
}

/** dimensions引数として一度に受け付けられる最大次元数(rank)。reader.cのREADER_TOKEN_MAX等と同種の固定長バッファ上限 */
#define MAX_ARRAY_RANK 8

/**
 * rank/dimsから配列本体の可変長ブロック(word0=rank、word[1..rank]=各次元のサイズ、
 * word[rank+1..]=データ)をヒープに確保し、ヘッダ(rank, dims)だけ書き込む。
 * データ部の初期化は呼び出し側の責務(make-array/create-vector/vectorで初期化ポリシーが
 * 異なるため)。
 * @param rank 次元数
 * @param dims 各次元のサイズ(rank個)
 * @return 確保したブロックの先頭アドレス(タグなし)
 */
static lisp_addr_t alloc_vector_block(UINT64 rank, const UINT64 *dims) {
    UINT64 total = 1;
    for (UINT64 i = 0; i < rank; i++) {
        total *= dims[i];
    }
    lisp_addr_t addr = os_alloc_bytes(8 * (1 + rank + total));
    lisp_val_t *header = (lisp_val_t *)addr;
    header[0] = rank;
    for (UINT64 i = 0; i < rank; i++) {
        header[1 + i] = dims[i];
    }
    return addr;
}

/**
 * VECTOR(TAG_INSTANCE+MAGIC_VECTOR)から、内部の可変長ブロック(rank+dims+data)の
 * 先頭へのポインタを取り出す。
 * @param vec VECTOR
 * @return ブロック先頭へのポインタ
 */
static lisp_val_t *vector_header(lisp_val_t vec) {
    UINT64 *obj = (UINT64 *)(vec & ~TAG_MASK);
    return (lisp_val_t *)obj[1];
}

/**
 * valがVECTOR(TAG_INSTANCE+MAGIC_VECTOR)かどうかを判定する。
 * @param val 判定対象の値
 * @return VECTORなら非0、そうでなければ0
 */
static int is_vector(lisp_val_t val) {
    return (val & TAG_MASK) == TAG_INSTANCE
        && ((UINT64 *)(val & ~TAG_MASK))[0] == MAGIC_VECTOR;
}

lisp_val_t *os_vector_header(lisp_val_t vec) {
    return vector_header(vec);
}

lisp_val_t os_make_vector_from_list(lisp_val_t list) {
    UINT64 count = 0;
    for (lisp_val_t cur = list; cur != nil; cur = cc_cdr(cur)) {
        count++;
    }
    lisp_addr_t addr = alloc_vector_block(1, &count);
    lisp_val_t *data = (lisp_val_t *)(addr + 16);
    UINT64 i = 0;
    for (lisp_val_t cur = list; cur != nil; cur = cc_cdr(cur)) {
        data[i++] = cc_car(cur);
    }
    return os_make_instance(MAGIC_VECTOR, (UINT64)addr, 0, 0);
}

/**
 * 組み込み関数VECTOR。評価済みの引数列をそのまま要素とするrank1のgeneral-vectorを返す。
 * @param args 評価済みの引数リスト(すべて要素として使う)
 * @param env 呼び出し時の環境(未使用)
 * @return 構築したVECTOR
 */
lisp_val_t primitive_vector(lisp_val_t args, lisp_val_t env) {
    (void)env;
    return os_make_vector_from_list(args);
}

/**
 * 組み込み関数CREATE-VECTOR。第一引数の長さ(FIXNUM)のgeneral-vectorを確保する。
 * 第二引数(省略可)を指定すると全要素をその値で初期化する(省略時はnil)。
 * @param args 評価済みの引数リスト(第一引数はFIXNUM、第二引数は省略可)
 * @param env 呼び出し時の環境(未使用)
 * @return 確保したVECTOR
 */
lisp_val_t primitive_create_vector(lisp_val_t args, lisp_val_t env) {
    (void)env;
    UINT64 count = cc_car(args) >> 3;
    lisp_val_t rest = cc_cdr(args);
    lisp_val_t init = (rest != nil) ? cc_car(rest) : nil;

    lisp_addr_t addr = alloc_vector_block(1, &count);
    lisp_val_t *data = (lisp_val_t *)(addr + 16);
    for (UINT64 i = 0; i < count; i++) {
        data[i] = init;
    }
    return os_make_instance(MAGIC_VECTOR, (UINT64)addr, 0, 0);
}

/**
 * 組み込み関数MAKE-ARRAY。第一引数の次元(FIXNUM、またはFIXNUMのリスト)を持つ
 * 多次元配列を確保する。要素はすべてnilで初期化される。
 * @param args 評価済みの引数リスト(第一引数はFIXNUMまたはFIXNUMのリスト)
 * @param env 呼び出し時の環境(未使用)
 * @return 確保したVECTOR
 */
lisp_val_t primitive_make_array(lisp_val_t args, lisp_val_t env) {
    lisp_val_t dims_arg = cc_car(args);

    UINT64 dims[MAX_ARRAY_RANK];
    UINT64 rank = 0;

    if ((dims_arg & TAG_MASK) == TAG_FIXNUM) {
        dims[rank++] = dims_arg >> 3;
    } else {
        for (lisp_val_t cur = dims_arg; cur != nil && rank < MAX_ARRAY_RANK; cur = cc_cdr(cur)) {
            dims[rank++] = cc_car(cur) >> 3;
        }
    }

    UINT64 total = 1;
    for (UINT64 i = 0; i < rank; i++) {
        total *= dims[i];
    }

    lisp_addr_t addr = alloc_vector_block(rank, dims);
    lisp_val_t *data = (lisp_val_t *)(addr + 8 * (1 + rank));
    for (UINT64 i = 0; i < total; i++) {
        data[i] = nil;
    }

    return os_make_instance(MAGIC_VECTOR, (UINT64)addr, 0, 0);
}

/**
 * argsのcur以降を配列の各次元の添字として順に辿り、行優先(row-major)オフセットを計算する。
 * 添字が対応する次元のサイズ以上の場合はg_sym_eval_errorを示すため*out_of_boundsを立てる。
 * @param header 配列のヒープ先頭(word0=rank, word[1..rank]=各次元のサイズ)
 * @param rank 配列の次元数
 * @param cur 添字群の先頭cons
 * @param out_of_bounds 範囲外の添字があった場合に非0を書き込む
 * @return 計算したオフセット
 */
static UINT64 array_offset(lisp_val_t *header, UINT64 rank, lisp_val_t cur, int *out_of_bounds) {
    UINT64 offset = 0;
    *out_of_bounds = 0;
    for (UINT64 i = 0; i < rank; i++) {
        UINT64 idx = cc_car(cur) >> 3;
        UINT64 dim = header[1 + i];
        if (idx >= dim) {
            *out_of_bounds = 1;
        }
        offset = offset * dim + idx;
        cur = cc_cdr(cur);
    }
    return offset;
}

/**
 * 組み込み関数AREF。第一引数の配列から、残りの引数(各次元の添字)が指す要素を返す。
 * @param args 評価済みの引数リスト(第一引数はVECTOR、残りはFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 添字が指す要素。範囲外の添字が指定された場合はg_sym_eval_error
 */
lisp_val_t primitive_aref(lisp_val_t args, lisp_val_t env) {
    lisp_val_t array = cc_car(args);
    lisp_val_t *header = vector_header(array);
    UINT64 rank = header[0];

    int out_of_bounds;
    UINT64 offset = array_offset(header, rank, cc_cdr(args), &out_of_bounds);
    if (out_of_bounds) {
        return g_sym_eval_error;
    }

    lisp_val_t *data = (lisp_val_t *)((lisp_addr_t)header + 8 * (1 + rank));
    return data[offset];
}

/**
 * 組み込み関数ARRAY-DIMENSIONS。第一引数の配列の各次元のサイズをリストで返す。
 * @param args 評価済みの引数リスト(第一引数はVECTOR)
 * @param env 呼び出し時の環境(未使用)
 * @return 各次元のサイズ(FIXNUM)のリスト
 */
lisp_val_t primitive_array_dimensions(lisp_val_t args, lisp_val_t env) {
    lisp_val_t array = cc_car(args);
    lisp_val_t *header = vector_header(array);
    UINT64 rank = header[0];

    lisp_val_t result = nil;
    for (UINT64 i = rank; i > 0; i--) {
        result = os_make_cons(os_make_fixnum(header[i]), result);
    }
    return result;
}

/**
 * 組み込み関数SET-CAR。第一引数のconsのcarを第二引数で破壊的に書き換える。
 * @param args 評価済みの引数リスト(第一引数はCONS)
 * @param env 呼び出し時の環境(未使用)
 * @return 書き込んだ値(第二引数)
 */
lisp_val_t primitive_set_car(lisp_val_t args, lisp_val_t env) {
    lisp_val_t target = cc_car(args);
    lisp_val_t val = cc_car(cc_cdr(args));
    cc_set_car(target, val);
    return val;
}

/**
 * 組み込み関数SET-CDR。第一引数のconsのcdrを第二引数で破壊的に書き換える。
 * @param args 評価済みの引数リスト(第一引数はCONS)
 * @param env 呼び出し時の環境(未使用)
 * @return 書き込んだ値(第二引数)
 */
lisp_val_t primitive_set_cdr(lisp_val_t args, lisp_val_t env) {
    lisp_val_t target = cc_car(args);
    lisp_val_t val = cc_car(cc_cdr(args));
    cc_set_cdr(target, val);
    return val;
}

/**
 * 組み込み関数SET-AREF。第一引数の配列の、続く添字が指す要素を最後の引数で破壊的に書き換える。
 * @param args 評価済みの引数リスト(array idx1 idx2 ... value の並び)
 * @param env 呼び出し時の環境(未使用)
 * @return 書き込んだ値(最後の引数)。範囲外の添字が指定された場合はg_sym_eval_error
 */
lisp_val_t primitive_set_aref(lisp_val_t args, lisp_val_t env) {
    lisp_val_t array = cc_car(args);
    lisp_val_t *header = vector_header(array);
    UINT64 rank = header[0];

    int out_of_bounds;
    lisp_val_t idx_cur = cc_cdr(args);
    UINT64 offset = array_offset(header, rank, idx_cur, &out_of_bounds);
    if (out_of_bounds) {
        return g_sym_eval_error;
    }

    lisp_val_t value_cur = idx_cur;
    for (UINT64 i = 0; i < rank; i++) {
        value_cur = cc_cdr(value_cur);
    }
    lisp_val_t val = cc_car(value_cur);

    lisp_val_t *data = (lisp_val_t *)((lisp_addr_t)header + 8 * (1 + rank));
    data[offset] = val;
    return val;
}

/**
 * 組み込み関数CREATE-STRING。第一引数の長さ(FIXNUM)のSTRINGを確保する。
 * 第二引数(省略可、CHAR)を指定すると全要素をその文字で初期化する(省略時は空白)。
 * @param args 評価済みの引数リスト(第一引数はFIXNUM、第二引数は省略可のCHAR)
 * @param env 呼び出し時の環境(未使用)
 * @return 確保したSTRING
 */
lisp_val_t primitive_create_string(lisp_val_t args, lisp_val_t env) {
    UINT64 len = cc_car(args) >> 3;
    lisp_val_t char_arg = cc_cdr(args);
    UINT8 fill = ' ';
    if (char_arg != nil) {
        fill = (UINT8)(cc_car(char_arg) >> 3);
    }

    lisp_addr_t addr = os_alloc_bytes(8 + len);
    lisp_val_t *header = (lisp_val_t *)addr;
    header[0] = len;
    UINT8 *bytes = (UINT8 *)(addr + 8);
    for (UINT64 i = 0; i < len; i++) {
        bytes[i] = fill;
    }
    return (lisp_val_t)(addr | TAG_STRING);
}

/**
 * 組み込み関数STRING-ELT。第一引数のSTRINGの第二引数(0起算)番目の文字を返す。
 * @param args 評価済みの引数リスト(第一引数はSTRING、第二引数はFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 添字が指す文字(CHAR)。範囲外の添字が指定された場合はg_sym_eval_error
 */
lisp_val_t primitive_string_elt(lisp_val_t args, lisp_val_t env) {
    lisp_val_t str = cc_car(args);
    UINT64 idx = cc_car(cc_cdr(args)) >> 3;

    lisp_addr_t addr = str & ~TAG_MASK;
    UINT64 len = ((lisp_val_t *)addr)[0];
    if (idx >= len) {
        return g_sym_eval_error;
    }
    UINT8 *bytes = (UINT8 *)(addr + 8);
    return os_make_char((char)bytes[idx]);
}

/**
 * 組み込み関数LENGTH。第一引数のシーケンス(LIST/STRING/VECTOR)の要素数を返す。
 * VECTORの場合は次元に関わらず全要素数(各次元のサイズの積)を返す。
 * @param args 評価済みの引数リスト(第一引数はLIST/STRING/VECTOR)
 * @param env 呼び出し時の環境(未使用)
 * @return 要素数(FIXNUM)
 */
lisp_val_t primitive_length(lisp_val_t args, lisp_val_t env) {
    lisp_val_t seq = cc_car(args);
    if (seq == nil) {
        return os_make_fixnum(0);
    }

    switch (seq & TAG_MASK) {
        case TAG_CONS: {
            UINT64 count = 0;
            for (lisp_val_t cur = seq; cur != nil; cur = cc_cdr(cur)) {
                count++;
            }
            return os_make_fixnum(count);
        }
        case TAG_STRING: {
            lisp_addr_t addr = seq & ~TAG_MASK;
            return os_make_fixnum(((lisp_val_t *)addr)[0]);
        }
        case TAG_INSTANCE: {
            if (!is_vector(seq)) {
                return g_sym_eval_error;
            }
            lisp_val_t *header = vector_header(seq);
            UINT64 rank = header[0];
            UINT64 total = 1;
            for (UINT64 i = 0; i < rank; i++) {
                total *= header[1 + i];
            }
            return os_make_fixnum(total);
        }
        default:
            return g_sym_eval_error;
    }
}


/**
 * 組み込み関数%%MAKE-CLASS-RAW。ILOSクラスオブジェクト(MAGIC_CLASS)を確保する。
 * @param args 評価済みの引数リスト(name symbol, supers クラスオブジェクトのlist, slots スロット記述子のlist)
 * @param env 呼び出し時の環境(未使用)
 * @return 確保したクラスオブジェクト
 */
lisp_val_t primitive_make_class_raw(lisp_val_t args, lisp_val_t env) {
    lisp_val_t name = cc_car(args);
    lisp_val_t supers = cc_car(cc_cdr(args));
    lisp_val_t slots = cc_car(cc_cdr(cc_cdr(args)));
    return os_make_instance(MAGIC_CLASS, name, supers, slots);
}

/**
 * 組み込み関数%%CLASS-NAME。ILOSクラスオブジェクトのname(symbol)を返す。
 * @param args 評価済みの引数リスト(第一引数はクラスオブジェクト)
 * @param env 呼び出し時の環境(未使用)
 * @return name(symbol)
 */
lisp_val_t primitive_class_name(lisp_val_t args, lisp_val_t env) {
    UINT64 *obj = (UINT64 *)(cc_car(args) & ~TAG_MASK);
    return obj[1];
}

/**
 * 組み込み関数%%CLASS-SUPERS。ILOSクラスオブジェクトのsuperclasses(クラスオブジェクトのlist)を返す。
 * @param args 評価済みの引数リスト(第一引数はクラスオブジェクト)
 * @param env 呼び出し時の環境(未使用)
 * @return superclasses(クラスオブジェクトのlist)
 */
lisp_val_t primitive_class_supers(lisp_val_t args, lisp_val_t env) {
    UINT64 *obj = (UINT64 *)(cc_car(args) & ~TAG_MASK);
    return obj[2];
}

/**
 * 組み込み関数%%CLASS-SLOTS。ILOSクラスオブジェクトのslots(スロット記述子のlist、継承分含む)を返す。
 * @param args 評価済みの引数リスト(第一引数はクラスオブジェクト)
 * @param env 呼び出し時の環境(未使用)
 * @return slots(スロット記述子のlist)
 */
lisp_val_t primitive_class_slots(lisp_val_t args, lisp_val_t env) {
    UINT64 *obj = (UINT64 *)(cc_car(args) & ~TAG_MASK);
    return obj[3];
}

/**
 * 組み込み関数%%CLASSP。第一引数がILOSクラスオブジェクト(MAGIC_CLASS)かどうかを判定する。
 * @param args 評価済みの引数リスト(第一引数は任意の値)
 * @param env 呼び出し時の環境(未使用)
 * @return クラスオブジェクトならt、それ以外ならnil
 */
lisp_val_t primitive_classp(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) != TAG_INSTANCE) {
        return nil;
    }
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    return obj[0] == MAGIC_CLASS ? g_sym_t : nil;
}

/**
 * 組み込み関数%%MAKE-INSTANCE-RAW。ILOSクラスインスタンス(MAGIC_CLASS_INSTANCE)を確保する。
 * @param args 評価済みの引数リスト(class クラスオブジェクト, slots-vector MAGIC_VECTOR)
 * @param env 呼び出し時の環境(未使用)
 * @return 確保したクラスインスタンス
 */
lisp_val_t primitive_make_instance_raw(lisp_val_t args, lisp_val_t env) {
    lisp_val_t class_obj = cc_car(args);
    lisp_val_t slots_vector = cc_car(cc_cdr(args));
    return os_make_instance(MAGIC_CLASS_INSTANCE, class_obj, slots_vector, nil);
}

/**
 * 組み込み関数%%INSTANCE-CLASS。ILOSクラスインスタンスのclass(クラスオブジェクト)を返す。
 * @param args 評価済みの引数リスト(第一引数はクラスインスタンス)
 * @param env 呼び出し時の環境(未使用)
 * @return class(クラスオブジェクト)
 */
lisp_val_t primitive_instance_class(lisp_val_t args, lisp_val_t env) {
    UINT64 *obj = (UINT64 *)(cc_car(args) & ~TAG_MASK);
    return obj[1];
}

/**
 * 組み込み関数%%INSTANCE-SLOTS。ILOSクラスインスタンスのslots-vector(MAGIC_VECTOR)を返す。
 * @param args 評価済みの引数リスト(第一引数はクラスインスタンス)
 * @param env 呼び出し時の環境(未使用)
 * @return slots-vector(MAGIC_VECTOR)
 */
lisp_val_t primitive_instance_slots(lisp_val_t args, lisp_val_t env) {
    UINT64 *obj = (UINT64 *)(cc_car(args) & ~TAG_MASK);
    return obj[2];
}

/**
 * 組み込み関数%%CLASS-INSTANCE-P。第一引数がILOSクラスインスタンス(MAGIC_CLASS_INSTANCE)かどうかを判定する。
 * @param args 評価済みの引数リスト(第一引数は任意の値)
 * @param env 呼び出し時の環境(未使用)
 * @return クラスインスタンスならt、それ以外ならnil
 */
lisp_val_t primitive_class_instance_p(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) != TAG_INSTANCE) {
        return nil;
    }
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    return obj[0] == MAGIC_CLASS_INSTANCE ? g_sym_t : nil;
}

/**
 * 組み込み関数%%SET-DYNAMIC。os_set_dynamicの薄いラッパーで、レキシカルなenvの
 * 親子関係とは無関係なグローバルの動的変数(defdynamicで定義したもの)を、
 * 関数呼び出しの内側からでも書き換えられるようにする。
 * @param args (name value) 評価済みの引数リスト。nameは動的変数名のシンボル
 * @param env 呼び出し時の環境(未使用)
 * @return 書き込んだvalue
 */
lisp_val_t primitive_set_dynamic(lisp_val_t args, lisp_val_t env) {
    lisp_val_t name = cc_car(args);
    lisp_val_t val = cc_car(cc_cdr(args));
    return os_set_dynamic(name, val);
}


/*-
 * (defun set-variable (sym val env)
 *   (let* ((var-cell (car env))
 *          (alist (cdr var-cell))
 *          (existing (assoc sym alist)))
 *     (if existing
 *         (set-cdr! existing val)
 *         (set-cdr! var-cell (cons (cons sym val) alist)))))
 */


// (defun make-environment (parent-env)
//   (list '() parent-env))
// (defun find-symbol (sym env)
//   (assoc sym env))

