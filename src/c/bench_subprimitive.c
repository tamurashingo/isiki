#include "bench_subprimitive.h"
#include "runtime.h"
#include "lisp.h"
#include "eval.h"

// [性能測定] 構文別ベンチマークスイートのC側参照実装(bench_subprimitive.h参照)。
//
// 実装方針(src/lisp/bench_aot.lispのコメントと対になる):
//   - 「Lispコードが本来意図している計算」を、どの言語でも書くであろう素直な形で
//     Cで書く。Cコンパイラの自動ベクトル化やインライン化を狙った書き方はしない。
//   - ビルド条件はAOT側と揃える(Makefileの-O1、同じmingw-gccで同時にコンパイル)。
//   - 再帰系は、CもAOTもスタック深度の上限があるため深さBENCH_REC_DEPTHで区切り、
//     外側のループでN/BENCH_REC_DEPTH回繰り返す(仕事の単位数は常にN個になる)。
//
// BENCH_KEEPについて: -O1のGCCは、ループの最終値を閉じた式で求められる場合
// (単純な減算ループや等差数列の和)にループ自体を消してしまう(SCEVによる
// final value replacement)。そうなると「ループを回すコスト」の計測にならないため、
// 各反復で値をレジスタに具現化させるだけの空のインラインアセンブラを挟む。
// 命令は1つも増えない(Google BenchmarkのDoNotOptimizeと同じイディオム)。
// 最適化を「利用する」ための細工ではなく、計測対象が消えるのを防ぐためのもの。
#define BENCH_KEEP(x) __asm__ __volatile__("" : "+r"(x))

/** 再帰ベンチマーク1回あたりの再帰の深さ。
    [性能測定] Phase4: 当初1000にしていたが、AOTの非末尾再帰は1レベルあたり
    __fixed(136byte)と__step_fixed(112byte)の2フレームを積み、保存レジスタと
    戻りアドレスを含めて約330byte消費する。プロセスのスタックは256KB
    (process.cのSTACK_SIZE)しかないため深度1000では約330KBとなり溢れる。
    スタックガードが無いためオーバーフローは検出されず、ゲストが無反応の
    まま停止する(CPU使用率が1%未満になるだけで、外からは極端に遅い処理と
    区別しにくい)。AOT側で十分な余裕を持つ100にする。C側と同じ値を使うことで
    比較の前提は保たれる(仕事の単位数は常にN個で変わらない) */
#define BENCH_REC_DEPTH 100
/** cons/vectorベンチマークが使うリスト長・ベクタ長 */
#define BENCH_LIST_LEN 1000
#define BENCH_VEC_LEN 1000

/* --- 1. 単純ループ(制御構造のベースライン) --- */
static UINT64 bench_loop(UINT64 n) {
    UINT64 i = n;
    while (i > 0) {
        i = i - 1;
        BENCH_KEEP(i);
    }
    return i;
}

/* --- 2. fixnum算術(加算+比較) --- */
static UINT64 bench_arith(UINT64 n) {
    UINT64 acc = 0;
    UINT64 i = 0;
    while (i < n) {
        acc = acc + i;
        i = i + 1;
        BENCH_KEEP(acc);
    }
    return acc;
}

/* --- 3. 末尾再帰 --- */
static UINT64 bench_tailrec_step(UINT64 n, UINT64 acc) {
    if (n == 0) {
        return acc;
    }
    return bench_tailrec_step(n - 1, acc + n);
}

static UINT64 bench_tailrec(UINT64 n) {
    UINT64 reps = n / BENCH_REC_DEPTH;
    UINT64 total = 0;
    UINT64 r = 0;
    while (r < reps) {
        total = total + bench_tailrec_step(BENCH_REC_DEPTH, 0);
        r = r + 1;
        BENCH_KEEP(total);
    }
    return total;
}

/* --- 4. 非末尾再帰 --- */
static UINT64 bench_nontailrec_step(UINT64 n) {
    if (n == 0) {
        return 0;
    }
    return n + bench_nontailrec_step(n - 1);
}

