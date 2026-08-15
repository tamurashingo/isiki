#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "lisp.h"
#include "eval.h"
#include "reader.h"
#include "stream.h"
#ifdef ISIKIOS_UNIT_TEST
/* ネイティブ(x86_64以外を含む)ホストでのユニットテストではx87/SSE2インラインアセンブラが
   使えないため、libmの対応する関数で計算する。実機(x86_64 UEFI, ISIKIOS_UNIT_TEST未定義)
   では下のasm実装のみが使われる。 */
#include <math.h>
#endif

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
 * #define TAG_MASK        0x7ULL
 * #define TAG_FIXNUM      0x0ULL // 000 即値
 * #define TAG_CONS        0x1ULL // 001 アドレス
 * #define TAG_SYMBOL      0x2ULL // 010 アドレス
 * #define TAG_CHAR        0x3ULL // 011 即値
 * #define TAG_STRING      0x4ULL // 100 アドレス
 * #define TAG_INSTANCE    0x5ULL // 101 アドレス
 * #define TAG_FORWARD     0x6ULL // 110 アドレス
 * #define TAG_RAW_POINTER 0x7ULL // 111 アドレス
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
 * TAG_SYMBOL: アドレス、ヒープ32Byte
 *  [ sym-addr(61bit) ................................. ][0 1 0]
 *    symbol用に確保した32byteへのアドレスが入っている
 *    - word0: 名前のstringへのポインタ(STRINGオブジェクト)
 *             symbol の値や関数は環境(environment)から取得する
 *    - word1: gensymフラグ。nilなら通常のinterned symbol(g_symbol_tableに登録済み)、
 *             非nilならos_make_uninterned_symbolが作ったgensym由来のsymbolであることを
 *             示す(将来のGCでgensym symbolだけを回収対象にする際の判定に使う想定)
 *    - word2, word3: 未使用(予約)
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
 *    - word2: MAGIC_FUNCTION_NATIVEの場合: fixnum 1(za.cがコンパイルした関数)またはNIL(組み込みprimitive)。
 *             GCで移動しない即値(fixnum/nil)のみを許すことで、word1と同様に素通しできる
 *             MAGIC_FUNCTION_INTERPRETEDの場合: 本体(未評価のフォーム列)
 *    - word3: MAGIC_FUNCTION_INTERPRETEDの場合: 定義時の環境のアドレス
 *             MAGIC_VECTORの場合: word1に多次元配列(general array)本体への生ポインタを
 *             持つ(ヒープ可変長、8*(1+rank+total)byte、8byte境界に整列)。
 *             本体のレイアウトは以下の通り:
 *               - word0:          次元数(rank、タグなしの整数)
 *               - word[1..rank]:  各次元のサイズ(タグなしの整数)
 *               - word[rank+1..]: 要素本体(タグ付きのlisp_val_t、行優先(row-major)順)
 *
 * TAG_FORWARD: アドレス、ヒープ(コピー元のサイズ)
 *  [ forward-addr(61bit) ............................. ][1 1 0]
 *    From空間からTo空間へコピー済みのオブジェクト
 *    移動先アドレス | TAG_FORWARD で上書きする
 *
 * TAG_RAW_POINTER: アドレス、即値
 *  [ raw-addr(61bit) ................................. ][1 1 1]
 *    Lispから生の64bitアドレス(c構造体・MMIOレジスタ等)を安全に持てるようにするためのタグ。
 *    中身は「タグを外した生アドレス」そのもので、fixnumのようなシフトエンコードは行わない。
 *    GCはfixnum(0)・char(3)と同様にこのタグを即値として素通しスキャンしない。
 *
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
/** dotted pair記法の'.'を表すシンボル。reader.cのread_listが単独トークンの'.'を検出するために使う */
lisp_val_t g_sym_dot;
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

/** init.lisp の make-instance 関数を表すシンボル(os_signal_conditionがC→Lisp呼び出しに使う) */
lisp_val_t g_sym_make_instance;
/** init.lisp の signal-condition 関数を表すシンボル(os_signal_conditionがC→Lisp呼び出しに使う) */
lisp_val_t g_sym_signal_condition;
/** init.lisp の %find-class 関数を表すシンボル(signal_domain_errorが expected-class 解決に使う) */
lisp_val_t g_sym_percent_find_class;
/** <domain-error> クラスを表すシンボル */
lisp_val_t g_sym_class_domain_error;
/** <parse-error> クラスを表すシンボル */
lisp_val_t g_sym_class_parse_error;
/** <number> クラスを表すシンボル */
lisp_val_t g_sym_class_number;
/** :object キーワードを表すシンボル(<domain-error>の初期化引数) */
lisp_val_t g_sym_kw_object;
/** :expected-class キーワードを表すシンボル(<domain-error>/<parse-error>の初期化引数) */
lisp_val_t g_sym_kw_expected_class;
/** :string キーワードを表すシンボル(<parse-error>の初期化引数) */
lisp_val_t g_sym_kw_string;

/** ルートの環境(全プロセスの環境が最終的にこれを親として辿る) */
lisp_val_t global_environment;

/** defdynamicで定義された動的変数の値を保持するグローバルなフラットalist(sym . val)。レキシカルなenvの親子関係とは無関係 */
lisp_val_t g_dynamic_bindings;

/** internされたsymbolを保持できる最大数 */
#define MAX_SYMBOLS 8192
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
 * nil(自己参照するconsセル)専用の固定領域。From/To空間のどちらにも属さず、GCの
 * コピー/スワップの対象にならない(g_symbol_table等と同じ静的配列パターン)。
 */
static UINT8 g_nil_cell[16] __attribute__((aligned(8)));

/**
 * From空間からnバイト(8byte境界に整列)を割り当てる。枯渇した場合は停止する。
 * @param n 割り当てるバイト数
 * @return 割り当てたメモリの先頭アドレス
 */
