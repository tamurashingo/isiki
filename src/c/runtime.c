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
 * #define TAG_VECTOR   0x6ULL   // 110 アドレス
 *                               // 111 予約
 *
 * ---- タグ別メモリ配置 ----
 *
 * TAG_FIXNUM: 即値、ヒープなし
 *  [ val(61bit) ...................................... ][0 0 0]
 *    61bitで表現できる即値を埋め込む
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
 *
 * TAG_VECTOR: アドレス、ヒープ可変長(8*(1+rank+total)byte, 8byte境界に整列)
 *  [ vector-addr(61bit) .............................. ][1 1 0]
 *    多次元配列(general array)のアドレスが入っている
 *    - word0:          次元数(rank、タグなしの整数)
 *    - word[1..rank]:  各次元のサイズ(タグなしの整数)
 *    - word[rank+1..]: 要素本体(タグ付きのlisp_val_t、行優先(row-major)順)
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
        os_set_function(os_make_symbol("NUMBERP"), os_make_native_function((lisp_addr_t)(void *)primitive_numberp), global_environment);
        os_set_function(os_make_symbol("FIXNUMP"), os_make_native_function((lisp_addr_t)(void *)primitive_fixnump), global_environment);
        os_set_function(os_make_symbol("SYMBOLP"), os_make_native_function((lisp_addr_t)(void *)primitive_symbolp), global_environment);
        os_set_function(os_make_symbol("CONSP"), os_make_native_function((lisp_addr_t)(void *)primitive_consp), global_environment);
        os_set_function(os_make_symbol("SYMBOL-NAME"), os_make_native_function((lisp_addr_t)(void *)primitive_symbol_name), global_environment);
        os_set_function(os_make_symbol("STRING-TO-SYMBOL"), os_make_native_function((lisp_addr_t)(void *)primitive_string_to_symbol), global_environment);
        os_set_function(os_make_symbol("GENSYM"), os_make_native_function((lisp_addr_t)(void *)primitive_gensym), global_environment);
        os_set_function(os_make_symbol("MAKE-ARRAY"), os_make_native_function((lisp_addr_t)(void *)primitive_make_array), global_environment);
        os_set_function(os_make_symbol("AREF"), os_make_native_function((lisp_addr_t)(void *)primitive_aref), global_environment);
        os_set_function(os_make_symbol("ARRAY-DIMENSIONS"), os_make_native_function((lisp_addr_t)(void *)primitive_array_dimensions), global_environment);
        os_set_function(os_make_symbol("SET-CAR"), os_make_native_function((lisp_addr_t)(void *)primitive_set_car), global_environment);
        os_set_function(os_make_symbol("SET-CDR"), os_make_native_function((lisp_addr_t)(void *)primitive_set_cdr), global_environment);
        os_set_function(os_make_symbol("SET-AREF"), os_make_native_function((lisp_addr_t)(void *)primitive_set_aref), global_environment);
        os_set_function(os_make_symbol("CREATE-STRING"), os_make_native_function((lisp_addr_t)(void *)primitive_create_string), global_environment);
        os_set_function(os_make_symbol("STRING-ELT"), os_make_native_function((lisp_addr_t)(void *)primitive_string_elt), global_environment);
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
 * fixnumオブジェクトを作る(即値、ヒープ確保なし)。
 * @param fixnum 表現する値
 * @return タグ付けされたFIXNUM
 */