static UINT64 bench_nontailrec(UINT64 n) {
    UINT64 reps = n / BENCH_REC_DEPTH;
    UINT64 total = 0;
    UINT64 r = 0;
    while (r < reps) {
        total = total + bench_nontailrec_step(BENCH_REC_DEPTH);
        r = r + 1;
        BENCH_KEEP(total);
    }
    return total;
}

/* --- 5. 条件分岐(5分岐のcond相当) --- */
static UINT64 bench_branch(UINT64 n) {
    UINT64 acc = 0;
    UINT64 i = 0;
    UINT64 k = 0;
    while (i < n) {
        if (k == 0) {
            acc = acc + 1;
        } else if (k == 1) {
            acc = acc + 2;
        } else if (k == 2) {
            acc = acc + 3;
        } else if (k == 3) {
            acc = acc + 4;
        } else {
            acc = acc + 5;
        }
        k = (k >= 4) ? 0 : k + 1;
        i = i + 1;
        BENCH_KEEP(acc);
    }
    return acc;
}

/* --- 6. ローカル変数束縛(5変数) --- */
static UINT64 bench_let(UINT64 n) {
    UINT64 acc = 0;
    UINT64 i = 0;
    while (i < n) {
        UINT64 a = i;
        UINT64 b = a + 1;
        UINT64 c = b + 1;
        UINT64 d = c + 1;
        UINT64 e = d + 1;
        acc = acc + e;
        i = i + 1;
        BENCH_KEEP(acc);
    }
    return acc;
}

/* --- 7. cons/リスト操作 ---
   長さBENCH_LIST_LENのリストを構築して走査し、捨てる。これをN/BENCH_LIST_LEN回
   繰り返す(生存ヒープが際限なく増えないようにするため)。C側もLispのconsセルを
   os_make_consで確保する(データ構造そのものは同じにし、Lisp呼び出し規約と
   GC_PROTECTの有無だけを比較対象にする)。 */
static UINT64 bench_cons(UINT64 n) {
    UINT64 reps = n / BENCH_LIST_LEN;
    UINT64 total = 0;
    for (UINT64 r = 0; r < reps; r++) {
        lisp_val_t list = nil;
        GC_PROTECT(list);
        for (UINT64 i = 0; i < BENCH_LIST_LEN; i++) {
            list = os_make_cons(os_make_fixnum(i), list);
        }
        // 走査中は確保を行わないためGCは発火せず、追加の保護は不要
        for (lisp_val_t cur = list; cur != nil; cur = cc_cdr(cur)) {
            total = total + os_fixnum_magnitude(cc_car(cur));
        }
    }
    return total;
}

/* --- 8. イテレーション構文(ISLispのforマクロに対応するCのforループ) --- */
static UINT64 bench_for(UINT64 n) {
    UINT64 acc = 0;
    for (UINT64 i = 0; i < n; i = i + 1) {
        acc = acc + i;
        BENCH_KEEP(acc);
    }
    return acc;
}

/* --- 9. ベクタ操作(書き込み+読み出し) ---
   長さBENCH_VEC_LENのgeneral-vectorを1回だけ確保し、以降はN回の書き込み+読み出しを
   行う(確保のコストではなく、要素アクセスのコストを測る)。ループ中に確保を
   行わないためGCは発火せず、dataポインタは最後まで有効。 */
static UINT64 bench_vector(UINT64 n) {
    lisp_val_t *data;
    lisp_val_t vec = os_make_vector_raw(BENCH_VEC_LEN, &data);
    GC_PROTECT(vec);
    for (UINT64 i = 0; i < BENCH_VEC_LEN; i++) {
        data[i] = os_make_fixnum(0);
    }
    UINT64 acc = 0;
    UINT64 idx = 0;
    for (UINT64 i = 0; i < n; i++) {
        data[idx] = os_make_fixnum(i);
        acc = acc + os_fixnum_magnitude(data[idx]);
        idx = (idx + 1 >= BENCH_VEC_LEN) ? 0 : idx + 1;
    }
    return acc;
}