static lisp_addr_t os_alloc_bytes(UINT64 n) {
    UINT64 aligned = (n + 7) & ~7ULL;
#ifndef ISIKIOS_UNIT_TEST
    __asm__ __volatile__ ("cli");
#endif
    UINT8 *p = g_from_ptr;
    if (p + aligned > g_from_end) {
        // From空間が枯渇。GCを1回走らせて空きを作れるか試す
        os_gc_collect();
        p = g_from_ptr;
        if (p + aligned > g_from_end) {
            // GC後もなお不足している場合は本当の枯渇として停止する
            frame_buffer *fb = get_active_frame_buffer();
            fb->write_string(fb, "out of memory...");
            for (;;) {
            }
        }
    }
    g_from_ptr = p + aligned;
#ifndef ISIKIOS_UNIT_TEST
    __asm__ __volatile__ ("sti");
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

double os_heap_used_ratio(void) {
    UINT64 total = (UINT64)(g_from_end - g_from_start);
    if (total == 0) {
        return 0.0;
    }
    UINT64 used = (UINT64)(g_from_ptr - g_from_start);
    return (double)used / (double)total;
}

/** os_gc_collectが呼ばれた延べ回数。テストが「計算中に実際にGCが発火したか」を確認するために使う */
static UINT64 g_gc_collect_count = 0;

UINT64 os_gc_collect_count(void) {
    return g_gc_collect_count;
}

/* ============================== GC (Cheney方式コピーGC) ============================== */

/**
 * lisp_val_t型の変数へのポインタを、GCが毎回のos_gc_collectで書き換えるルート集合に
 * 登録する(idempotent: 同じポインタを複数回登録しても1回分の登録として扱う)。
 * initialize_processes等、同じアドレスを何度も渡してくる呼び出し元があるため、
 * 単純な追加だけだとテーブルがすぐ枯渇する。
 */
#define GC_MAX_EXTRA_ROOTS 40
static lisp_val_t *g_gc_extra_roots[GC_MAX_EXTRA_ROOTS];
static UINT64 g_gc_extra_root_count = 0;

void os_gc_register_root(lisp_val_t *root_ptr) {
    for (UINT64 i = 0; i < g_gc_extra_root_count; i++) {
        if (g_gc_extra_roots[i] == root_ptr) {
            return;
        }
    }
    if (g_gc_extra_root_count >= GC_MAX_EXTRA_ROOTS) {
        frame_buffer *fb = get_active_frame_buffer();
        fb->write_string(fb, "gc: extra root table exhausted...");
        for (;;) {
        }
    }
    g_gc_extra_roots[g_gc_extra_root_count++] = root_ptr;
}

/**
 * コピー済み(To空間へ転送済み)だがまだフィールドを転送していないオブジェクトのFIFOキュー
 * (gray list)。conの再帰的コピー(depth-first)はcdr連鎖でCスタックを溢れさせる危険が
 * あるため、コピー自体は再帰せずここへ積み、後段のgc_scan_queueがbreadth-firstに消費する。
 */
#define GC_QUEUE_CAPACITY 65536
static lisp_val_t g_gc_queue[GC_QUEUE_CAPACITY];
static UINT64 g_gc_queue_head;
static UINT64 g_gc_queue_tail;

static void gc_queue_push(lisp_val_t tagged) {
    if (g_gc_queue_tail - g_gc_queue_head >= GC_QUEUE_CAPACITY) {
        frame_buffer *fb = get_active_frame_buffer();
        fb->write_string(fb, "gc: work queue exhausted...");
        for (;;) {
        }
    }
    g_gc_queue[g_gc_queue_tail % GC_QUEUE_CAPACITY] = tagged;
    g_gc_queue_tail++;
}

static int gc_queue_pop(lisp_val_t *out) {
    if (g_gc_queue_head == g_gc_queue_tail) {
        return 0;
    }
    *out = g_gc_queue[g_gc_queue_head % GC_QUEUE_CAPACITY];
    g_gc_queue_head++;
    return 1;
}

/** To空間にsizeバイトを確保して返す(枯渇時は診断メッセージを表示して停止する) */
static UINT8 *gc_to_alloc(UINT64 size) {
    UINT64 aligned = (size + 7) & ~7ULL;
    UINT8 *dst = g_to_ptr;
    if (dst + aligned > g_to_end) {
        frame_buffer *fb = get_active_frame_buffer();
        fb->write_string(fb, "gc: to-space exhausted...");
        for (;;) {
        }
    }
    g_to_ptr = dst + aligned;
    return dst;
}

/**
 * objをFrom空間からTo空間へコピー(既に転送済みならその転送先)して返す。
 * fixnum/char/TAG_RAW_POINTERは即値としてそのまま返し、nilも何よりも先に素通しする。
 * @param obj コピー対象の値
 * @return To空間上の(同じ意味を持つ)値
 */
static lisp_val_t gc_copy_value(lisp_val_t obj) {
    if (obj == nil) {
        return obj;
    }

    UINT64 tag = obj & TAG_MASK;
    if (tag == TAG_FIXNUM || tag == TAG_CHAR || tag == TAG_RAW_POINTER) {
        return obj;
    }

    lisp_addr_t addr = obj & ~TAG_MASK;
    UINT64 *words = (UINT64 *)addr;
    UINT64 word0 = words[0];

    if ((word0 & TAG_MASK) == TAG_FORWARD) {
        UINT8 *fwd_addr = (UINT8 *)(lisp_addr_t)(word0 & ~TAG_MASK);
        // Stringのword0は生の整数長であり、たまたま下位3bitが0x6(TAG_FORWARD)と一致した
        // だけの誤検知の可能性がある。転送先は必ずTo空間内のアドレスになるはずなので、
        // 範囲外なら転送済みではないとみなし、下のcopy_freshへ進む
        if (fwd_addr >= g_to_start && fwd_addr < g_to_ptr) {
            return (lisp_val_t)((lisp_addr_t)fwd_addr | tag);
        }
    }

    UINT64 size;
    switch (tag) {
        case TAG_CONS:     size = 16; break;
        case TAG_SYMBOL:   size = 32; break;
        case TAG_STRING:   size = 8 + word0; break;
        case TAG_INSTANCE: size = 32; break;
        default:           size = 0; break;
    }

    UINT8 *dst = gc_to_alloc(size);
    UINT8 *src = (UINT8 *)addr;
    for (UINT64 i = 0; i < size; i++) {
        dst[i] = src[i];
    }

    // 古いFrom空間側のword0を転送先アドレス+TAG_FORWARDで上書きする
    // (Stringの場合はここでword0=生の整数長が上書きされるのが必須の副作用)
    words[0] = (UINT64)((lisp_addr_t)dst | TAG_FORWARD);

    lisp_val_t new_obj = (lisp_val_t)((lisp_addr_t)dst | tag);
    gc_queue_push(new_obj); // 子要素の転送はここでは行わず、gc_scan_queueに委ねる(非再帰)
    return new_obj;
}

/** MAGIC_BIGNUM(word3=limb配列への生ポインタ、中身はLisp値を含まない生の32bit値の配列)を再配置する */
static void gc_relocate_bignum(UINT64 *words) {
    UINT64 count = words[2];
    UINT8 *dst = gc_to_alloc(8 * count);
    UINT8 *src = (UINT8 *)words[3];
    for (UINT64 i = 0; i < 8 * count; i++) {
        dst[i] = src[i];
    }
    words[3] = (UINT64)dst;
}

/**
 * MAGIC_VECTORの配列本体ブロック(word0=rank、word[1..rank]=各次元サイズ、
 * word[rank+1..]=要素データ)を再配置する。要素データはLisp値なのでgc_copy_valueで転送する。
 */
static void gc_relocate_vector_block(UINT64 *words) {
    UINT64 *old_header = (UINT64 *)words[1];
    UINT64 rank = old_header[0];
    UINT64 total = 1;
    for (UINT64 i = 0; i < rank; i++) {
        total *= old_header[1 + i];
    }
    UINT64 size = 8 * (1 + rank + total);

    UINT8 *dst = gc_to_alloc(size);
    UINT8 *src = (UINT8 *)old_header;
    for (UINT64 i = 0; i < size; i++) {
        dst[i] = src[i];
    }

    lisp_val_t *new_data = (lisp_val_t *)(dst + 8 * (1 + rank));
    for (UINT64 i = 0; i < total; i++) {
        new_data[i] = gc_copy_value(new_data[i]);
    }

    words[1] = (UINT64)dst;
}

/**
 * MAGIC_STREAM(word1=os_stream_tへの生ポインタ)を再配置する。buf_dataはos_stream_t
 * 本体に埋め込まれた配列なので構造体ごとコピーすれば移動する。out_fb(frame buffer)は
 * 常にLispヒープ外の静的領域を指すため素通しし、str_buf(STREAM_STRING_INPUT/OUTPUT
 * のみヒープ上)だけ追加で再配置する。
 */
static void gc_relocate_stream(UINT64 *words) {
    os_stream_t *old_stream = (os_stream_t *)words[1];
    UINT8 *dst = gc_to_alloc(sizeof(os_stream_t));
    UINT8 *src = (UINT8 *)old_stream;
    for (UINT64 i = 0; i < sizeof(os_stream_t); i++) {
        dst[i] = src[i];
    }

    os_stream_t *new_stream = (os_stream_t *)dst;
    if (new_stream->kind == STREAM_STRING_INPUT || new_stream->kind == STREAM_STRING_OUTPUT) {
        UINT8 *buf_dst = gc_to_alloc(new_stream->str_cap);
        UINT8 *buf_src = (UINT8 *)new_stream->str_buf;
        for (UINT64 i = 0; i < new_stream->str_cap; i++) {
            buf_dst[i] = buf_src[i];
        }
        new_stream->str_buf = buf_dst;
    }

    words[1] = (UINT64)new_stream;
}

/**
 * TAG_INSTANCE(words[0]=magic)のword1〜word3を、magicごとの規則に従って
 * To空間上のコピーへ転送する。
 */
static void gc_scan_instance(UINT64 *words) {
    UINT64 magic = words[0];

    switch (magic) {
        case MAGIC_FUNCTION_NATIVE:
            // word1はCコード領域への生の関数ポインタ(Lispヒープ外)。素通し
            break;

        case MAGIC_FUNCTION_INTERPRETED:
        case MAGIC_MACRO:
            words[1] = gc_copy_value(words[1]); // params
            words[2] = gc_copy_value(words[2]); // body
            words[3] = gc_copy_value(words[3]); // closure env
            break;

        case MAGIC_PROCESS:
            words[1] = gc_copy_value(words[1]); // fixnum(process index)
            // word2(saved_rsp)は生アドレス(process.cの静的スタック領域、Lispヒープ外)。素通し
            words[3] = gc_copy_value(words[3]); // state symbol
            break;

        case MAGIC_BLOCK_EXIT:
            words[1] = gc_copy_value(words[1]); // block名symbol
            words[2] = gc_copy_value(words[2]); // 戻り値
            break;

        case MAGIC_STREAM:
            gc_relocate_stream(words);
            break;

        case MAGIC_CLASS_INSTANCE:
            words[1] = gc_copy_value(words[1]); // class
            words[2] = gc_copy_value(words[2]); // slots-vector
            break;

        case MAGIC_CATCH_EXIT:
            words[1] = gc_copy_value(words[1]); // tag
            words[2] = gc_copy_value(words[2]); // throwされた値
            break;

        case MAGIC_GO_EXIT:
            words[1] = gc_copy_value(words[1]); // tag symbol
            break;

        case MAGIC_BIGNUM:
            // word1(sign)/word2(limb数)は生のint。word3(limb配列)のみ再配置する
            gc_relocate_bignum(words);
            break;

        case MAGIC_VECTOR:
            // word1==0はまだ本体ブロックを持たないプレースホルダ(確保直後にラップして
            // GC_PROTECTし、その後で本体ブロックを確保する構築中の状態)。再配置対象が
            // 存在しないのでそのまま素通しする
            if (words[1] != 0) {
                gc_relocate_vector_block(words);
            }
            break;

        case MAGIC_FLOAT:
            // word1はdoubleのビットパターン(生の64bit値)。素通し
            break;

        case MAGIC_BUILTIN_CLASS:
        case MAGIC_STANDARD_CLASS:
            words[1] = gc_copy_value(words[1]); // name symbol
            words[2] = gc_copy_value(words[2]); // superclasses list
            words[3] = gc_copy_value(words[3]); // slots list
            break;

        default:
            break;
    }
}

/**
 * ワークキューを空になるまで消費し、To空間へコピーされたオブジェクトのフィールドを
 * 転送する。gc_copy_valueは子要素をここへ積むだけで再帰しないため、breadth-firstに
 * 進み、長いcons連鎖でもCスタックを消費しない。
 */
static void gc_scan_queue(void) {
    lisp_val_t tagged;
    while (gc_queue_pop(&tagged)) {
        UINT64 tag = tagged & TAG_MASK;
        UINT64 *words = (UINT64 *)(tagged & ~TAG_MASK);

        switch (tag) {
            case TAG_CONS:
                words[0] = gc_copy_value(words[0]);
                words[1] = gc_copy_value(words[1]);
                break;

            case TAG_SYMBOL:
                // word0=name string。word1(gensymフラグ)/word2/word3(未使用)は常に
                // nilまたはfixnumなのでgc_copy_valueに通しても素通しされるだけで安全
                words[0] = gc_copy_value(words[0]);
                words[1] = gc_copy_value(words[1]);
                words[2] = gc_copy_value(words[2]);
                words[3] = gc_copy_value(words[3]);
                break;

            case TAG_STRING:
                // 文字データのみでLisp値を指すフィールドが無いため何もしない
                break;

            case TAG_INSTANCE:
                gc_scan_instance(words);
                break;

            default:
                break;
        }
    }
}

/**
 * Cheney方式のコピーGCを1回実行する。global_environment・g_dynamic_bindings・
 * g_symbol_table・キャッシュ済みg_sym_*・os_gc_register_rootで登録されたroot・
 * 全プロセスのshadow stack(GC_PROTECTされたCローカル変数)をルートとして
 * 生存オブジェクトをTo空間へコピーし、完了後にFrom/To空間を入れ替える。
 */
void os_gc_collect(void) {
    g_gc_collect_count++;
    g_to_ptr = g_to_start;
    g_gc_queue_head = 0;
    g_gc_queue_tail = 0;

    global_environment = gc_copy_value(global_environment);
    g_dynamic_bindings = gc_copy_value(g_dynamic_bindings);

    for (int i = 0; i < g_symbol_count; i++) {
        g_symbol_table[i] = gc_copy_value(g_symbol_table[i]);
    }

    g_sym_t = gc_copy_value(g_sym_t);
    g_sym_process_ready = gc_copy_value(g_sym_process_ready);
    g_sym_process_running = gc_copy_value(g_sym_process_running);
    g_sym_process_dead = gc_copy_value(g_sym_process_dead);
    g_sym_run_queue = gc_copy_value(g_sym_run_queue);
    g_sym_current_process = gc_copy_value(g_sym_current_process);
    g_sym_quote = gc_copy_value(g_sym_quote);
    g_sym_if = gc_copy_value(g_sym_if);
    g_sym_progn = gc_copy_value(g_sym_progn);
    g_sym_setq = gc_copy_value(g_sym_setq);
    g_sym_defun = gc_copy_value(g_sym_defun);
    g_sym_lambda = gc_copy_value(g_sym_lambda);
    g_sym_defmacro = gc_copy_value(g_sym_defmacro);
    g_sym_block = gc_copy_value(g_sym_block);
    g_sym_return_from = gc_copy_value(g_sym_return_from);
    g_sym_unwind_protect = gc_copy_value(g_sym_unwind_protect);
    g_sym_function = gc_copy_value(g_sym_function);
    g_sym_flet = gc_copy_value(g_sym_flet);
    g_sym_labels = gc_copy_value(g_sym_labels);
    g_sym_defvar = gc_copy_value(g_sym_defvar);
    g_sym_defconstant = gc_copy_value(g_sym_defconstant);
    g_sym_defdynamic = gc_copy_value(g_sym_defdynamic);
    g_sym_defglobal = gc_copy_value(g_sym_defglobal);
    g_sym_dynamic = gc_copy_value(g_sym_dynamic);
    g_sym_rest = gc_copy_value(g_sym_rest);
    g_sym_quasiquote = gc_copy_value(g_sym_quasiquote);
    g_sym_unquote = gc_copy_value(g_sym_unquote);
    g_sym_unquote_splicing = gc_copy_value(g_sym_unquote_splicing);
    g_sym_dot = gc_copy_value(g_sym_dot);
    g_sym_car = gc_copy_value(g_sym_car);
    g_sym_cdr = gc_copy_value(g_sym_cdr);
    g_sym_cons = gc_copy_value(g_sym_cons);
    g_sym_read_error = gc_copy_value(g_sym_read_error);
    g_sym_eval_error = gc_copy_value(g_sym_eval_error);
    g_sym_top_level_block = gc_copy_value(g_sym_top_level_block);
    g_sym_catch = gc_copy_value(g_sym_catch);
    g_sym_throw = gc_copy_value(g_sym_throw);
    g_sym_tagbody = gc_copy_value(g_sym_tagbody);
    g_sym_go = gc_copy_value(g_sym_go);
    g_sym_make_instance = gc_copy_value(g_sym_make_instance);
    g_sym_signal_condition = gc_copy_value(g_sym_signal_condition);
    g_sym_percent_find_class = gc_copy_value(g_sym_percent_find_class);
    g_sym_class_domain_error = gc_copy_value(g_sym_class_domain_error);
    g_sym_class_parse_error = gc_copy_value(g_sym_class_parse_error);
    g_sym_class_number = gc_copy_value(g_sym_class_number);
    g_sym_kw_object = gc_copy_value(g_sym_kw_object);
    g_sym_kw_expected_class = gc_copy_value(g_sym_kw_expected_class);
    g_sym_kw_string = gc_copy_value(g_sym_kw_string);

    for (UINT64 i = 0; i < g_gc_extra_root_count; i++) {
        *g_gc_extra_roots[i] = gc_copy_value(*g_gc_extra_roots[i]);
    }

    // 全プロセスのshadow stack(GC_PROTECTされたCローカル変数)をスキャンする。
    // プリエンプティブなプロセス切替により、実行中でない他プロセスもCスタックの
    // 途中(evalの再帰の中)で停止しているだけなので、自プロセスだけでなく全プロセスを辿る
    for (UINT32 i = 0; i < PROCESS_COUNT; i++) {
        for (gc_rootnode *node = get_process(i)->gc_roots; node != 0; node = node->next) {
            *node->var_ptr = gc_copy_value(*node->var_ptr);
        }
    }

    gc_scan_queue();

    UINT8 *new_from_start = g_to_start;
    UINT8 *new_from_end = g_to_end;
    UINT8 *new_from_ptr = g_to_ptr;
    UINT8 *new_to_start = g_from_start;
    UINT8 *new_to_end = g_from_end;

    g_from_start = new_from_start;
    g_from_end = new_from_end;
    g_from_ptr = new_from_ptr;
    g_to_start = new_to_start;
    g_to_end = new_to_end;
    g_to_ptr = g_to_start;
}

/** NIL・global_environment・組み込みシンボル/関数を構築し、Lisp実行環境を起動する */
void os_bootstrap() {
    // NIL の作成。From/To空間どちらにも属さない専用の固定領域(g_nil_cell)を使う
    {
        lisp_addr_t addr = (lisp_addr_t)g_nil_cell;
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
        g_sym_dot = os_make_symbol(".");
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

        g_sym_make_instance = os_make_symbol("MAKE-INSTANCE");
        g_sym_signal_condition = os_make_symbol("SIGNAL-CONDITION");
        g_sym_percent_find_class = os_make_symbol("%FIND-CLASS");
        g_sym_class_domain_error = os_make_symbol("<DOMAIN-ERROR>");
        g_sym_class_parse_error = os_make_symbol("<PARSE-ERROR>");
        g_sym_class_number = os_make_symbol("<NUMBER>");
        g_sym_kw_object = os_make_symbol(":OBJECT");
        g_sym_kw_expected_class = os_make_symbol(":EXPECTED-CLASS");
        g_sym_kw_string = os_make_symbol(":STRING");


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
        os_set_function(os_make_symbol("FLOATP"), os_make_native_function((lisp_addr_t)(void *)primitive_floatp), global_environment);
        os_set_function(os_make_symbol("FLOAT"), os_make_native_function((lisp_addr_t)(void *)primitive_float), global_environment);
        os_set_function(os_make_symbol("SQRT"), os_make_native_function((lisp_addr_t)(void *)primitive_sqrt), global_environment);
        os_set_function(os_make_symbol("LOG"), os_make_native_function((lisp_addr_t)(void *)primitive_log), global_environment);
        os_set_function(os_make_symbol("EXP"), os_make_native_function((lisp_addr_t)(void *)primitive_exp), global_environment);
        os_set_function(os_make_symbol("SIN"), os_make_native_function((lisp_addr_t)(void *)primitive_sin), global_environment);
        os_set_function(os_make_symbol("COS"), os_make_native_function((lisp_addr_t)(void *)primitive_cos), global_environment);
        os_set_function(os_make_symbol("ATAN2"), os_make_native_function((lisp_addr_t)(void *)primitive_atan2), global_environment);
        os_set_function(os_make_symbol("FLOOR"), os_make_native_function((lisp_addr_t)(void *)primitive_floor), global_environment);
        os_set_function(os_make_symbol("CEILING"), os_make_native_function((lisp_addr_t)(void *)primitive_ceiling), global_environment);
        os_set_function(os_make_symbol("TRUNCATE"), os_make_native_function((lisp_addr_t)(void *)primitive_truncate), global_environment);
        os_set_function(os_make_symbol("ROUND"), os_make_native_function((lisp_addr_t)(void *)primitive_round), global_environment);
        os_set_function(os_make_symbol("PARSE-NUMBER"), os_make_native_function((lisp_addr_t)(void *)primitive_parse_number), global_environment);
        os_set_function(os_make_symbol("SYMBOLP"), os_make_native_function((lisp_addr_t)(void *)primitive_symbolp), global_environment);
        os_set_function(os_make_symbol("CONSP"), os_make_native_function((lisp_addr_t)(void *)primitive_consp), global_environment);
        os_set_function(os_make_symbol("ATOM"), os_make_native_function((lisp_addr_t)(void *)primitive_atom), global_environment);
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
        os_set_function(os_make_symbol("%%ZA-COMPILED-P"), os_make_native_function((lisp_addr_t)(void *)primitive_za_compiled_p), global_environment);
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
        os_set_function(os_make_symbol("ELT"), os_make_native_function((lisp_addr_t)(void *)primitive_elt), global_environment);
        os_set_function(os_make_symbol("SET-ELT"), os_make_native_function((lisp_addr_t)(void *)primitive_set_elt), global_environment);
        os_set_function(os_make_symbol("SUBSEQ"), os_make_native_function((lisp_addr_t)(void *)primitive_subseq), global_environment);
        os_set_function(os_make_symbol("%%MAKE-CLASS-RAW"), os_make_native_function((lisp_addr_t)(void *)primitive_make_class_raw), global_environment);
        os_set_function(os_make_symbol("%%MAKE-BUILTIN-CLASS-RAW"), os_make_native_function((lisp_addr_t)(void *)primitive_make_builtin_class_raw), global_environment);
        os_set_function(os_make_symbol("%%CLASS-NAME"), os_make_native_function((lisp_addr_t)(void *)primitive_class_name), global_environment);
        os_set_function(os_make_symbol("%%CLASS-SUPERS"), os_make_native_function((lisp_addr_t)(void *)primitive_class_supers), global_environment);
        os_set_function(os_make_symbol("%%CLASS-SLOTS"), os_make_native_function((lisp_addr_t)(void *)primitive_class_slots), global_environment);
        os_set_function(os_make_symbol("%%CLASSP"), os_make_native_function((lisp_addr_t)(void *)primitive_classp), global_environment);
        os_set_function(os_make_symbol("%%BUILTIN-CLASSP"), os_make_native_function((lisp_addr_t)(void *)primitive_builtin_classp), global_environment);
        os_set_function(os_make_symbol("%%STANDARD-CLASSP"), os_make_native_function((lisp_addr_t)(void *)primitive_standard_classp), global_environment);
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
lisp_val_t os_make_cons(lisp_val_t car, lisp_val_t cdr) {
    GC_PROTECT(car);
    GC_PROTECT(cdr);
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
    // シンボルNILは仕様上、空リストを表すnilセンチネル自身と同一のオブジェクトである
    // べき(Lisp1.5以来の伝統/ISLisp仕様の<null>クラスの二重継承と同様の考え方)。
    // 新規にTAG_SYMBOLを作ってしまうと、'nilやreaderが生成するNILシンボルがnil
    // センチネル(not/null/eq等がpointer比較する対象)と一致せず、(not 'nil)等が
    // 誤った結果を返す原因になるため、ここで吸収してnil自身を返す。
    if (strncmpignorecase("NIL", name, 3) == 0 && name[3] == '\0') {
        return nil;
    }

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
    GC_PROTECT(name_str);
    lisp_addr_t addr = os_alloc_bytes(32);
    lisp_val_t *sym = (lisp_val_t *)addr;
    sym[0] = name_str; // string へのポインタ
    sym[1] = nil;      // gensymフラグ(nil=通常のinterned symbol)
    sym[2] = nil;      // 未使用(予約)
    sym[3] = nil;      // 未使用(予約)
    lisp_val_t tagged = (lisp_val_t)(addr | TAG_SYMBOL);

    if (g_symbol_count >= MAX_SYMBOLS) {
        // g_symbol_tableに登録できないままtaggedを返すと、以後同名のsymbolをinternする
        // 度に別オブジェクトが生成され続け、eq比較が壊れる(内部で検出しづらい)ため、
        // os_alloc_bytesのメモリ枯渇時と同様に即時停止する。
        frame_buffer *fb = get_active_frame_buffer();
        fb->write_string(fb, "symbol table exhausted...");
        for (;;) {
        }
    }
    g_symbol_table[g_symbol_count++] = tagged;

    return tagged;
}


/**
 * name(大文字化される)の新しいsymbolを、名前の重複チェックもg_symbol_tableへの
 * 登録もせずに作る。gensymが作るsymbolはeqによるアドレス比較でのみ使われ、
 * 名前から再度探し出す必要が無いため、g_symbol_tableに恒久的に残す理由がない
 * (登録してしまうと、ガベージコレクションの無いこの実装ではMAX_SYMBOLSの上限を
 * 際限なく消費してしまう)。word1に非nilのgensymフラグを立てることで、
 * os_make_symbolが作る通常のinterned symbolと区別できるようにしておく。
 * @param name symbol名
 * @return タグ付けされたSYMBOL(g_symbol_tableには登録されない)
 */
lisp_val_t os_make_uninterned_symbol(const char *name) {
    lisp_val_t name_str = os_make_string_for(name, 1 /* uppercase */);
    GC_PROTECT(name_str);
    lisp_addr_t addr = os_alloc_bytes(32);
    lisp_val_t *sym = (lisp_val_t *)addr;
    sym[0] = name_str;          // string へのポインタ
    sym[1] = os_make_fixnum(1); // gensymフラグ(非nil)
    sym[2] = nil;                // 未使用(予約)
    sym[3] = nil;                // 未使用(予約)
    return (lisp_val_t)(addr | TAG_SYMBOL);
}


/**
 * symがos_make_uninterned_symbol(gensym)で作られたsymbolかどうかを判定する。
 * @param sym 判定するSYMBOL
 * @return gensym由来なら0以外、通常のinterned symbolなら0
 */
int os_symbol_is_gensym(lisp_val_t sym) {
    lisp_addr_t addr = sym & ~TAG_MASK;
    return ((lisp_val_t *)addr)[1] != nil;
}


/**
 * g_symbol_tableに現在登録済みのinterned symbol数を返す(テスト・診断用)。
 * os_make_uninterned_symbol(gensym)が作るsymbolはここに含まれない。
 * @return 登録済みsymbol数
 */
int os_symbol_table_count(void) {
    return g_symbol_count;
}

/**
 * g_symbol_table・global_environment・g_dynamic_bindings・追加GC rootを初期状態に戻す
 * (テスト専用)。本体はos_heap_init/os_bootstrapを起動時に1回しか呼ばないため
 * 不要だが、1プロセス内でヒープを何度も再確保して起動し直すユニットテストでは、
 * これらのstatic変数がヒープの再確保をまたいで残ってしまう。特にg_symbol_tableが
 * 残っていると、os_make_symbolの重複チェックが以前の(既に破棄された)ヒープ上の
 * symbolをそのまま返してしまい、新しいヒープのFrom/To空間の外を指すstaleな参照が
 * GCのルートに混入する。os_heap_initの直後に呼ぶことで、各テストが真に独立した
 * クリーンな状態からブートストラップできるようにする。
 */
void os_reset_runtime_state_for_test(void) {
    g_symbol_count = 0;
    global_environment = nil;
    g_dynamic_bindings = nil;
    g_gc_extra_root_count = 0;
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
    // w1/w2/w3はUINT64だが、呼び出し元によっては実体がタグ付きのlisp_val_t(生存中の
    // ヒープオブジェクトへの参照)であることがある。os_alloc_bytesがOOM時にos_gc_collect
    // を発火させうるため、GCで再配置されても追随できるよう確保前にGC_PROTECTする
    GC_PROTECT(w1);
    GC_PROTECT(w2);
    GC_PROTECT(w3);
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
    GC_PROTECT(env_symbol);
    GC_PROTECT(parent_env);


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
    GC_PROTECT(name_symbol);
    lisp_val_t variables_symbol = os_make_symbol("variables");
    GC_PROTECT(variables_symbol);
    lisp_val_t functions_symbol = os_make_symbol("functions");
    GC_PROTECT(functions_symbol);
    lisp_val_t parent_symbol = os_make_symbol("parent");
    GC_PROTECT(parent_symbol);
    lisp_val_t constants_symbol = os_make_symbol("constants");
    GC_PROTECT(constants_symbol);

    lisp_val_t name_slot = os_make_cons(name_symbol, env_symbol);
    GC_PROTECT(name_slot);
    lisp_val_t variables_slot = os_make_cons(variables_symbol, nil);
    GC_PROTECT(variables_slot);
    lisp_val_t functions_slot = os_make_cons(functions_symbol, nil);
    GC_PROTECT(functions_slot);
    lisp_val_t parent_slot = os_make_cons(parent_symbol, parent_env);
    GC_PROTECT(parent_slot);
    lisp_val_t constants_slot = os_make_cons(constants_symbol, nil);

    lisp_val_t list_step4 = os_make_cons(constants_slot, nil);
    lisp_val_t list_step3 = os_make_cons(parent_slot, list_step4);
    lisp_val_t list_step2 = os_make_cons(functions_slot, list_step3);
    lisp_val_t list_step1 = os_make_cons(variables_slot, list_step2);
    lisp_val_t env_obj = os_make_cons(name_slot, list_step1);

    return env_obj;
}

/**
 * symがenvまたはその親のいずれかのconstantsスロットに登録されているかどうかを判定する。
 * os_setq_variableが親を辿って書き込み先を探すため、ネストしたクロージャ内からの
 * setqが外側スコープのdefconstant定数を素通りして書き換えてしまわないよう、
 * os_get_variable/os_setq_variableと同じ親チェーン探索にしている。
 * @param sym 判定するsymbol
 * @param env 判定を開始する環境
 * @return 定数として登録されていればnon-zero
 */
int os_is_constant(lisp_val_t sym, lisp_val_t env) {
    lisp_val_t current_env = env;

    while (current_env != nil) {
        lisp_val_t const_slot = cc_car(cc_cdr(cc_cdr(cc_cdr(cc_cdr(current_env)))));
        lisp_val_t alist = cc_cdr(const_slot);

        if (cc_assoc_eq(sym, alist) != nil) {
            return 1;
        }

        lisp_val_t cell1 = cc_cdr(current_env);
        lisp_val_t cell2 = cc_cdr(cell1);
        lisp_val_t cell3 = cc_cdr(cell2);
        lisp_val_t par_slot = cc_car(cell3);

        current_env = cc_cdr(par_slot);
    }

    return 0;
}

/**
 * envのconstantsスロットにsymを定数として登録する(既に登録済みなら何もしない)。
 * @param sym 登録するsymbol
 * @param env 登録先の環境
 */
void os_mark_constant(lisp_val_t sym, lisp_val_t env) {
    lisp_val_t const_slot = cc_car(cc_cdr(cc_cdr(cc_cdr(cc_cdr(env)))));
    GC_PROTECT(const_slot);
    lisp_val_t alist = cc_cdr(const_slot);
    GC_PROTECT(alist);

    if (cc_assoc_eq(sym, alist) != nil) {
        return;
    }
    lisp_val_t new_pair = os_make_cons(sym, g_sym_t);
    lisp_val_t new_alist = os_make_cons(new_pair, alist);
    // 上記2回のos_make_cons双方でGCが発火しconst_slotが移動している可能性があるため、
    // 書き込み先アドレスはconst_slot(GC_PROTECT済み)から、両方の確保が完了した今
    // 最後に読み直す(先に読んでしまうと2回目のos_make_consのGCで書き込み先が
    // 古いfrom-space上のアドレスのまま stale になり、登録が失われる)
    lisp_addr_t const_slot_addr = const_slot & ~TAG_MASK;
    ((lisp_val_t *)const_slot_addr)[1] = new_alist;
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
 * @return MAGIC_FUNCTION_NATIVEのINSTANCE(word2=NIL、組み込みprimitive扱い)
 */
lisp_val_t os_make_native_function(UINT64 fnptr) {
    return os_make_instance(MAGIC_FUNCTION_NATIVE, fnptr, nil, nil);
}

/**
 * fnptrをza.cがコンパイルしたネイティブ(C)関数として呼び出すTAG_INSTANCEオブジェクトを作る。
 * os_make_native_functionとの違いはword2にfixnum 1を立てる点のみで、印字時にコンパイル済みと
 * 組み込みprimitiveを区別するために使う。
 * @param fnptr 呼び出すJITコンパイル済み機械語のアドレス
 * @return MAGIC_FUNCTION_NATIVEのINSTANCE(word2=fixnum 1)
 */
lisp_val_t os_make_jit_function(UINT64 fnptr) {
    return os_make_instance(MAGIC_FUNCTION_NATIVE, fnptr, os_make_fixnum(1), nil);
}

lisp_val_t os_signal_condition(lisp_val_t class_sym, lisp_val_t initargs, lisp_val_t env) {
    GC_PROTECT(env);
    lisp_val_t make_instance_fn = os_get_function(g_sym_make_instance, env);
    lisp_val_t signal_condition_fn = os_get_function(g_sym_signal_condition, env);
    GC_PROTECT(signal_condition_fn);
    if (make_instance_fn == nil || signal_condition_fn == nil) {
        // init.lisp未ロード(make-instance/signal-conditionが未定義)時のフォールバック
        return g_sym_eval_error;
    }

    lisp_val_t condition = os_apply_function(make_instance_fn, os_make_cons(class_sym, initargs), env);
    if (os_is_control_transfer(condition)) {
        return condition;
    }

    lisp_val_t signal_args = os_make_cons(condition, os_make_cons(nil, nil));
    return os_apply_function(signal_condition_fn, signal_args, env);
}

lisp_val_t os_resolve_class(lisp_val_t class_name_sym, lisp_val_t env) {
    lisp_val_t find_class_fn = os_get_function(g_sym_percent_find_class, env);
    if (find_class_fn == nil) {
        return g_sym_eval_error;
    }
    return os_apply_function(find_class_fn, os_make_cons(class_name_sym, nil), env);
}

/**
 * offending_objectを<domain-error>(:object offending-object :expected-class (%find-class '<number>))
 * としてsignalする。sqrt/logのように「型は合っているが値が定義域外」の場合に使う。
 * @param offending_object domain-errorの原因になった値
 * @param env 呼び出し時の環境
 * @return signal-conditionの戻り値(通常はハンドラ経由でトップレベルへabortするため到達しない)。
 *         init.lisp未ロードの場合はg_sym_eval_error
 */
static lisp_val_t signal_domain_error(lisp_val_t offending_object, lisp_val_t env) {
    GC_PROTECT(offending_object);
    GC_PROTECT(env);
    lisp_val_t number_class = os_resolve_class(g_sym_class_number, env);
    if (number_class == g_sym_eval_error || os_is_control_transfer(number_class)) {
        return number_class;
    }

    lisp_val_t initargs = os_make_cons(g_sym_kw_object, os_make_cons(offending_object,
        os_make_cons(g_sym_kw_expected_class, os_make_cons(number_class, nil))));
    return os_signal_condition(g_sym_class_domain_error, initargs, env);
}


/**
 * envの変数slotにsymの値としてvalを設定する(既存なら破壊的に上書き、無ければ新規追加)。
 * @param sym 設定するsymbol
 * @param val 設定する値
 * @param env 設定先の環境
 * @return val 自身
 */
lisp_val_t os_set_variable(lisp_val_t sym, lisp_val_t val, lisp_val_t env) {
    // 新規追加パスではos_make_cons後にvalをreturnで読み直すため保護する
    GC_PROTECT(val);
    // current environment の (variables . alist) のペアを取り出す(cadr)
    lisp_val_t var_slot = cc_car(cc_cdr(env));
    GC_PROTECT(var_slot);

    lisp_val_t alist = cc_cdr(var_slot); // cdr (alist)
    GC_PROTECT(alist);
    lisp_val_t existing_pair = cc_assoc_eq(sym, alist);

    if (existing_pair != nil) {
        // すでに存在する場合は cdr を破壊的に書き換える
        ((lisp_val_t *)(existing_pair & ~TAG_MASK))[1] = val;
    } else {
        // 新規追加
        lisp_val_t new_pair = os_make_cons(sym, val);
        // (push new-pair alist)
        lisp_val_t new_alist = os_make_cons(new_pair, alist);
        // 上記2回のos_make_cons双方でGCが発火しvar_slotが移動している可能性があるため、
        // 書き込み先アドレスはvar_slot(GC_PROTECT済み)から、両方の確保が完了した今
        // 最後に読み直す(先に読んでしまうと2回目のos_make_consのGCで書き込み先が
        // 古いfrom-space上のアドレスのまま stale になり、束縛の追加が失われる)
        lisp_addr_t var_slot_addr = var_slot & ~TAG_MASK;
        ((lisp_val_t *)var_slot_addr)[1] = new_alist;
    }

    return val;
}


/**
 * envから親を順に辿り、既存のsym変数束縛を探して見つかったframeで破壊的に上書きする(setq用)。
 * os_set_variableがcurrent frameだけを見て「新規に束縛を定義する」のに対し、setqは
 * レキシカルスコープ上に見えている既存の変数を更新する必要があるため、os_get_variableと
 * 同じ親チェーン探索でどのframeに実体があるかを見つけてから書き換える。
 * どのframeにも見つからない場合(未束縛変数へのsetq)は、os_set_variableと同じく
 * envにローカル新規追加する。
 * @param sym 設定するsymbol
 * @param val 設定する値
 * @param env 探索を開始する環境
 * @return val 自身
 */
lisp_val_t os_setq_variable(lisp_val_t sym, lisp_val_t val, lisp_val_t env) {
    lisp_val_t current_env = env;

    while (current_env != nil) {
        lisp_val_t va_slot = cc_car(cc_cdr(current_env));
        lisp_val_t alist = cc_cdr(va_slot);
        lisp_val_t pair = cc_assoc_eq(sym, alist);

        if (pair != nil) {
            ((lisp_val_t *)(pair & ~TAG_MASK))[1] = val;
            return val;
        }

        lisp_val_t cell1 = cc_cdr(current_env);
        lisp_val_t cell2 = cc_cdr(cell1);
        lisp_val_t cell3 = cc_cdr(cell2);
        lisp_val_t par_slot = cc_car(cell3);

        current_env = cc_cdr(par_slot);
    }

    return os_set_variable(sym, val, env);
}


/**
 * envの関数slotにsymの関数定義としてfn_objを設定する(既存なら破壊的に上書き、無ければ新規追加)。
 * @param sym 設定するsymbol
 * @param fn_obj 設定する関数オブジェクト
 * @param env 設定先の環境
 * @return fn_obj 自身
 */
lisp_val_t os_set_function(lisp_val_t sym, lisp_val_t fn_obj, lisp_val_t env) {
    // 新規追加パスではos_make_cons後にfn_objをreturnで読み直すため保護する
    GC_PROTECT(fn_obj);
    // current environment から (functions . alist) のペアを取り出すため、 cddr を取る
    lisp_val_t next_cell = cc_cdr(cc_cdr(env));

    // (functions . alist) のペアを取り出す
    lisp_val_t func_slot = cc_car(next_cell);
    GC_PROTECT(func_slot);

    lisp_val_t alist = cc_cdr(func_slot); // cdr (alist)
    GC_PROTECT(alist);
    lisp_val_t existing_pair = cc_assoc_eq(sym, alist);

    if (existing_pair != nil) {
        // すでに存在する場合は cdr を破壊的に書き換える
        ((lisp_val_t *)(existing_pair & ~TAG_MASK))[1] = fn_obj;
    } else {
        // 新規追加
        lisp_val_t new_pair = os_make_cons(sym, fn_obj);
        // (push new-pair alist)
        lisp_val_t new_alist = os_make_cons(new_pair, alist);
        // 上記2回のos_make_cons双方でGCが発火しfunc_slotが移動している可能性があるため、
        // 書き込み先アドレスはfunc_slot(GC_PROTECT済み)から、両方の確保が完了した今
        // 最後に読み直す(先に読んでしまうと2回目のos_make_consのGCで書き込み先が
        // 古いfrom-space上のアドレスのまま stale になり、束縛の追加が失われる)
        lisp_addr_t func_slot_addr = func_slot & ~TAG_MASK;
        ((lisp_val_t *)func_slot_addr)[1] = new_alist;
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

    // limbsは呼び出し元が管理する生バッファで、GCのルートとして追跡されない。
    // この後アロケーションを2回以上挟むとGCがfrom/to空間を2回フリップし得て、
    // 2回目のフリップで元のfrom空間(=limbsの実体があった領域)が新たなto空間として
    // 再利用され、まだ読んでいないlimbsの内容が上書きされる危険がある。そのため、
    // limbsを読む最後の操作(このコピー)を、limbs確保後最初のアロケーション
    // (limb配列自体の確保)の直後、他のアロケーションを一切挟まずに完了させる
    lisp_addr_t limb_addr = os_alloc_bytes(8 * count);
    UINT64 *dst = (UINT64 *)limb_addr;
    for (UINT64 i = 0; i < count; i++) {
        dst[i] = limbs[i];
    }

    // ここから先はlimbsを二度と読まないため、以降で何回アロケーションが発生しても安全。
    // 書き込みはword3(limb配列アドレス)→word2(count)の順に行うことで、
    // 「countだけ確定してaddrが未確定」という危険な中間状態を作らない
    lisp_val_t bignum = os_make_instance(MAGIC_BIGNUM, (UINT64)sign, 0, 0);
    GC_PROTECT(bignum);
    UINT64 *words = (UINT64 *)(bignum & ~TAG_MASK);
    words[3] = (UINT64)limb_addr;
    words[2] = count;
    return bignum;
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

/** valがfloat(MAGIC_FLOATのINSTANCE)かどうかを判定する */
static int is_float(lisp_val_t val) {
    return (val & TAG_MASK) == TAG_INSTANCE && ((UINT64 *)(val & ~TAG_MASK))[0] == MAGIC_FLOAT;
}

/** valがbignum(MAGIC_BIGNUMのINSTANCE)かどうかを判定する */
static int is_bignum(lisp_val_t val) {
    return (val & TAG_MASK) == TAG_INSTANCE && ((UINT64 *)(val & ~TAG_MASK))[0] == MAGIC_BIGNUM;
}

lisp_val_t os_make_float(double value) {
    union { double d; UINT64 u; } conv;
    conv.d = value;
    return os_make_instance(MAGIC_FLOAT, conv.u, 0, 0);
}

double os_float_value(lisp_val_t val) {
    union { double d; UINT64 u; } conv;
    conv.u = ((UINT64 *)(val & ~TAG_MASK))[1];
    return conv.d;
}

double bignum_to_double(lisp_val_t val) {
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    int sign = (int)obj[1];
    UINT64 count = obj[2];
    UINT64 *limbs = (UINT64 *)obj[3];
    double result = 0.0;
    for (UINT64 i = count; i > 0; i--) {
        result = result * 4294967296.0 + (double)limbs[i - 1];
    }
    return sign ? -result : result;
}

/**
 * argsの中にfloat(MAGIC_FLOATのINSTANCE)が1つでも含まれるかどうかを判定する。
 * 四則演算プリミティブが整数専用の高速/一般パスとfloatパスのどちらを使うかを
 * 振り分けるために使う。
 */
static int any_float(lisp_val_t args) {
    for (lisp_val_t cur = args; cur != nil; cur = cc_cdr(cur)) {
        if (is_float(cc_car(cur))) {
            return 1;
        }
    }
    return 0;
}

/**
 * 数値(FIXNUM/bignum/float)をdoubleへ変換する。float同士の演算・比較の前に
 * オペランドをdoubleへ揃えるために使う。
 */
static double to_double(lisp_val_t v) {
    if (is_float(v)) {
        return os_float_value(v);
    }
    if (is_bignum(v)) {
        return bignum_to_double(v);
    }
    double mag = (double)os_fixnum_magnitude(v);
    return os_fixnum_is_negative(v) ? -mag : mag;
}

/**
 * 2つの整数(FIXNUM/bignum)の大小を比較する。両方FIXNUMの場合はヒープ確保なしの
 * 高速パスを使う。いずれかがfloatの場合は両方をdoubleへ変換して比較する。
 * @return a<bなら負、a==bなら0、a>bなら正
 */
static int number_compare(lisp_val_t a, lisp_val_t b) {
    if (is_float(a) || is_float(b)) {
        double da = to_double(a);
        double db = to_double(b);
        return da < db ? -1 : (da > db ? 1 : 0);
    }

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
    GC_PROTECT(z1);
    GC_PROTECT(z2);
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
    // os_alloc_bytesを2回挟んだのでm1/m2のlimbsを再取得してから使う
    decompose(z1, &m1);
    decompose(z2, &m2);
    UINT64 quot_len, rem_len;
    mag_divmod(m1.limbs, m1.count, m2.limbs, m2.count, quot_buf, &quot_len, rem_buf, &rem_len);
    int m1_sign = m1.sign;
    int m2_sign = m2.sign;

    if (rem_len == 1 && rem_buf[0] == 0) {
        // 割り切れる: mod=0、divの符号はオペランドの符号のXOR(quot_bufはmag_divmod直後で未確保区間なので安全)
        int div_sign = (m1_sign != m2_sign);
        *div_out = os_make_integer(div_sign, quot_buf, quot_len);
        *mod_out = os_make_fixnum(0);
        return;
    }

    // quot_buf/rem_bufは以降のアロケーションを挟むと追従しない生バッファなので、
    // 直後にMAGIC_BIGNUMへ包んでGC_PROTECTし、使う直前にdecomposeで取り直す
    lisp_val_t quot_wrapped = os_make_integer(0, quot_buf, quot_len);
    GC_PROTECT(quot_wrapped);
    lisp_val_t rem_wrapped = os_make_integer(0, rem_buf, rem_len);
    GC_PROTECT(rem_wrapped);

    if (m1_sign == m2_sign) {
        // 同符号: truncate除算とfloor除算が一致する
        signed_mag_t mq, mr;
        decompose(quot_wrapped, &mq);
        *div_out = os_make_integer(0, mq.limbs, mq.count);
        decompose(rem_wrapped, &mr);
        *mod_out = os_make_integer(m2_sign, mr.limbs, mr.count);
        return;
    }

    // 異符号: div = -(q_trunc+1)、mod = |z2| - r_trunc (符号はz2に一致)
    UINT64 one[1] = {1};
    signed_mag_t mq;
    decompose(quot_wrapped, &mq);
    UINT64 *div_mag_buf = (UINT64 *)os_alloc_bytes(8 * (mq.count + 1));
    decompose(quot_wrapped, &mq);
    UINT64 div_mag_len = mag_add(mq.limbs, mq.count, one, 1, div_mag_buf);
    lisp_val_t div_result = os_make_integer(1, div_mag_buf, div_mag_len);
    GC_PROTECT(div_result);

    decompose(z2, &m2);
    UINT64 *mod_mag_buf = (UINT64 *)os_alloc_bytes(8 * m2.count);
    decompose(z2, &m2);
    signed_mag_t mr;
    decompose(rem_wrapped, &mr);
    UINT64 mod_mag_len = mag_sub(m2.limbs, m2.count, mr.limbs, mr.count, mod_mag_buf);
    *mod_out = os_make_integer(m2_sign, mod_mag_buf, mod_mag_len);
    *div_out = div_result;
}

/**
 * aとbの最大公約数のマグニチュードを求める(符号なし、ユークリッドの互除法。
 * mag_divmodの繰り返しで実装する)。gcd(0,0)=0、gcd(a,0)=aとなる。
 * @param out_limbs 結果のlimb配列(新規にヒープ確保したもの)の格納先
 * @param out_len 結果の実効長の格納先
 */
static lisp_val_t mag_gcd(lisp_val_t a_val, lisp_val_t b_val) {
    // cur_a/cur_bはイテレーションを跨いで生き続ける必要があるため、生バッファのまま
    // 保持せず、確保直後にMAGIC_BIGNUMへ包んでGC_PROTECTし、使う直前にdecomposeで取り直す
    GC_PROTECT(a_val);
    GC_PROTECT(b_val);
    signed_mag_t ma, mb;
    decompose(a_val, &ma);
    decompose(b_val, &mb);

    UINT64 *cur_a_buf = (UINT64 *)os_alloc_bytes(8 * ma.count);
    decompose(a_val, &ma);
    for (UINT64 i = 0; i < ma.count; i++) {
        cur_a_buf[i] = ma.limbs[i];
    }
    lisp_val_t cur_a = os_make_integer(0, cur_a_buf, ma.count);
    GC_PROTECT(cur_a);

    decompose(b_val, &mb);
    UINT64 *cur_b_buf = (UINT64 *)os_alloc_bytes(8 * mb.count);
    decompose(b_val, &mb);
    for (UINT64 i = 0; i < mb.count; i++) {
        cur_b_buf[i] = mb.limbs[i];
    }
    lisp_val_t cur_b = os_make_integer(0, cur_b_buf, mb.count);
    GC_PROTECT(cur_b);

    signed_mag_t mcb;
    decompose(cur_b, &mcb);
    while (!(mcb.count == 1 && mcb.limbs[0] == 0)) {
        signed_mag_t mca;
        decompose(cur_a, &mca);
        decompose(cur_b, &mcb);
        UINT64 *quot_buf = (UINT64 *)os_alloc_bytes(8 * mca.count);
        UINT64 *rem_buf = (UINT64 *)os_alloc_bytes(8 * mca.count);
        decompose(cur_a, &mca);
        decompose(cur_b, &mcb);
        UINT64 quot_len, rem_len;
        mag_divmod(mca.limbs, mca.count, mcb.limbs, mcb.count, quot_buf, &quot_len, rem_buf, &rem_len);

        cur_a = cur_b;
        cur_b = os_make_integer(0, rem_buf, rem_len);
        decompose(cur_b, &mcb);
    }

    return cur_a;
}

/**
 * nのマグニチュードの整数平方根floor(sqrt(n))を求める(符号なし、ニュートン法。
 * x=n, y=(x+1)/2 から始め、y<xである限りx=y, y=(x+n/x)/2を繰り返す)。
 * mag_add/mag_divmod/mag_divmod_small/mag_compareのみで実装し、bignumでも正しく動作する。
 * @param out_limbs 結果のlimb配列(新規にヒープ確保したもの)の格納先
 * @param out_len 結果の実効長の格納先
 */
static lisp_val_t mag_isqrt(lisp_val_t n_val) {
    // x/yはイテレーションを跨いで生き続ける必要があるため、生バッファのまま保持せず、
    // 確保直後にMAGIC_BIGNUMへ包んでGC_PROTECTし、使う直前にdecomposeで取り直す
    GC_PROTECT(n_val);
    signed_mag_t mn;
    decompose(n_val, &mn);
    UINT64 nlen = mn.count;

    UINT64 *x_buf = (UINT64 *)os_alloc_bytes(8 * nlen);
    decompose(n_val, &mn);
    for (UINT64 i = 0; i < nlen; i++) {
        x_buf[i] = mn.limbs[i];
    }
    lisp_val_t x = os_make_integer(0, x_buf, nlen);
    GC_PROTECT(x);

    UINT64 one[1] = {1};
    UINT64 dummy_rem;

    signed_mag_t mx;
    decompose(x, &mx);
    UINT64 *xp1_buf = (UINT64 *)os_alloc_bytes(8 * (mx.count + 1));
    decompose(x, &mx);
    UINT64 xp1_len = mag_add(mx.limbs, mx.count, one, 1, xp1_buf);
    UINT64 ylen = mag_divmod_small(xp1_buf, xp1_len, 2, &dummy_rem);
    lisp_val_t y = os_make_integer(0, xp1_buf, ylen);
    GC_PROTECT(y);

    signed_mag_t my_mag, mx_mag;
    decompose(y, &my_mag);
    decompose(x, &mx_mag);
    while (mag_compare(my_mag.limbs, my_mag.count, mx_mag.limbs, mx_mag.count) < 0) {
        x = y;

        decompose(n_val, &mn);
        decompose(x, &mx_mag);
        UINT64 *quot_buf = (UINT64 *)os_alloc_bytes(8 * nlen);
        UINT64 *rem_buf = (UINT64 *)os_alloc_bytes(8 * nlen);
        decompose(n_val, &mn);
        decompose(x, &mx_mag);
        UINT64 quot_len, rem_len;
        mag_divmod(mn.limbs, mn.count, mx_mag.limbs, mx_mag.count, quot_buf, &quot_len, rem_buf, &rem_len);

        decompose(x, &mx_mag);
        UINT64 cap = (mx_mag.count > quot_len ? mx_mag.count : quot_len) + 1;
        UINT64 *sum_buf = (UINT64 *)os_alloc_bytes(8 * cap);
        decompose(x, &mx_mag);
        UINT64 sum_len = mag_add(mx_mag.limbs, mx_mag.count, quot_buf, quot_len, sum_buf);

        ylen = mag_divmod_small(sum_buf, sum_len, 2, &dummy_rem);
        y = os_make_integer(0, sum_buf, ylen);

        decompose(y, &my_mag);
        decompose(x, &mx_mag);
    }

    return x;
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
 * 組み込み関数+。argsの全数値(FIXNUM/bignum/float、負数も可)を合計する。
 * floatが1つでも含まれる場合は全オペランドをdoubleへ変換して合計する。
 * それ以外で全オペランドが非負FIXNUMかつ桁あふれの恐れがない場合はヒープ確保なしの
 * 高速パスを使い、それ以外(負数・bignumが絡む、桁あふれの恐れがある)は符号付き
 * マグニチュードによる一般パスにフォールバックする。
 * @param args 評価済みの引数リスト(すべて数値)
 * @param env 呼び出し時の環境(未使用)
 * @return 合計値の数値(floatが絡まなければ60bit以内ならFIXNUM、それを超えるならbignum)
 */
lisp_val_t primitive_add(lisp_val_t args, lisp_val_t env) {
    if (any_float(args)) {
        double sum = 0.0;
        for (lisp_val_t cur = args; cur != nil; cur = cc_cdr(cur)) {
            sum += to_double(cc_car(cur));
        }
        return os_make_float(sum);
    }

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

    // curはループ内でos_alloc_bytesを呼ぶため、イテレーションを跨いで
    // アドレスをGCに追跡させる必要がある
    lisp_val_t cur = args;
    GC_PROTECT(cur);

    // accはイテレーションを跨いで生き続ける生バッファを持たせず、確保直後に
    // MAGIC_BIGNUMへ包んでGC_PROTECTし、使う直前にdecomposeで取り直す
    lisp_val_t acc_val = os_make_fixnum(0);
    GC_PROTECT(acc_val);
    signed_mag_t acc;

    for (; cur != nil; cur = cc_cdr(cur)) {
        signed_mag_t operand;
        decompose(cc_car(cur), &operand);
        decompose(acc_val, &acc);

        UINT64 cap = (acc.count > operand.count ? acc.count : operand.count) + 1;
        UINT64 *result = (UINT64 *)os_alloc_bytes(8 * cap);
        decompose(cc_car(cur), &operand);
        decompose(acc_val, &acc);
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

        acc_val = os_make_integer(result_sign, result, result_len);
    }

    return acc_val;
}

/**
 * primitive_addを2引数固定で呼ぶためのラッパー。JITコンパイルされたコードから
 * 呼ばれることを想定し、引数aとbは呼び出し直後にGC_PROTECTしてからconsリストへ
 * 組み立てるため、この呼び出し中にGCが走ってもa/bが失われることはない。
 * @param a 第一オペランド
 * @param b 第二オペランド
 * @return primitive_addと同じ規則で計算した合計値
 */
lisp_val_t primitive_add2(lisp_val_t a, lisp_val_t b) {
    GC_PROTECT(a);
    GC_PROTECT(b);
    lisp_val_t args = os_make_cons(a, os_make_cons(b, nil));
    return primitive_add(args, global_environment);
}

/**
 * primitive_subtractを2引数固定で呼ぶためのラッパー。JITコンパイル済みコードから
 * 呼ばれる想定。primitive_add2と同様、aとbは呼び出し直後にGC_PROTECTする。
 * @param a 第一オペランド
 * @param b 第二オペランド
 * @return primitive_subtractと同じ規則で計算したa-b
 */
lisp_val_t primitive_subtract2(lisp_val_t a, lisp_val_t b) {
    GC_PROTECT(a);
    GC_PROTECT(b);
    lisp_val_t args = os_make_cons(a, os_make_cons(b, nil));
    return primitive_subtract(args, global_environment);
}

/**
 * 組み込み関数-。argsの第一引数から残りを順に減算する。1引数の場合は単項マイナス(0-x)として
 * 符号を反転する。floatが1つでも含まれる場合は全オペランドをdoubleへ変換して減算する。
 * それ以外で全オペランドが非負FIXNUMかつ結果が負にならない場合はヒープ確保なしの
 * 高速パスを使い、それ以外は符号付きマグニチュードによる一般パスにフォールバックする。
 * @param args 評価済みの引数リスト(すべて数値)
 * @param env 呼び出し時の環境(未使用)
 * @return 減算結果の数値(floatが絡まなければ60bit以内ならFIXNUM、それを超えるならbignum)
 */
lisp_val_t primitive_subtract(lisp_val_t args, lisp_val_t env) {
    lisp_val_t first = cc_car(args);

    if (any_float(args)) {
        double result = to_double(first);
        if (cc_cdr(args) == nil) {
            return os_make_float(-result);
        }
        for (lisp_val_t rest = cc_cdr(args); rest != nil; rest = cc_cdr(rest)) {
            result -= to_double(cc_car(rest));
        }
        return os_make_float(result);
    }

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

    // restはループ内でos_alloc_bytesを呼ぶため、イテレーションを跨いで
    // アドレスをGCに追跡させる必要がある
    lisp_val_t rest = cc_cdr(args);
    GC_PROTECT(rest);

    // accはイテレーションを跨いで生き続ける生バッファを持たせず、確保直後に
    // MAGIC_BIGNUMへ包んでGC_PROTECTし、使う直前にdecomposeで取り直す
    lisp_val_t acc_val = first;
    GC_PROTECT(acc_val);
    signed_mag_t acc;

    for (; rest != nil; rest = cc_cdr(rest)) {
        signed_mag_t operand;
        decompose(cc_car(rest), &operand);
        decompose(acc_val, &acc);

        UINT64 cap = (acc.count > operand.count ? acc.count : operand.count) + 1;
        UINT64 *result_buf = (UINT64 *)os_alloc_bytes(8 * cap);
        decompose(cc_car(rest), &operand);
        decompose(acc_val, &acc);
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

        acc_val = os_make_integer(result_sign, result_buf, result_len);
    }

    return acc_val;
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
 * primitive_eqを2引数固定・非allocatingで呼ぶためのラッパー。zaのJITコンパイル済み
 * コードから呼ばれることを想定する。primitive_add2等と異なり、eq自体は元から固定
 * 2引数のポインタ比較で確保を伴わないため、引数リストconsで包まずに直接比較する。
 * @param a 第一オペランド
 * @param b 第二オペランド
 * @return aとbが同一ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_eq2(lisp_val_t a, lisp_val_t b) {
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
 * primitive_nullを1引数固定・非allocatingで呼ぶためのラッパー。zaのJITコンパイル済み
 * コードから呼ばれることを想定する。
 * @param a オペランド
 * @return aがnilならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_null1(lisp_val_t a) {
    return a == nil ? g_sym_t : nil;
}

/**
 * 組み込み関数*。argsの全数値(FIXNUM/bignum/float、負数も可)を乗算する。
 * floatが1つでも含まれる場合は全オペランドをdoubleへ変換して乗算する。
 * それ以外で全オペランドが非負FIXNUMかつ桁あふれの恐れがない場合はヒープ確保なしの高速パスを使い、
 * それ以外は符号付きマグニチュードによる一般パス(素朴なO(n*m)乗算)にフォールバックする。
 * @param args 評価済みの引数リスト(すべて数値)
 * @param env 呼び出し時の環境(未使用)
 * @return 積の数値(floatが絡まなければ60bit以内ならFIXNUM、それを超えるならbignum)
 */
lisp_val_t primitive_multiply(lisp_val_t args, lisp_val_t env) {
    if (any_float(args)) {
        double product = 1.0;
        for (lisp_val_t cur = args; cur != nil; cur = cc_cdr(cur)) {
            product *= to_double(cc_car(cur));
        }
        return os_make_float(product);
    }

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

    // curはループ内でos_alloc_bytesを呼ぶため、イテレーションを跨いで
    // アドレスをGCに追跡させる必要がある
    lisp_val_t cur = args;
    GC_PROTECT(cur);

    // accはイテレーションを跨いで生き続ける生バッファを持たせず、確保直後に
    // MAGIC_BIGNUMへ包んでGC_PROTECTし、使う直前にdecomposeで取り直す
    lisp_val_t acc_val = os_make_fixnum(1);
    GC_PROTECT(acc_val);
    signed_mag_t acc;

    for (; cur != nil; cur = cc_cdr(cur)) {
        signed_mag_t operand;
        decompose(cc_car(cur), &operand);
        decompose(acc_val, &acc);

        UINT64 cap = acc.count + operand.count;
        UINT64 *result = (UINT64 *)os_alloc_bytes(8 * cap);
        decompose(cc_car(cur), &operand);
        decompose(acc_val, &acc);
        UINT64 result_len = mag_mul(acc.limbs, acc.count, operand.limbs, operand.count, result);

        int result_sign = (acc.sign != operand.sign);
        acc_val = os_make_integer(result_sign, result, result_len);
    }

    return acc_val;
}

/**
 * primitive_multiplyを2引数固定で呼ぶためのラッパー。JITコンパイル済みコードから
 * 呼ばれる想定。primitive_add2と同様、aとbは呼び出し直後にGC_PROTECTする。
 * @param a 第一オペランド
 * @param b 第二オペランド
 * @return primitive_multiplyと同じ規則で計算したa*b
 */
lisp_val_t primitive_multiply2(lisp_val_t a, lisp_val_t b) {
    GC_PROTECT(a);
    GC_PROTECT(b);
    lisp_val_t args = os_make_cons(a, os_make_cons(b, nil));
    return primitive_multiply(args, global_environment);
}

/**
 * 組み込み関数/。argsの第一引数から残りを順に除算する(整数除算、商のみ返す)。
 * floatが1つでも含まれる場合は全オペランドをdoubleへ変換して除算する
 * (0除算はIEEE754の挙動どおり+inf/-inf/nanを返す。domain-error未実装のための簡略化)。
 * それ以外で全オペランドが非負FIXNUMの場合はヒープ確保なしの高速パスを使い、それ以外
 * (負数・bignumが絡む)は符号付きマグニチュードによる一般パス(1bitずつのシフト&サブトラクトに
 * よる長除算)にフォールバックする。商の符号は絶対値の商にオペランドの符号のXORを付与して決める。
 * @param args 評価済みの引数リスト(すべて数値)
 * @param env 呼び出し時の環境(未使用)
 * @return 除算結果の数値。floatが絡まず0除算の場合はg_sym_eval_error
 */
lisp_val_t primitive_divide(lisp_val_t args, lisp_val_t env) {
    lisp_val_t first = cc_car(args);

    if (any_float(args)) {
        double result = to_double(first);
        for (lisp_val_t rest = cc_cdr(args); rest != nil; rest = cc_cdr(rest)) {
            result /= to_double(cc_car(rest));
        }
        return os_make_float(result);
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

    // restはループ内でos_alloc_bytesを呼ぶため、イテレーションを跨いで
    // アドレスをGCに追跡させる必要がある
    lisp_val_t rest = cc_cdr(args);
    GC_PROTECT(rest);

    // accはイテレーションを跨いで生き続ける生バッファを持たせず、確保直後に
    // MAGIC_BIGNUMへ包んでGC_PROTECTし、使う直前にdecomposeで取り直す
    lisp_val_t acc_val = first;
    GC_PROTECT(acc_val);
    signed_mag_t acc;

    for (; rest != nil; rest = cc_cdr(rest)) {
        signed_mag_t operand;
        decompose(cc_car(rest), &operand);

        if (operand.count == 1 && operand.limbs[0] == 0) {
            return g_sym_eval_error;
        }

        decompose(acc_val, &acc);
        UINT64 *quot_buf = (UINT64 *)os_alloc_bytes(8 * acc.count);
        decompose(acc_val, &acc);
        UINT64 *rem_buf = (UINT64 *)os_alloc_bytes(8 * acc.count);
        decompose(cc_car(rest), &operand);
        decompose(acc_val, &acc);
        UINT64 quot_len, rem_len;
        mag_divmod(acc.limbs, acc.count, operand.limbs, operand.count, quot_buf, &quot_len, rem_buf, &rem_len);

        int result_sign = (acc.sign != operand.sign);
        acc_val = os_make_integer(result_sign, quot_buf, quot_len);
    }

    return acc_val;
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
 * primitive_less_thanを2引数固定で呼ぶためのラッパー。JITコンパイル済みコードから
 * 呼ばれる想定。primitive_add2と同様、aとbは呼び出し直後にGC_PROTECTする。
 * @param a 第一オペランド
 * @param b 第二オペランド
 * @return a<bならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_less_than2(lisp_val_t a, lisp_val_t b) {
    GC_PROTECT(a);
    GC_PROTECT(b);
    lisp_val_t args = os_make_cons(a, os_make_cons(b, nil));
    return primitive_less_than(args, global_environment);
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
 * primitive_num_equalを2引数固定で呼ぶためのラッパー。JITコンパイル済みコードから
 * 呼ばれる想定。primitive_add2と同様、aとbは呼び出し直後にGC_PROTECTする。
 * @param a 第一オペランド
 * @param b 第二オペランド
 * @return a=bならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_num_equal2(lisp_val_t a, lisp_val_t b) {
    GC_PROTECT(a);
    GC_PROTECT(b);
    lisp_val_t args = os_make_cons(a, os_make_cons(b, nil));
    return primitive_num_equal(args, global_environment);
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
    lisp_val_t z1 = cc_car(args);
    lisp_val_t z2 = cc_car(cc_cdr(args));
    return mag_gcd(z1, z2);
}

/**
 * 組み込み関数LCM。z1とz2の最小公倍数を返す(結果は常に非負。gcd*lcm=|z1*z2|の関係を
 * 使い、|z1*z2|/gcd(z1,z2)として求める。gcdが0(z1=z2=0の場合のみ)ならlcmも0)。
 * @param args 評価済みの引数リスト(整数2個)
 * @param env 呼び出し時の環境(未使用)
 * @return 最小公倍数(非負整数)
 */
lisp_val_t primitive_lcm(lisp_val_t args, lisp_val_t env) {
    lisp_val_t z1 = cc_car(args);
    lisp_val_t z2 = cc_car(cc_cdr(args));
    GC_PROTECT(z1);
    GC_PROTECT(z2);

    lisp_val_t gcd_val = mag_gcd(z1, z2);
    GC_PROTECT(gcd_val);

    signed_mag_t mg;
    decompose(gcd_val, &mg);
    if (mg.count == 1 && mg.limbs[0] == 0) {
        return os_make_fixnum(0);
    }

    signed_mag_t m1, m2;
    decompose(z1, &m1);
    decompose(z2, &m2);
    UINT64 *prod_buf = (UINT64 *)os_alloc_bytes(8 * (m1.count + m2.count));
    decompose(z1, &m1);
    decompose(z2, &m2);
    UINT64 prod_len = mag_mul(m1.limbs, m1.count, m2.limbs, m2.count, prod_buf);
    // prod_bufはこの後さらにアロケーションを挟むので確保直後に包んでGC_PROTECTする
    lisp_val_t prod_val = os_make_integer(0, prod_buf, prod_len);
    GC_PROTECT(prod_val);

    signed_mag_t mp;
    decompose(prod_val, &mp);
    decompose(gcd_val, &mg);
    UINT64 *quot_buf = (UINT64 *)os_alloc_bytes(8 * mp.count);
    UINT64 *rem_buf = (UINT64 *)os_alloc_bytes(8 * mp.count);
    decompose(prod_val, &mp);
    decompose(gcd_val, &mg);
    UINT64 quot_len, rem_len;
    mag_divmod(mp.limbs, mp.count, mg.limbs, mg.count, quot_buf, &quot_len, rem_buf, &rem_len);

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

    return mag_isqrt(val);
}

/**
 * doubleの平方根を返す。実機(ISIKIOS_UNIT_TEST未定義)ではSSE2のsqrtsd命令、
 * ネイティブユニットテストではlibmのsqrtを使う。
 * @param d 平方根を求めるdouble(非負であること)
 * @return sqrt(d)
 */
static double sqrt_fpu(double d) {
#ifndef ISIKIOS_UNIT_TEST
    double result;
    __asm__ __volatile__ ("sqrtsd %1, %0" : "=x"(result) : "x"(d));
    return result;
#else
    return sqrt(d);
#endif
}

/**
 * 組み込み関数SQRT。第一引数の平方根を返す。整数(FIXNUM/bignum)で完全平方数の場合は
 * mag_isqrtの結果をそのまま整数として返し(例: (sqrt 4) => 2)、それ以外はsqrt_fpuで
 * doubleの平方根を計算してfloatとして返す。負数はdomain-error
 * (「型は合っているが値が定義域外」、spec上sqrtは非負数のみを受け付ける)。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(signal_domain_error経由でinit.lispのsignal-conditionを呼ぶ)
 * @return 平方根。負数が渡された場合はsignal_domain_errorの戻り値
 */
lisp_val_t primitive_sqrt(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);

    if (is_float(val)) {
        double d = os_float_value(val);
        if (d < 0.0) {
            return signal_domain_error(val, env);
        }
        return os_make_float(sqrt_fpu(d));
    }

    GC_PROTECT(val);
    signed_mag_t m;
    decompose(val, &m);
    if (m.sign) {
        return signal_domain_error(val, env);
    }

    lisp_val_t root = mag_isqrt(val);
    GC_PROTECT(root);

    signed_mag_t mr;
    decompose(root, &mr);
    UINT64 *sq_buf = (UINT64 *)os_alloc_bytes(8 * mr.count * 2);
    decompose(root, &mr);
    UINT64 sq_len = mag_mul(mr.limbs, mr.count, mr.limbs, mr.count, sq_buf);

    decompose(val, &m);
    if (mag_compare(sq_buf, sq_len, m.limbs, m.count) == 0) {
        return root;
    }

    return os_make_float(sqrt_fpu(to_double(val)));
}

/**
 * xの自然対数を返す。実機ではx87のfyl2xでlog2(x)を求めln(2)倍して自然対数へ変換し、
 * ネイティブユニットテストではlibmのlogを使う。
 * @param x 対数を求めるdouble(正であること)
 * @return log(x)
 */
static double log_fpu(double x) {
#ifndef ISIKIOS_UNIT_TEST
    double log2x;
    __asm__ __volatile__ (
        "fld1\n"
        "fldl %1\n"
        "fyl2x\n"
        "fstpl %0\n"
        : "=m"(log2x)
        : "m"(x)
    );
    return log2x * 0.6931471805599453;
#else
    return log(x);
#endif
}

/**
 * 組み込み関数LOG。第一引数の自然対数を返す(log_fpu)。0以下はdomain-error
 * (「型は合っているが値が定義域外」、spec上logは正の数のみを受け付ける)。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(signal_domain_error経由でinit.lispのsignal-conditionを呼ぶ)
 * @return xの自然対数。0以下が渡された場合はsignal_domain_errorの戻り値
 */
lisp_val_t primitive_log(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    double x = to_double(val);
    if (x <= 0.0) {
        return signal_domain_error(val, env);
    }
    return os_make_float(log_fpu(x));
}

/**
 * 自然対数の底eのx乗を返す。実機ではx87のf2xm1は引数域[-1,1]でしか2^x-1を正しく
 * 計算できないため、y=x*log2(e)を最も近い整数n(frndint)と小数部f=y-n(|f|<=0.5)に
 * 分割し、2^f=f2xm1(f)+1をfscaleでn桁シフトしてexp(x)=2^y=2^f*2^nを求める。
 * ネイティブユニットテストではlibmのexpを使う。
 * @param x 指数
 * @return e^x
 */
static double exp_fpu(double x) {
#ifndef ISIKIOS_UNIT_TEST
    double y;
    __asm__ __volatile__ (
        "fldl %1\n"
        "fldl2e\n"
        "fmulp\n"
        "fstpl %0\n"
        : "=m"(y)
        : "m"(x)
    );

    double n;
    __asm__ __volatile__ (
        "fldl %1\n"
        "frndint\n"
        "fstpl %0\n"
        : "=m"(n)
        : "m"(y)
    );

    double f = y - n;

    double pow2f;
    __asm__ __volatile__ (
        "fldl %1\n"
        "f2xm1\n"
        "fld1\n"
        "faddp\n"
        "fstpl %0\n"
        : "=m"(pow2f)
        : "m"(f)
    );

    double result;
    __asm__ __volatile__ (
        "fldl %1\n"
        "fldl %2\n"
        "fscale\n"
        "fstpl %0\n"
        "fstp %%st(0)\n"
        : "=m"(result)
        : "m"(n), "m"(pow2f)
    );

    return result;
#else
    return exp(x);
#endif
}

/**
 * 組み込み関数EXP。第一引数を指数とする自然対数の底eの累乗を返す(exp_fpu)。
 * 定義域制約は無い(数値でなければ挙動は未定義)。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(未使用)
 * @return e^x
 */
lisp_val_t primitive_exp(lisp_val_t args, lisp_val_t env) {
    return os_make_float(exp_fpu(to_double(cc_car(args))));
}

/**
 * xの正弦(ラジアン)を返す。実機ではx87のfsin命令、ネイティブユニットテストでは
 * libmのsinを使う。fsinは引数が大きいほど周期還元の精度が落ちる既知の制限があり、
 * 大きな引数の精度検証は未対応(documents/isiki-os.mdの既存の注記を参照)。
 * @param x ラジアン
 * @return sin(x)
 */
static double sin_fpu(double x) {
#ifndef ISIKIOS_UNIT_TEST
    double result;
    __asm__ __volatile__ (
        "fldl %1\n"
        "fsin\n"
        "fstpl %0\n"
        : "=m"(result)
        : "m"(x)
    );
    return result;
#else
    return sin(x);
#endif
}

/**
 * 組み込み関数SIN。第一引数(ラジアン)の正弦を返す(sin_fpu)。定義域制約は無い。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(未使用)
 * @return sin(x)
 */
lisp_val_t primitive_sin(lisp_val_t args, lisp_val_t env) {
    return os_make_float(sin_fpu(to_double(cc_car(args))));
}

/**
 * xの余弦(ラジアン)を返す。実機ではx87のfcos命令、ネイティブユニットテストでは
 * libmのcosを使う。SINと同じく大きな引数の精度は未対応。
 * @param x ラジアン
 * @return cos(x)
 */
static double cos_fpu(double x) {
#ifndef ISIKIOS_UNIT_TEST
    double result;
    __asm__ __volatile__ (
        "fldl %1\n"
        "fcos\n"
        "fstpl %0\n"
        : "=m"(result)
        : "m"(x)
    );
    return result;
#else
    return cos(x);
#endif
}

/**
 * 組み込み関数COS。第一引数(ラジアン)の余弦を返す(cos_fpu)。定義域制約は無い。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(未使用)
 * @return cos(x)
 */
lisp_val_t primitive_cos(lisp_val_t args, lisp_val_t env) {
    return os_make_float(cos_fpu(to_double(cc_car(args))));
}

/**
 * atan(y/x)を、xの符号を使って正しい象限で返す。実機ではx87のfpatan
 * (ST(1):=atan(ST(1)/ST(0))を計算してpop)を使うため、先にy、次にxをpushする
 * (ST(0)=x, ST(1)=y)。ネイティブユニットテストではlibmのatan2を使う。
 * @param y 分子
 * @param x 分母
 * @return atan2(y, x)
 */
static double atan2_fpu(double y, double x) {
#ifndef ISIKIOS_UNIT_TEST
    double result;
    __asm__ __volatile__ (
        "fldl %2\n"
        "fldl %1\n"
        "fpatan\n"
        "fstpl %0\n"
        : "=m"(result)
        : "m"(x), "m"(y)
    );
    return result;
#else
    return atan2(y, x);
#endif
}

/**
 * 組み込み関数ATAN2。(atan2 y x) で atan(y/x) を、xの符号を使って正しい象限で
 * 返す(atan2_fpu)。定義域制約は無い。
 * @param args 評価済みの引数リスト(数値2個、第一引数がy、第二引数がx)
 * @param env 呼び出し時の環境(未使用)
 * @return atan2(y, x)
 */
lisp_val_t primitive_atan2(lisp_val_t args, lisp_val_t env) {
    double y = to_double(cc_car(args));
    double x = to_double(cc_car(cc_cdr(args)));
    return os_make_float(atan2_fpu(y, x));
}

/**
 * 整数値(小数部が無い)のdoubleを、IEEE754のsign/exponent/mantissaを分解して
 * fixnum/bignumへ変換する。round_via_x87(frndint)の結果を整数化するために使う。
 * 0.0/-0.0はどちらもFIXNUM 0とする。有限の整数値であることを前提とし(NaN/無限大は
 * 未対応)、significand(53bit、暗黙の先頭1bit込み)を2^shift倍する形で復元する
 * (shift>0は左シフト、mag_mul_small_add_smallによる2倍の繰り返しで実現。shift<=0は
 * 右シフトで、整数値である以上下位ビットは必ず0なので切り捨てなしに割り切れる)。
 * @param d 変換対象のdouble(整数値であること)
 * @return dと数値として等しいfixnum/bignum
 */
static lisp_val_t double_to_integer(double d) {
    if (d == 0.0) {
        return os_make_fixnum(0);
    }

    union { double d; UINT64 u; } conv;
    conv.d = d;
    UINT64 bits = conv.u;

    int sign = (int)(bits >> 63);
    UINT64 exp_field = (bits >> 52) & 0x7FFULL;
    UINT64 mantissa = bits & 0xFFFFFFFFFFFFFULL;
    UINT64 significand = mantissa | (1ULL << 52);
    int actual_exp = (int)exp_field - 1023;
    int shift = actual_exp - 52; // |d| == significand * 2^shift

    if (shift <= 0) {
        UINT64 val = significand >> (-shift);
        UINT64 limbs[2] = { val & 0xFFFFFFFFULL, val >> 32 };
        return os_make_integer(sign, limbs, 2);
    }

    UINT64 capacity = (UINT64)((64 + shift + 31) / 32) + 1;
    UINT64 *limbs = (UINT64 *)os_alloc_bytes(8 * capacity);
    limbs[0] = significand & 0xFFFFFFFFULL;
    limbs[1] = significand >> 32;
    for (UINT64 i = 2; i < capacity; i++) {
        limbs[i] = 0;
    }
    UINT64 len = 2;
    for (int i = 0; i < shift; i++) {
        len = mag_mul_small_add_small(limbs, len, 2, 0);
    }
    return os_make_integer(sign, limbs, len);
}

/**
 * x87のFPU制御ワードの丸めモードビット(RC、ビット[11:10])を一時的にrc_bitsへ
 * 変更した上でfrndintを実行し、制御ワードを元に戻す。rc_bits: 00=round-nearest
 * (ties-to-even)、01=round-down(floor)、10=round-up(ceiling)、11=truncate。
 * ネイティブユニットテストではlibmのfloor/ceil/trunc/nearbyint(デフォルトの
 * round-to-nearest-evenモード下)で同じ丸め規則を再現する。
 * @param x 丸め対象のdouble
 * @param rc_bits FPU制御ワードのRCフィールドに設定する2bit値
 * @return 丸めた結果のdouble(整数値)
 */
static double round_via_x87(double x, UINT16 rc_bits) {
#ifndef ISIKIOS_UNIT_TEST
    UINT16 old_cw;
    UINT16 new_cw;
    double result;

    __asm__ __volatile__ ("fnstcw %0" : "=m"(old_cw));
    new_cw = (UINT16)((old_cw & ~0x0C00) | (rc_bits << 10));
    __asm__ __volatile__ ("fldcw %0" : : "m"(new_cw));

    __asm__ __volatile__ (
        "fldl %1\n"
        "frndint\n"
        "fstpl %0\n"
        : "=m"(result)
        : "m"(x)
    );

    __asm__ __volatile__ ("fldcw %0" : : "m"(old_cw));

    return result;
#else
    switch (rc_bits) {
        case 0x1: return floor(x);
        case 0x2: return ceil(x);
        case 0x3: return trunc(x);
        default: return nearbyint(x);
    }
#endif
}

/**
 * 組み込み関数FLOOR。第一引数以下の最大の整数を返す(-∞方向の丸め)。すでに
 * FIXNUM/bignumならFPUを経由せずそのまま返す。定義域制約は無い。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(未使用)
 * @return floor(x)
 */
lisp_val_t primitive_floor(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if (!is_float(val)) {
        return val;
    }
    return double_to_integer(round_via_x87(os_float_value(val), 0x1));
}

/**
 * 組み込み関数CEILING。第一引数以上の最小の整数を返す(+∞方向の丸め)。すでに
 * FIXNUM/bignumならFPUを経由せずそのまま返す。定義域制約は無い。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(未使用)
 * @return ceiling(x)
 */
lisp_val_t primitive_ceiling(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if (!is_float(val)) {
        return val;
    }
    return double_to_integer(round_via_x87(os_float_value(val), 0x2));
}

/**
 * 組み込み関数TRUNCATE。第一引数の小数部を切り捨てた整数を返す(0方向の丸め)。
 * すでにFIXNUM/bignumならFPUを経由せずそのまま返す。定義域制約は無い。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(未使用)
 * @return truncate(x)
 */
lisp_val_t primitive_truncate(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if (!is_float(val)) {
        return val;
    }
    return double_to_integer(round_via_x87(os_float_value(val), 0x3));
}

/**
 * 組み込み関数ROUND。第一引数を最も近い整数に丸める(ties-to-even、spec通り
 * 中間値は偶数側)。すでにFIXNUM/bignumならFPUを経由せずそのまま返す。
 * 定義域制約は無い。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(未使用)
 * @return round(x)
 */
lisp_val_t primitive_round(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if (!is_float(val)) {
        return val;
    }
    return double_to_integer(round_via_x87(os_float_value(val), 0x0));
}

/**
 * 組み込み関数PARSE-NUMBER。第一引数のSTRINGを数値として読み取る(os_parse_number、reader.c)。
 * 文字列が数値の字句として解釈できない場合は<parse-error>をsignalする。
 * @param args 評価済みの引数リスト(第一引数はSTRING)
 * @param env 呼び出し時の環境(<parse-error>のsignal-conditionに使う)
 * @return 解析された数値
 */
lisp_val_t primitive_parse_number(lisp_val_t args, lisp_val_t env) {
    lisp_val_t str = cc_car(args);
    return os_parse_number(str, env);
}

/**
 * 組み込み関数NUMBERP。第一引数が数値(FIXNUM、bignum、float)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 数値ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_numberp(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) == TAG_FIXNUM) {
        return g_sym_t;
    }
    if ((val & TAG_MASK) == TAG_INSTANCE) {
        UINT64 magic = ((UINT64 *)(val & ~TAG_MASK))[0];
        if (magic == MAGIC_BIGNUM || magic == MAGIC_FLOAT) {
            return g_sym_t;
        }
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
 * 組み込み関数FLOATP。第一引数がfloat(MAGIC_FLOATのINSTANCE)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return floatならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_floatp(lisp_val_t args, lisp_val_t env) {
    return is_float(cc_car(args)) ? g_sym_t : nil;
}

/**
 * 組み込み関数FLOAT。第一引数を(既にfloatならそのまま、FIXNUM/bignumならdoubleへ変換して)floatとして返す。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(未使用)
 * @return floatに変換した値。数値以外が渡された場合はg_sym_eval_error
 */
lisp_val_t primitive_float(lisp_val_t args, lisp_val_t env) {
    lisp_val_t x = cc_car(args);
    if (is_float(x)) {
        return x;
    }
    if ((x & TAG_MASK) == TAG_FIXNUM || is_bignum(x)) {
        return os_make_float(to_double(x));
    }
    return g_sym_eval_error;
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
 * primitive_conspの論理否定にあたる非allocatingな核ロジック。nilはconsではないため
 * atomとして扱う(g_sym_tを返す)。zaのJITコンパイル済みコードからも直接呼ばれる。
 * @param val 判定対象
 * @return consでなければg_sym_t、consならnil
 */
lisp_val_t primitive_atom1(lisp_val_t val) {
    if (val == nil) {
        return g_sym_t;
    }
    return (val & TAG_MASK) == TAG_CONS ? nil : g_sym_t;
}

/**
 * 組み込み関数ATOM。第一引数がconsでないかどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return consでなければg_sym_t、consならnil
 */
lisp_val_t primitive_atom(lisp_val_t args, lisp_val_t env) {
    return primitive_atom1(cc_car(args));
}

/**
 * 組み込み関数EQL。第一引数と第二引数が同一かどうかを判定する。
 * 仕様上eqとの違いは数値・文字の値比較だが、本実装のfixnum/charは即値表現のため
 * eqのポインタ比較のままで正しく判定できる。ただしbignumは同じ値でも異なるヒープ
 * オブジェクトになりうるため、両者がMAGIC_BIGNUMの場合はsign+limb内容を比較する。
 * floatも同じ値でも異なるヒープオブジェクトになりうるため、両者がMAGIC_FLOATの場合は
 * word1のビットパターン(doubleの値)を比較する(それ以外はeqと同じ判定になる)。
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
        if (obj_a[0] == MAGIC_FLOAT && obj_b[0] == MAGIC_FLOAT && obj_a[1] == obj_b[1]) {
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
    GC_PROTECT(args);
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
 * 組み込み関数%%ZA-COMPILED-P。第一引数がza.cによって機械語へJITコンパイルされた関数
 * (MAGIC_FUNCTION_NATIVEでword2がfixnum)かどうかを判定する。テスト用の内部primitiveの
 * ため%%を付ける(ISLisp仕様外)。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return JITコンパイル済みならg_sym_t、インタプリタ実行(またはそれ以外)ならnil
 */
lisp_val_t primitive_za_compiled_p(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) != TAG_INSTANCE) {
        return nil;
    }
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    return (obj[0] == MAGIC_FUNCTION_NATIVE && obj[2] != nil) ? g_sym_t : nil;
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
 * 組み込み関数GENSYM。呼ぶたびに"G"+連番の名前で新しいuninterned symbol
 * (os_make_uninterned_symbol)を作って返す。eqによるアドレス比較でのみ使われる
 * ことを前提に、g_symbol_tableへの登録は行わない(連番が一巡しない限り
 * 名前の重複自体は起きない)。
 * @param args 未使用
 * @param env 呼び出し時の環境(未使用)
 * @return 新しく作られたuninterned SYMBOL
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
    return os_make_uninterned_symbol(buf);
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
    GC_PROTECT(list);
    UINT64 count = 0;
    for (lisp_val_t cur = list; cur != nil; cur = cc_cdr(cur)) {
        count++;
    }
    // まずword1=0のプレースホルダでVECTORをラップしGC_PROTECTしてから本体ブロックを
    // 確保する。本体ブロック確保中にGCが発火してもラップ済みのVECTOR自身は根から
    // 再配置されるため安全であり、ブロックへの書き込みが終わるまでアロケーションを
    // 挟まないことで、ブロックがどのタグ付き値からも到達不能な期間を作らない
    lisp_val_t vec = os_make_instance(MAGIC_VECTOR, 0, 0, 0);
    GC_PROTECT(vec);
    lisp_addr_t addr = alloc_vector_block(1, &count);
    lisp_val_t *data = (lisp_val_t *)(addr + 16);
    UINT64 i = 0;
    for (lisp_val_t cur = list; cur != nil; cur = cc_cdr(cur)) {
        data[i++] = cc_car(cur);
    }
    ((UINT64 *)(vec & ~TAG_MASK))[1] = (UINT64)addr;
    return vec;
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
    GC_PROTECT(init);

    lisp_val_t vec = os_make_instance(MAGIC_VECTOR, 0, 0, 0);
    GC_PROTECT(vec);
    lisp_addr_t addr = alloc_vector_block(1, &count);
    lisp_val_t *data = (lisp_val_t *)(addr + 16);
    for (UINT64 i = 0; i < count; i++) {
        data[i] = init;
    }
    ((UINT64 *)(vec & ~TAG_MASK))[1] = (UINT64)addr;
    return vec;
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

    lisp_val_t vec = os_make_instance(MAGIC_VECTOR, 0, 0, 0);
    GC_PROTECT(vec);
    lisp_addr_t addr = alloc_vector_block(rank, dims);
    lisp_val_t *data = (lisp_val_t *)(addr + 8 * (1 + rank));
    for (UINT64 i = 0; i < total; i++) {
        data[i] = nil;
    }

    ((UINT64 *)(vec & ~TAG_MASK))[1] = (UINT64)addr;
    return vec;
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
 * STRINGはrank1のbasic-arrayとして扱い、その長さを単一要素のリストで返す
 * (STRINGはVECTORと異なるヒープレイアウトのため、vector_headerに渡すと
 * 別のフィールドをrank/dimsとして誤読し、暴走したループでクラッシュする)。
 * @param args 評価済みの引数リスト(第一引数はVECTORまたはSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return 各次元のサイズ(FIXNUM)のリスト
 */
lisp_val_t primitive_array_dimensions(lisp_val_t args, lisp_val_t env) {
    lisp_val_t array = cc_car(args);
    GC_PROTECT(array);

    if ((array & TAG_MASK) == TAG_STRING) {
        lisp_addr_t addr = array & ~TAG_MASK;
        UINT64 len = ((lisp_val_t *)addr)[0];
        return os_make_cons(os_make_fixnum(len), nil);
    }

    if (!is_vector(array)) {
        return g_sym_eval_error;
    }

    lisp_val_t *header = vector_header(array);
    UINT64 rank = header[0];

    lisp_val_t result = nil;
    GC_PROTECT(result);
    for (UINT64 i = rank; i > 0; i--) {
        header = vector_header(array);
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
 * 組み込み関数ELT。第一引数のシーケンス(LIST/STRING/VECTOR)の第二引数(0起算)
 * 番目の要素を返す。VECTORの場合は次元に関わらずdata部を1次元配列とみなす
 * (LENGTHと同じ簡略化方針)。
 * @param args 評価済みの引数リスト(第一引数はLIST/STRING/VECTOR、第二引数はFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 添字が指す要素。範囲外の添字が指定された場合はg_sym_eval_error
 */
lisp_val_t primitive_elt(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t seq = cc_car(args);
    UINT64 idx = cc_car(cc_cdr(args)) >> 3;

    switch (seq & TAG_MASK) {
        case TAG_CONS: {
            lisp_val_t cur = seq;
            for (UINT64 i = 0; i < idx && cur != nil; i++) {
                cur = cc_cdr(cur);
            }
            if (cur == nil) {
                return g_sym_eval_error;
            }
            return cc_car(cur);
        }
        case TAG_STRING: {
            lisp_addr_t addr = seq & ~TAG_MASK;
            UINT64 len = ((lisp_val_t *)addr)[0];
            if (idx >= len) {
                return g_sym_eval_error;
            }
            UINT8 *bytes = (UINT8 *)(addr + 8);
            return os_make_char((char)bytes[idx]);
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
            if (idx >= total) {
                return g_sym_eval_error;
            }
            lisp_val_t *data = (lisp_val_t *)((lisp_addr_t)header + 8 * (1 + rank));
            return data[idx];
        }
        default:
            return g_sym_eval_error;
    }
}

/**
 * 組み込み関数SET-ELT。第二引数のシーケンス(LIST/STRING/VECTOR)の第三引数(0起算)
 * 番目の要素を第一引数で破壊的に書き換える。仕様上set-eltは「新しい値が最初」
 * という引数順である点に注意(SET-AREF/SET-CAR/SET-CDRとは逆順)。
 * @param args 評価済みの引数リスト(第一引数は新しい値、第二引数はLIST/STRING/VECTOR、
 *             第三引数はFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 書き込んだ値(第一引数)。範囲外の添字が指定された場合はg_sym_eval_error
 */
lisp_val_t primitive_set_elt(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t obj = cc_car(args);
    lisp_val_t seq = cc_car(cc_cdr(args));
    UINT64 idx = cc_car(cc_cdr(cc_cdr(args))) >> 3;

    switch (seq & TAG_MASK) {
        case TAG_CONS: {
            lisp_val_t cur = seq;
            for (UINT64 i = 0; i < idx && cur != nil; i++) {
                cur = cc_cdr(cur);
            }
            if (cur == nil) {
                return g_sym_eval_error;
            }
            cc_set_car(cur, obj);
            return obj;
        }
        case TAG_STRING: {
            lisp_addr_t addr = seq & ~TAG_MASK;
            UINT64 len = ((lisp_val_t *)addr)[0];
            if (idx >= len) {
                return g_sym_eval_error;
            }
            UINT8 *bytes = (UINT8 *)(addr + 8);
            bytes[idx] = (UINT8)(obj >> 3);
            return obj;
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
            if (idx >= total) {
                return g_sym_eval_error;
            }
            lisp_val_t *data = (lisp_val_t *)((lisp_addr_t)header + 8 * (1 + rank));
            data[idx] = obj;
            return obj;
        }
        default:
            return g_sym_eval_error;
    }
}

/**
 * 組み込み関数SUBSEQ。第一引数のシーケンス(LIST/STRING/VECTOR)の[z1, z2)の
 * 範囲を要素とする、同じクラスの新規シーケンスを返す。
 * @param args 評価済みの引数リスト(第一引数はLIST/STRING/VECTOR、第二・第三引数は
 *             FIXNUM(z1, z2))
 * @param env 呼び出し時の環境(未使用)
 * @return 新規に確保したシーケンス(元と同じクラス)
 */
lisp_val_t primitive_subseq(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t seq = cc_car(args);
    UINT64 z1 = cc_car(cc_cdr(args)) >> 3;
    UINT64 z2 = cc_car(cc_cdr(cc_cdr(args))) >> 3;
    UINT64 out_len = z2 - z1;

    switch (seq & TAG_MASK) {
        case TAG_CONS: {
            lisp_val_t cur = seq;
            GC_PROTECT(cur);
            for (UINT64 i = 0; i < z1; i++) {
                cur = cc_cdr(cur);
            }
            lisp_val_t result = nil;
            lisp_val_t prev = nil;
            GC_PROTECT(result);
            GC_PROTECT(prev);
            for (UINT64 i = 0; i < out_len; i++) {
                lisp_val_t cell = os_make_cons(cc_car(cur), nil);
                if (prev == nil) {
                    result = cell;
                } else {
                    cc_set_cdr(prev, cell);
                }
                prev = cell;
                cur = cc_cdr(cur);
            }
            return result;
        }
        case TAG_STRING: {
            GC_PROTECT(seq);
            lisp_addr_t out_addr = os_alloc_bytes(8 + out_len);
            lisp_addr_t addr = seq & ~TAG_MASK;
            UINT8 *bytes = (UINT8 *)(addr + 8);
            ((lisp_val_t *)out_addr)[0] = out_len;
            UINT8 *out_bytes = (UINT8 *)(out_addr + 8);
            for (UINT64 i = 0; i < out_len; i++) {
                out_bytes[i] = bytes[z1 + i];
            }
            return (lisp_val_t)(out_addr | TAG_STRING);
        }
        case TAG_INSTANCE: {
            if (!is_vector(seq)) {
                return g_sym_eval_error;
            }
            GC_PROTECT(seq);
            lisp_val_t out_vec = os_make_instance(MAGIC_VECTOR, 0, 0, 0);
            GC_PROTECT(out_vec);
            lisp_addr_t out_addr = alloc_vector_block(1, &out_len);
            // out_addr確保後はseqから再度header/dataを取り直す(GCでseqが再配置された可能性がある)
            lisp_val_t *header = vector_header(seq);
            UINT64 rank = header[0];
            lisp_val_t *data = (lisp_val_t *)((lisp_addr_t)header + 8 * (1 + rank));
            lisp_val_t *out_data = (lisp_val_t *)(out_addr + 16);
            for (UINT64 i = 0; i < out_len; i++) {
                out_data[i] = data[z1 + i];
            }
            ((UINT64 *)(out_vec & ~TAG_MASK))[1] = (UINT64)out_addr;
            return out_vec;
        }
        default:
            return g_sym_eval_error;
    }
}


/**
 * 組み込み関数%%MAKE-CLASS-RAW。ILOSクラスオブジェクト(MAGIC_STANDARD_CLASS)を確保する。
 * @param args 評価済みの引数リスト(name symbol, supers クラスオブジェクトのlist, slots スロット記述子のlist)
 * @param env 呼び出し時の環境(未使用)
 * @return 確保したクラスオブジェクト
 */
lisp_val_t primitive_make_class_raw(lisp_val_t args, lisp_val_t env) {
    lisp_val_t name = cc_car(args);
    lisp_val_t supers = cc_car(cc_cdr(args));
    lisp_val_t slots = cc_car(cc_cdr(cc_cdr(args)));
    return os_make_instance(MAGIC_STANDARD_CLASS, name, supers, slots);
}

/**
 * 組み込み関数%%MAKE-BUILTIN-CLASS-RAW。ILOSクラスオブジェクト(MAGIC_BUILTIN_CLASS)を確保する。
 * @param args 評価済みの引数リスト(name symbol, supers クラスオブジェクトのlist, slots スロット記述子のlist)
 * @param env 呼び出し時の環境(未使用)
 * @return 確保したクラスオブジェクト
 */
lisp_val_t primitive_make_builtin_class_raw(lisp_val_t args, lisp_val_t env) {
    lisp_val_t name = cc_car(args);
    lisp_val_t supers = cc_car(cc_cdr(args));
    lisp_val_t slots = cc_car(cc_cdr(cc_cdr(args)));
    return os_make_instance(MAGIC_BUILTIN_CLASS, name, supers, slots);
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
 * 組み込み関数%%CLASSP。第一引数がILOSクラスオブジェクト(MAGIC_BUILTIN_CLASSまたはMAGIC_STANDARD_CLASS)かどうかを判定する。
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
    return (obj[0] == MAGIC_BUILTIN_CLASS || obj[0] == MAGIC_STANDARD_CLASS) ? g_sym_t : nil;
}

/**
 * 組み込み関数%%BUILTIN-CLASSP。第一引数がILOSの組み込みクラスオブジェクト(MAGIC_BUILTIN_CLASS)かどうかを判定する。
 * @param args 評価済みの引数リスト(第一引数は任意の値)
 * @param env 呼び出し時の環境(未使用)
 * @return 組み込みクラスオブジェクトならt、それ以外ならnil
 */
lisp_val_t primitive_builtin_classp(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) != TAG_INSTANCE) {
        return nil;
    }
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    return obj[0] == MAGIC_BUILTIN_CLASS ? g_sym_t : nil;
}

/**
 * 組み込み関数%%STANDARD-CLASSP。第一引数がILOSの標準クラスオブジェクト(MAGIC_STANDARD_CLASS)かどうかを判定する。
 * @param args 評価済みの引数リスト(第一引数は任意の値)
 * @param env 呼び出し時の環境(未使用)
 * @return 標準クラスオブジェクトならt、それ以外ならnil
 */
lisp_val_t primitive_standard_classp(lisp_val_t args, lisp_val_t env) {
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) != TAG_INSTANCE) {
        return nil;
    }
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    return obj[0] == MAGIC_STANDARD_CLASS ? g_sym_t : nil;
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