lisp_val_t os_make_fixnum(const UINT64 fixnum) {
    // TODO: 61bitで表現しきれない数の場合はbignumにする
    return (lisp_val_t)(fixnum << 3);
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
 * 組み込み関数+。argsの全fixnumを合計する。
 * @param args 評価済みの引数リスト(すべてFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 合計値のFIXNUM
 */
lisp_val_t primitive_add(lisp_val_t args, lisp_val_t env) {
    UINT64 sum = 0;
    for (lisp_val_t cur = args; cur != nil; cur = cc_cdr(cur)) {
        sum += cc_car(cur) >> 3;
    }
    return os_make_fixnum(sum);
}

/**
 * 組み込み関数-。argsの第一引数から残りを順に減算する(単項の符号反転は未サポート)。
 * @param args 評価済みの引数リスト(すべてFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 減算結果のFIXNUM
 */
lisp_val_t primitive_subtract(lisp_val_t args, lisp_val_t env) {
    // fixnumは符号無しの表現しか持たないため、単項の符号反転はサポートしない
    UINT64 result = cc_car(args) >> 3;
    for (lisp_val_t rest = cc_cdr(args); rest != nil; rest = cc_cdr(rest)) {
        result -= cc_car(rest) >> 3;
    }
    return os_make_fixnum(result);
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
 * 組み込み関数*。argsの全fixnumを乗算する。
 * @param args 評価済みの引数リスト(すべてFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 積のFIXNUM
 */
lisp_val_t primitive_multiply(lisp_val_t args, lisp_val_t env) {
    UINT64 product = 1;
    for (lisp_val_t cur = args; cur != nil; cur = cc_cdr(cur)) {
        product *= cc_car(cur) >> 3;
    }
    return os_make_fixnum(product);
}

/**
 * 組み込み関数/。argsの第一引数から残りを順に除算する(整数除算)。
 * @param args 評価済みの引数リスト(すべてFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 除算結果のFIXNUM。0除算の場合はg_sym_eval_error
 */
lisp_val_t primitive_divide(lisp_val_t args, lisp_val_t env) {
    UINT64 result = cc_car(args) >> 3;
    for (lisp_val_t rest = cc_cdr(args); rest != nil; rest = cc_cdr(rest)) {
        UINT64 divisor = cc_car(rest) >> 3;
        if (divisor == 0) {
            return g_sym_eval_error;
        }
        result /= divisor;
    }
    return os_make_fixnum(result);
}

/**
 * 組み込み関数<。argsが単調増加(a<b<c<...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調増加ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_less_than(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        UINT64 a = cc_car(rest) >> 3;
        UINT64 b = cc_car(cc_cdr(rest)) >> 3;
        if (!(a < b)) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数>。argsが単調減少(a>b>c>...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調減少ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_greater_than(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        UINT64 a = cc_car(rest) >> 3;
        UINT64 b = cc_car(cc_cdr(rest)) >> 3;
        if (!(a > b)) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数=。argsがすべて等しいかどうかを判定する。
 * @param args 評価済みの引数リスト(すべてFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return すべて等しいならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_num_equal(lisp_val_t args, lisp_val_t env) {
    for (lisp_val_t rest = args; rest != nil && cc_cdr(rest) != nil; rest = cc_cdr(rest)) {
        UINT64 a = cc_car(rest) >> 3;
        UINT64 b = cc_car(cc_cdr(rest)) >> 3;
        if (a != b) {
            return nil;
        }
    }
    return g_sym_t;
}

/**
 * 組み込み関数NUMBERP。第一引数が数値(現状はFIXNUMのみ)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 数値ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_numberp(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    return (val & TAG_MASK) == TAG_FIXNUM ? g_sym_t : nil;
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

    lisp_addr_t addr = os_alloc_bytes(8 * (1 + rank + total));
    lisp_val_t *header = (lisp_val_t *)addr;
    header[0] = rank;
    for (UINT64 i = 0; i < rank; i++) {
        header[1 + i] = dims[i];
    }
    lisp_val_t *data = (lisp_val_t *)(addr + 8 * (1 + rank));
    for (UINT64 i = 0; i < total; i++) {
        data[i] = nil;
    }

    return (lisp_val_t)(addr | TAG_VECTOR);
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
    lisp_addr_t addr = array & ~TAG_MASK;
    lisp_val_t *header = (lisp_val_t *)addr;
    UINT64 rank = header[0];

    int out_of_bounds;
    UINT64 offset = array_offset(header, rank, cc_cdr(args), &out_of_bounds);
    if (out_of_bounds) {
        return g_sym_eval_error;
    }

    lisp_val_t *data = (lisp_val_t *)(addr + 8 * (1 + rank));
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
    lisp_addr_t addr = array & ~TAG_MASK;
    lisp_val_t *header = (lisp_val_t *)addr;
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
    lisp_addr_t addr = array & ~TAG_MASK;
    lisp_val_t *header = (lisp_val_t *)addr;
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

    lisp_val_t *data = (lisp_val_t *)(addr + 8 * (1 + rank));
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
        case TAG_VECTOR: {
            lisp_addr_t addr = seq & ~TAG_MASK;
            lisp_val_t *header = (lisp_val_t *)addr;
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
 * @param args 評価済みの引数リスト(class クラスオブジェクト, slots-vector TAG_VECTOR)
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
 * 組み込み関数%%INSTANCE-SLOTS。ILOSクラスインスタンスのslots-vector(TAG_VECTOR)を返す。
 * @param args 評価済みの引数リスト(第一引数はクラスインスタンス)
 * @param env 呼び出し時の環境(未使用)
 * @return slots-vector(TAG_VECTOR)
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