/* --- 10. 関数呼び出し(非末尾・非自己再帰) ---
   noinlineを付けるのは、AOT側が実際に関数呼び出しを行う以上、C側でインライン化を
   許すと「呼び出しコストの比較」にならないため(最適化に頼るのではなく、比較の
   前提を揃えるための指定)。 */
__attribute__((noinline)) static UINT64 bench_callee(UINT64 x) {
    return x + 1;
}

static UINT64 bench_funcall(UINT64 n) {
    UINT64 acc = 0;
    for (UINT64 i = 0; i < n; i++) {
        acc = bench_callee(acc);
        BENCH_KEEP(acc);
    }
    return acc;
}

/* [性能測定] 診断専用(Phase2の2-0-1): インライン展開で消せる3つの操作
   (1)os_make_lifted_closure_with_meta によるクロージャ生成
   (2)引数リストのos_make_cons
   (3)primitive_funcall の動的ディスパッチ
   だけをN回繰り返し、その合計コストを測る。letのAOT生成コードが1束縛あたり
   この3つを行うため、「インライン展開が最大でいくつ削減できるか」の下限が分かる
   (AOT側は各式をGC_PROTECT/os_is_control_transferで包むぶん更に重いので下限)。 */
static lisp_val_t bench_closure_body(lisp_val_t evaluated_args, lisp_val_t env) {
    (void)env;
    return cc_car(evaluated_args);
}

static UINT64 bench_closure_call(UINT64 n) {
    static za_fn_meta_t meta;
    UINT64 acc = 0;
    for (UINT64 i = 0; i < n; i++) {
        lisp_val_t fn = os_make_lifted_closure_with_meta(
            &meta, (lisp_addr_t)(void *)bench_closure_body, global_environment);
        GC_PROTECT(fn);
        lisp_val_t args = os_make_cons(fn, os_make_cons(os_make_fixnum(i), nil));
        lisp_val_t r = primitive_funcall(args, global_environment);
        acc = acc + os_fixnum_magnitude(r);
    }
    return acc;
}

/* [性能測定] 診断専用(Phase4 第0部 2-3): os_is_control_transferの単価を実測する。
   GC_PROTECTの34.9命令が実測値なのに対し、こちらは見積もりのままだったため、
   同じ土俵(傾き法)で比較できるようにする。戻り値を積算して最適化で消えるのを防ぐ */
static UINT64 bench_ct_check(UINT64 n) {
    UINT64 hits = 0;
    lisp_val_t v = os_make_fixnum(1);
    for (UINT64 i = 0; i < n; i++) {
        BENCH_KEEP(v);
        if (os_is_control_transfer(v)) {
            hits = hits + 1;
        }
    }
    return hits + n;
}

/* [性能測定] 診断専用: Immobilized Spaceを意図的に消費し、枯渇時にOSが永久停止
   せずos_panicへ到達することを確認するためのもの。専用カーソルから確保するため、
   os_imm_space_used_bytesはこのカーソルの現在ページの未使用末尾分だけ過大に
   報告する(診断用途では問題にならない)。%%DIAG-IDE-READ-SECTORS-ADDRと同様、
   恒久的な公開APIとしての安定性は保証しない */
static imm_slot_cursor_t g_bench_imm_burn_cursor = {0, 0};

static lisp_val_t cc_diag_imm_burn(lisp_val_t args, lisp_val_t env) {
    (void)env;
    UINT64 n = os_fixnum_magnitude(cc_car(args));
    for (UINT64 i = 0; i < n; i++) {
        (void)os_imm_slot_alloc(&g_bench_imm_burn_cursor, sizeof(za_fn_meta_t));
    }
    return os_make_fixnum(os_imm_space_used_bytes());
}

/** 第一引数のFIXNUMをUINT64として取り出す(全ベンチマーク共通の引数取り出し) */
static UINT64 bench_arg_n(lisp_val_t args) {
    return os_fixnum_magnitude(cc_car(args));
}

#define BENCH_DEFINE_PRIMITIVE(cc_name, impl)                     \
    static lisp_val_t cc_name(lisp_val_t args, lisp_val_t env) {  \
        (void)env;                                                \
        return os_make_fixnum(impl(bench_arg_n(args)));           \
    }

BENCH_DEFINE_PRIMITIVE(cc_bench_c_loop, bench_loop)
BENCH_DEFINE_PRIMITIVE(cc_bench_c_arith, bench_arith)
BENCH_DEFINE_PRIMITIVE(cc_bench_c_tailrec, bench_tailrec)
BENCH_DEFINE_PRIMITIVE(cc_bench_c_nontailrec, bench_nontailrec)
BENCH_DEFINE_PRIMITIVE(cc_bench_c_branch, bench_branch)
BENCH_DEFINE_PRIMITIVE(cc_bench_c_let, bench_let)
BENCH_DEFINE_PRIMITIVE(cc_bench_c_cons, bench_cons)
BENCH_DEFINE_PRIMITIVE(cc_bench_c_for, bench_for)
BENCH_DEFINE_PRIMITIVE(cc_bench_c_vector, bench_vector)
BENCH_DEFINE_PRIMITIVE(cc_bench_c_funcall, bench_funcall)
BENCH_DEFINE_PRIMITIVE(cc_diag_closure_call, bench_closure_call)
BENCH_DEFINE_PRIMITIVE(cc_diag_ct_check, bench_ct_check)

void os_register_bench_subprimitives(void) {
    os_set_function(os_make_symbol("%%BENCH-C-LOOP"),
                     os_make_native_function((lisp_addr_t)(void *)cc_bench_c_loop), global_environment);
    os_set_function(os_make_symbol("%%BENCH-C-ARITH"),
                     os_make_native_function((lisp_addr_t)(void *)cc_bench_c_arith), global_environment);
    os_set_function(os_make_symbol("%%BENCH-C-TAILREC"),
                     os_make_native_function((lisp_addr_t)(void *)cc_bench_c_tailrec), global_environment);
    os_set_function(os_make_symbol("%%BENCH-C-NONTAILREC"),
                     os_make_native_function((lisp_addr_t)(void *)cc_bench_c_nontailrec), global_environment);
    os_set_function(os_make_symbol("%%BENCH-C-BRANCH"),
                     os_make_native_function((lisp_addr_t)(void *)cc_bench_c_branch), global_environment);
    os_set_function(os_make_symbol("%%BENCH-C-LET"),
                     os_make_native_function((lisp_addr_t)(void *)cc_bench_c_let), global_environment);
    os_set_function(os_make_symbol("%%BENCH-C-CONS"),
                     os_make_native_function((lisp_addr_t)(void *)cc_bench_c_cons), global_environment);
    os_set_function(os_make_symbol("%%BENCH-C-FOR"),
                     os_make_native_function((lisp_addr_t)(void *)cc_bench_c_for), global_environment);
    os_set_function(os_make_symbol("%%BENCH-C-VECTOR"),
                     os_make_native_function((lisp_addr_t)(void *)cc_bench_c_vector), global_environment);
    os_set_function(os_make_symbol("%%DIAG-CT-CHECK"),
                     os_make_native_function((lisp_addr_t)(void *)cc_diag_ct_check), global_environment);
    os_set_function(os_make_symbol("%%DIAG-CLOSURE-CALL"),
                     os_make_native_function((lisp_addr_t)(void *)cc_diag_closure_call), global_environment);
    os_set_function(os_make_symbol("%%DIAG-IMM-BURN"),
                     os_make_native_function((lisp_addr_t)(void *)cc_diag_imm_burn), global_environment);
    os_set_function(os_make_symbol("%%BENCH-C-FUNCALL"),
                     os_make_native_function((lisp_addr_t)(void *)cc_bench_c_funcall), global_environment);
}
