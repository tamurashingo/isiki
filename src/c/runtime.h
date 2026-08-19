#ifndef _RUNTIME_H_
#define _RUNTIME_H_

#include "types.h"
#include "process.h"

/** タグ位置(下位3bit)を取り出すマスク */
#define TAG_MASK     0x7ULL
/** 即値のfixnum(000) */
#define TAG_FIXNUM   0x0ULL
/** cons cellへのアドレス(001) */
#define TAG_CONS     0x1ULL
/** symbolへのアドレス(010) */
#define TAG_SYMBOL   0x2ULL
/** 即値のchar(011) */
#define TAG_CHAR     0x3ULL
/** stringへのアドレス(100) */
#define TAG_STRING   0x4ULL
/** instance(function/process等)へのアドレス(101) */
#define TAG_INSTANCE 0x5ULL
/** GCが転送済みオブジェクトの新しい位置を指すために使う、転送先アドレス(110)。ヒープ上のオブジェクトのword0にのみ現れる */
#define TAG_FORWARD  0x6ULL
/** Lispヒープに属さない生の64bitアドレス(C構造体・MMIOレジスタ等)。fixnum/charと同様の即値として扱い、GCは素通しする(111) */
#define TAG_RAW_POINTER 0x7ULL

/** TAG_FIXNUMの値フィールド最上位bit(bit63)。1なら負数を表す(0は常に非負に正規化) */
#define FIXNUM_SIGN_BIT       0x8000000000000000ULL
/** TAG_FIXNUMの値フィールドのうちマグニチュードに使う60bit(bit3〜62)分のマスク */
#define FIXNUM_MAGNITUDE_MASK ((1ULL << 60) - 1)


/** TAG_INSTANCEのword0に入る、ネイティブ(C)関数であることを示すMAGIC NUMBER */
#define MAGIC_FUNCTION_NATIVE      0x1ULL
/** TAG_INSTANCEのword0に入る、Lisp(defun)で定義された関数であることを示すMAGIC NUMBER */
#define MAGIC_FUNCTION_INTERPRETED 0x2ULL
/** TAG_INSTANCEのword0に入る、プロセスのPCBであることを示すMAGIC NUMBER */
#define MAGIC_PROCESS              0x3ULL
/** TAG_INSTANCEのword0に入る、Lisp(defmacro)で定義されたマクロであることを示すMAGIC NUMBER */
#define MAGIC_MACRO                0x4ULL
/** TAG_INSTANCEのword0に入る、block/return-from/unwind-protectの非局所脱出シグナルであることを示すMAGIC NUMBER */
#define MAGIC_BLOCK_EXIT           0x5ULL
/** TAG_INSTANCEのword0に入る、streamオブジェクトであることを示すMAGIC NUMBER */
#define MAGIC_STREAM               0x6ULL
/** TAG_INSTANCEのword0に入る、ILOSのクラスインスタンスであることを示すMAGIC NUMBER。word1=class、word2=slots-vector(MAGIC_VECTOR)、word3=未使用 */
#define MAGIC_CLASS_INSTANCE       0x8ULL
/** TAG_INSTANCEのword0に入る、catch/throwの非局所脱出シグナルであることを示すMAGIC NUMBER。word1=tag(evalされた値)、word2=throwされた値 */
#define MAGIC_CATCH_EXIT           0x9ULL
/** TAG_INSTANCEのword0に入る、tagbody/goの非局所脱出シグナルであることを示すMAGIC NUMBER。word1=tag(未評価のsymbol) */
#define MAGIC_GO_EXIT              0xAULL
/** TAG_INSTANCEのword0に入る、60bitを超える整数(bignum)であることを示すMAGIC NUMBER。word1=sign(0:非負/1:負)、word2=limb数、word3=limb配列(基数2^32、下位32bitのみ使用、limbs[0]が最下位)への生ポインタ */
#define MAGIC_BIGNUM               0xBULL
/** TAG_INSTANCEのword0に入る、多次元配列(vector/general array)であることを示すMAGIC NUMBER。word1=配列本体(rank+各次元サイズ+要素データを格納した可変長ブロック)への生ポインタ、word2/word3=未使用 */
#define MAGIC_VECTOR               0xCULL
/** TAG_INSTANCEのword0に入る、ISLispのfloat(IEEE754 binary64)であることを示すMAGIC NUMBER。word1=doubleのビットパターン、word2/word3=未使用 */
#define MAGIC_FLOAT                0xDULL
/** TAG_INSTANCEのword0に入る、ILOSの組み込み(built-in)クラスオブジェクトであることを示すMAGIC NUMBER。メタクラスは`<built-in-class>`。word1=name(symbol)、word2=superclasses(クラスオブジェクトのlist)、word3=slots(スロット記述子のlist、継承分含む) */
#define MAGIC_BUILTIN_CLASS        0xEULL
/** TAG_INSTANCEのword0に入る、ILOSの標準(standard)クラスオブジェクトであることを示すMAGIC NUMBER。メタクラスは`<standard-class>`。word1=name(symbol)、word2=superclasses(クラスオブジェクトのlist)、word3=slots(スロット記述子のlist、継承分含む) */
#define MAGIC_STANDARD_CLASS       0xFULL

/** NIL */
extern lisp_val_t nil;
/** T */
extern lisp_val_t g_sym_t;

/** プロセスの状態を表すシンボル:READY(入力待ち) */
extern lisp_val_t g_sym_process_ready;
/** プロセスの状態を表すシンボル:RUNNING(実行中) */
extern lisp_val_t g_sym_process_running;
/** プロセスの状態を表すシンボル:DEAD(終了済み) */
extern lisp_val_t g_sym_process_dead;

/** 実行中のPCBを登録する自己参照する循環consリストを保持するシンボル(*RUN-QUEUE*) */
extern lisp_val_t g_sym_run_queue;
/** *RUN-QUEUE*上の現在実行中のプロセスを指すセルを保持するシンボル(*CURRENT-PROCESS*) */
extern lisp_val_t g_sym_current_process;

/** quote特殊形式を表すシンボル */
extern lisp_val_t g_sym_quote;
/** if特殊形式を表すシンボル */
extern lisp_val_t g_sym_if;
/** progn特殊形式を表すシンボル */
extern lisp_val_t g_sym_progn;
/** setq特殊形式を表すシンボル */
extern lisp_val_t g_sym_setq;
/** defun特殊形式を表すシンボル */
extern lisp_val_t g_sym_defun;
/** lambda特殊形式を表すシンボル */
extern lisp_val_t g_sym_lambda;
/** defmacro特殊形式を表すシンボル */
extern lisp_val_t g_sym_defmacro;
/** block特殊形式を表すシンボル */
extern lisp_val_t g_sym_block;
/** return-from特殊形式を表すシンボル */
extern lisp_val_t g_sym_return_from;
/** unwind-protect特殊形式を表すシンボル */
extern lisp_val_t g_sym_unwind_protect;
/** function特殊形式を表すシンボル */
extern lisp_val_t g_sym_function;
/** flet特殊形式を表すシンボル */
extern lisp_val_t g_sym_flet;
/** labels特殊形式を表すシンボル */
extern lisp_val_t g_sym_labels;
/** defvar特殊形式を表すシンボル */
extern lisp_val_t g_sym_defvar;
/** defconstant特殊形式を表すシンボル */
extern lisp_val_t g_sym_defconstant;
/** defdynamic特殊形式を表すシンボル */
extern lisp_val_t g_sym_defdynamic;
/** defglobal特殊形式を表すシンボル */
extern lisp_val_t g_sym_defglobal;
/** dynamic特殊形式を表すシンボル */
extern lisp_val_t g_sym_dynamic;
/** 仮引数リストで残りの実引数をリストとしてまとめて受け取ることを示すマーカーシンボル(&rest) */
extern lisp_val_t g_sym_rest;
/** quasiquote(`)を表すシンボル。reader.cが`x`を(QUASIQUOTE x)へ読むために使う */
extern lisp_val_t g_sym_quasiquote;
/** unquote(,)を表すシンボル。reader.cが,xを(UNQUOTE x)へ読むために使う */
extern lisp_val_t g_sym_unquote;
/** unquote-splicing(,@)を表すシンボル。reader.cが,@xを(UNQUOTE-SPLICING x)へ読むために使う */
extern lisp_val_t g_sym_unquote_splicing;
/** dotted pair記法の'.'を表すシンボル。reader.cのread_listが単独トークンの'.'を検出するために使う */
extern lisp_val_t g_sym_dot;
/** car関数を表すシンボル */
extern lisp_val_t g_sym_car;
/** cdr関数を表すシンボル */
extern lisp_val_t g_sym_cdr;
/** cons関数を表すシンボル */
extern lisp_val_t g_sym_cons;

/** 構文エラーを表すシンボル */
extern lisp_val_t g_sym_read_error;
/** 評価エラーを表すシンボル */
extern lisp_val_t g_sym_eval_error;

/** os_eval_top_levelが張るblockの名前を表すシンボル(%TOP-LEVEL) */
extern lisp_val_t g_sym_top_level_block;

/** catch特殊形式を表すシンボル */
extern lisp_val_t g_sym_catch;
/** throw特殊形式を表すシンボル */
extern lisp_val_t g_sym_throw;
/** tagbody特殊形式を表すシンボル */
extern lisp_val_t g_sym_tagbody;
/** go特殊形式を表すシンボル */
extern lisp_val_t g_sym_go;

/** init.lisp の make-instance 関数を表すシンボル(os_signal_conditionがC→Lisp呼び出しに使う) */
extern lisp_val_t g_sym_make_instance;
/** init.lisp の signal-condition 関数を表すシンボル(os_signal_conditionがC→Lisp呼び出しに使う) */
extern lisp_val_t g_sym_signal_condition;
/** init.lisp の %find-class 関数を表すシンボル(signal_domain_errorが expected-class 解決に使う) */
extern lisp_val_t g_sym_percent_find_class;
/** <domain-error> クラスを表すシンボル */
extern lisp_val_t g_sym_class_domain_error;
/** <parse-error> クラスを表すシンボル */
extern lisp_val_t g_sym_class_parse_error;
/** <number> クラスを表すシンボル */
extern lisp_val_t g_sym_class_number;
/** :object キーワードを表すシンボル(<domain-error>の初期化引数) */
extern lisp_val_t g_sym_kw_object;
/** :expected-class キーワードを表すシンボル(<domain-error>/<parse-error>の初期化引数) */
extern lisp_val_t g_sym_kw_expected_class;
/** :string キーワードを表すシンボル(<parse-error>の初期化引数) */
extern lisp_val_t g_sym_kw_string;

/** ルートの環境(全プロセスの環境が最終的にこれを親として辿る) */
extern lisp_val_t global_environment;

/** defdynamicで定義された動的変数の値を保持するグローバルなフラットalist(sym . val)。レキシカルなenvの親子関係とは無関係 */
extern lisp_val_t g_dynamic_bindings;

/**
 * ヒープをheap_base〜heap_base+heap_sizeで初期化する。
 * @param heap_base ヒープの先頭アドレス
 * @param heap_size ヒープのサイズ(バイト)
 */
void os_heap_init(UINT64 heap_base, UINT64 heap_size);

/** NIL・global_environment・組み込みシンボル/関数を構築し、Lisp実行環境を起動する */
void os_bootstrap();

/**
 * From空間の使用率を返す(0.0〜1.0)。os_gc_collectを呼ぶべきかどうかの閾値判定に使う。
 * @return 使用中バイト数 / From空間全体のバイト数
 */
double os_heap_used_ratio(void);

/**
 * Cheney方式のコピーGCを1回実行する。global_environment・g_dynamic_bindings・
 * g_symbol_table・キャッシュ済みg_sym_*・各プロセスのenv・全プロセスのshadow stack
 * (GC_PROTECTされたCローカル変数)をルートとして生存オブジェクトをTo空間へコピーし、
 * From/To空間を入れ替える。eval.c/runtime.cのアロケーションを行う関数はGC_PROTECT、
 * および生バッファの「確保直後に包む」規律(documents/isiki-os.md参照)によりいずれの
 * 呼び出しからの発火にも対応しているため、os_alloc_bytesのOOM時にも安全に呼び出せる。
 */
void os_gc_collect(void);

/**
 * os_gc_collectが呼ばれた延べ回数を返す。GC発火自体を直接観測する手段がないテストコードが、
 * 「計算の途中で実際にGCが起動したか」を確認するために使う(本体のロジックでは使用しない)。
 * @return os_gc_collectの呼び出し回数の累積
 */
UINT64 os_gc_collect_count(void);

/* ============================== Immobilized Space ==============================
 * GC(copy GC)が移動・破棄しない固定領域。JITコードやFunction Cellなど、Cのポインタとして
 * 直接掴んでおきたいデータの置き場所として使う。os_gc_collectのスキャン対象外であり、
 * ここへのポインタをTAG_RAW_POINTERとしてLisp値に埋め込んでも、gc_copy_valueは
 * fixnum/char同様に即値としてそのまま素通しする(移動・追跡しない)。
 */

/** Immobilized Spaceの1ページのバイト数 */
#define IMM_PAGE_SIZE 4096

/**
 * Immobilized Spaceから4KBページを1枚確保する。空間が枯渇した場合は診断メッセージを
 * 表示して停止する。
 * @return 確保した4KBページの先頭アドレス
 */
void *os_imm_page_alloc(void);

/**
 * os_imm_page_allocで確保したページをフリーリストへ返却し、再利用可能にする。
 * @param page 返却するページの先頭アドレス(os_imm_page_allocが返したものに限る)
 */
void os_imm_page_free(void *page);

/**
 * Immobilized Spaceのbump領域から、物理的に連続したcountページを確保する。
 * os_imm_page_freeで返却されたページは返却順に連続性の保証がないため、フリーリストは
 * 使わずbump領域のみを対象にする。JITコンパイル済み関数1つ分のコードを、複数ページに
 * わたる場合でも1つの連続領域として配置したいza.cからの利用を想定している。
 * os_imm_page_allocの単ページ確保と異なり、枯渇時はハードハルトせずNULLを返す
 * (呼び出し元がインタプリタへのフォールバックという既存の劣化パスを持つため)。
 * @param count 確保するページ数
 * @return 確保できた先頭ページのアドレス。空き不足の場合はNULL
 */
void *os_imm_pages_alloc_contiguous(UINT64 count);

/** os_imm_slot_allocが使う、1ページ内でのバンプアロケーションの進行状況を保持するカーソル */
typedef struct {
    UINT8 *page;   /* 現在切り出し中のページ(0ならまだページ未確保) */
    UINT64 offset; /* そのページ内で次に切り出す位置 */
} imm_slot_cursor_t;

/**
 * cursorが指すページから16byteアライメントでsizeバイトを切り出す。ページが未確保、
 * または残りが足りない場合は新しいページをos_imm_page_allocで確保してcursorを進める。
 * @param cursor 呼び出し元が保持するカーソル(呼ぶたびに状態が進む)
 * @param size 切り出すバイト数(IMM_PAGE_SIZEを超えるサイズは指定できない)
 * @return 切り出した領域の先頭アドレス
 */
void *os_imm_slot_alloc(imm_slot_cursor_t *cursor, UINT64 size);

/**
 * lisp_val_t型の変数へのポインタを、os_gc_collectが毎回書き換えるルート集合に登録する
 * (idempotent: 同じポインタを複数回登録しても1回分の登録として扱われる)。
 * process.cのように、GCの走査対象になる変数(各プロセスのenv等)をruntime.cの
 * static変数として持てない箇所から使う。
 * @param root_ptr 登録するroot変数へのポインタ
 */
void os_gc_register_root(lisp_val_t *root_ptr);

/**
 * os_gc_register_rootで登録したrootを登録解除する(Phase3.6: リテラルスロットプール
 * 枯渇解消)。g_gc_extra_roots(固定配列+線形スキャン、順序に意味を持たない)から
 * 該当エントリを見つけ、配列末尾要素と入れ替えてcountを減らす(swap-remove)。
 * 見つからない場合は何もしない(冪等)。
 * @param root_ptr 登録解除するroot変数へのポインタ(os_gc_register_rootに渡したものと同一)
 */
void os_gc_unregister_root(lisp_val_t *root_ptr);

/**
 * GC_PROTECTされたローカル変数のcleanup(スコープ脱出時)ハンドラ。
 * 現在のプロセスのshadow stack先頭を、このノードのnextに巻き戻す。
 * GC_PROTECTマクロ内でのみ使う。
 */
static inline void gc_unprotect_node(gc_rootnode *node) {
    get_current_process()->gc_roots = node->next;
}

/**
 * var(lisp_val_t型のローカル変数/パラメータ)を現在のプロセスのshadow stackの
 * 先頭に繋ぎ、GCのルート集合に加える。スコープを抜ける際(どのreturn文経由でも)
 * __attribute__((cleanup(...)))により自動的にshadow stackから外れるため、
 * 対応するGC_UNPROTECTの呼び出しは不要。varは登録前に有効なlisp_val_t(nil等)で
 * 初期化しておくこと。
 */
#define GC_PROTECT(var) \
    gc_rootnode _gcnode_##var __attribute__((cleanup(gc_unprotect_node))) = \
        { (lisp_val_t *)&(var), get_current_process()->gc_roots }; \
    get_current_process()->gc_roots = &_gcnode_##var

/**
 * 非負のfixnumオブジェクトを作る(即値、ヒープ確保なし、符号は常に0)。
 * fixnumは0〜2^60-1(60bit)まで表現できる。既存呼び出し側は非負値しか渡さないため
 * このAPIのまま残す。符号付きの値を作る場合はos_make_fixnum_signedを使う。
 * @param fixnum 表現する値(0〜2^60-1)
 * @return タグ付けされたFIXNUM
 */
lisp_val_t os_make_fixnum(const UINT64 fixnum);

/**
 * 符号付きのfixnumオブジェクトを作る(即値、ヒープ確保なし)。
 * magnitudeが0の場合はnegativeの値に関わらず符号は0に正規化される(-0は存在しない)。
 * @param negative 0以外を渡すと負数として作る
 * @param magnitude 絶対値(0〜2^60-1)
 * @return タグ付けされたFIXNUM
 */
lisp_val_t os_make_fixnum_signed(int negative, UINT64 magnitude);

/**
 * FIXNUMのマグニチュード(絶対値)を取り出す。
 * @param val タグ付けされたFIXNUM
 * @return 0〜2^60-1のマグニチュード
 */
UINT64 os_fixnum_magnitude(lisp_val_t val);

/**
 * FIXNUMが負数かどうかを判定する。
 * @param val タグ付けされたFIXNUM
 * @return 負数なら0以外、そうでなければ0
 */
int os_fixnum_is_negative(lisp_val_t val);

/**
 * 符号付きマグニチュード(limb配列、基数2^32、limbs[0]が最下位)から整数オブジェクトを作る。
 * 正規化した結果マグニチュードが60bit(FIXNUM_MAGNITUDE_MASK)以内に収まる場合は
 * os_make_fixnum_signedで即値化し(ヒープ確保なし)、それ以外はlimb配列をコピーして
 * ヒープに確保しMAGIC_BIGNUMのTAG_INSTANCEとして返す。
 * @param sign 0以外なら負数として作る(マグニチュードが0の場合は無視され非負になる)
 * @param limbs マグニチュードのlimb配列(呼び出し後は内容を保持しなくてよい、コピーされる)
 * @param count limbsの要素数(先頭の0limbが含まれていても構わない)
 * @return タグ付けされたFIXNUMまたはMAGIC_BIGNUMのINSTANCE
 */
lisp_val_t os_make_integer(int sign, UINT64 *limbs, UINT64 count);

/**
 * limbs(count個、基数2^32、limbs[0]が最下位)を「limbs*mul + add」に置き換える
 * (mul/addは小さい正数を想定)。桁上がり分はlimbs[count]以降に書き込むため、
 * 呼び出し側はその分の容量を確保しておくこと。リーダの10進リテラル桁蓄積で使う。
 * @return 更新後の実効長
 */
UINT64 mag_mul_small_add_small(UINT64 *limbs, UINT64 count, UINT64 mul, UINT64 add);

/**
 * limbs(count個、基数2^32、limbs[0]が最下位)をdiv(小さい正数)で割った商をlimbsに
 * 書き戻し、余りを*remに格納する。プリンタの10進変換で使う。
 * @return 商の実効長
 */
UINT64 mag_divmod_small(UINT64 *limbs, UINT64 count, UINT64 div, UINT64 *rem);

/**
 * cons cellをヒープに確保する。
 * @param car carに入れる値
 * @param cdr cdrに入れる値
 * @return タグ付けされたCONS
 */
lisp_val_t os_make_cons(const lisp_val_t car, const lisp_val_t cdr);

/**
 * name(大文字化される)のsymbolを返す。既に同名のsymbolがinternされていればそれを返す(interning)。
 * @param name symbol名
 * @return タグ付けされたSYMBOL
 */
lisp_val_t os_make_symbol(const char *name);

/**
 * name(大文字化される)の新しいsymbolを、名前の重複チェックもg_symbol_tableへの
 * 登録もせずに作る(gensym用)。word1に非nilのgensymフラグが立つ。
 * @param name symbol名
 * @return タグ付けされたSYMBOL(g_symbol_tableには登録されない)
 */
lisp_val_t os_make_uninterned_symbol(const char *name);

/**
 * symがos_make_uninterned_symbol(gensym)で作られたsymbolかどうかを判定する。
 * @param sym 判定するSYMBOL
 * @return gensym由来なら0以外、通常のinterned symbolなら0
 */
int os_symbol_is_gensym(lisp_val_t sym);

/**
 * g_symbol_tableに現在登録済みのinterned symbol数を返す(テスト・診断用)。
 * os_make_uninterned_symbol(gensym)が作るsymbolはここに含まれない。
 * @return 登録済みsymbol数
 */
int os_symbol_table_count(void);

/**
 * g_symbol_table・global_environment・g_dynamic_bindings・追加GC rootを初期状態に戻す
 * (テスト専用)。1プロセス内でos_heap_init/os_bootstrapを複数回呼び直すユニットテストが、
 * 前回のヒープ世代の状態を持ち込まずに真っ白な状態からブートストラップし直すために使う。
 * os_heap_initの直後、os_bootstrapを呼ぶ前に呼ぶこと。
 */
void os_reset_runtime_state_for_test(void);

/**
 * charオブジェクトを作る(即値、ヒープ確保なし)。
 * @param c 表現する文字
 * @return タグ付けされたCHAR
 */
lisp_val_t os_make_char(const char c);

/**
 * sをコピーしてstringオブジェクトを作る。
 * @param s 文字列(NUL終端)
 * @return タグ付けされたSTRING
 */
lisp_val_t os_make_string(const char *s);

/**
 * STRINGオブジェクトの内容をNUL終端Cバッファへコピーする。
 * out_cap-1バイトを超える分は切り詰める。
 * @param str コピー元のSTRING
 * @param out コピー先バッファ
 * @param out_cap outの容量(NUL終端分を含む)
 */
void os_string_to_cstr(lisp_val_t str, char *out, UINT32 out_cap);

/**
 * TAG_INSTANCE(32byte、magic+3word)のオブジェクトをヒープに確保する。
 * @param magic インスタンスの種別を表すMAGIC NUMBER
 * @param w1 word1に入れる値
 * @param w2 word2に入れる値
 * @param w3 word3に入れる値
 * @return タグ付けされたINSTANCE
 */
lisp_val_t os_make_instance(UINT64 magic, UINT64 w1, UINT64 w2, UINT64 w3);

/**
 * parent_envを親とする新しい環境(name/variables/functions/parent/constantsの5slotを持つリスト)を作る。
 * @param env_symbol 環境の名前を表すsymbol
 * @param parent_env 親環境。ルート環境の場合はnil
 * @return 作成した環境
 */
lisp_val_t os_make_environment(lisp_val_t env_symbol, lisp_val_t parent_env);

/**
 * global_environmentの子として、nameを名前とする新しい環境を生成し、*environments*
 * (init.lispのdefdynamicで定義されるグローバルな環境一覧)へも登録する。
 * os_repl_step/primitive_current_environmentが各プロセスのproc->envを初回呼び出し時に
 * 遅延生成する際に使う共通ヘルパー。os_make_environmentを直接呼ぶだけでは
 * *environments*へ登録されず、list-environmentsからプロセスのREPL環境(F1,F2,...)が
 * 見えなくなるため、登録まで含めてここに一本化する。
 * @param name 新しい環境の名前(proc->nameを渡す想定)
 * @return 生成した環境
 */
lisp_val_t os_make_process_environment(const char *name);

/**
 * symがenvまたはその親のいずれかのconstantsスロットに登録されているかどうかを判定する。
 * defconstantで定義された定数をsetqで上書きできないようにするために使う。
 * os_setq_variableが親を辿って書き込み先を探すため、ネストしたクロージャからの
 * setqも外側スコープの定数保護を素通りしないよう同じ親チェーン探索にしている。
 * @param sym 判定するsymbol
 * @param env 判定を開始する環境
 * @return 定数として登録されていればnon-zero
 */
int os_is_constant(lisp_val_t sym, lisp_val_t env);

/**
 * envのconstantsスロットにsymを定数として登録する(既に登録済みなら何もしない)。
 * @param sym 登録するsymbol
 * @param env 登録先の環境
 */
void os_mark_constant(lisp_val_t sym, lisp_val_t env);

/**
 * g_dynamic_bindingsからsymの動的変数の値を取得する(レキシカルなenvの親子関係とは無関係)。
 * @param sym 検索するsymbol
 * @return 見つかった値。未定義の場合はnil
 */
lisp_val_t os_get_dynamic(lisp_val_t sym);

/**
 * g_dynamic_bindingsにsymの動的変数の値としてvalを設定する(既存なら破壊的に上書き、無ければ新規追加)。
 * @param sym 設定するsymbol
 * @param val 設定する値
 * @return val 自身
 */
lisp_val_t os_set_dynamic(lisp_val_t sym, lisp_val_t val);

/**
 * envおよびその親を順に辿り、symの変数の値を取得する。
 * @param sym 検索するsymbol
 * @param env 検索を開始する環境
 * @return 見つかった値。未定義の場合はnil
 */
lisp_val_t os_get_variable(lisp_val_t sym, lisp_val_t env);

/**
 * envの変数slotにsymの値としてvalを設定する(既存なら破壊的に上書き、無ければ新規追加)。
 * @param sym 設定するsymbol
 * @param val 設定する値
 * @param env 設定先の環境
 * @return val 自身
 */
lisp_val_t os_set_variable(lisp_val_t sym, lisp_val_t val, lisp_val_t env);

/**
 * envから親を順に辿り、既存のsym変数束縛を探して見つかったframeで破壊的に上書きする(setq用)。
 * どのframeにも見つからない場合は、os_set_variableと同じくenvにローカル新規追加する。
 * @param sym 設定するsymbol
 * @param val 設定する値
 * @param env 探索を開始する環境
 * @return val 自身
 */
lisp_val_t os_setq_variable(lisp_val_t sym, lisp_val_t val, lisp_val_t env);

/**
 * envおよびその親を順に辿り、symの関数定義を取得する。
 * @param sym 検索するsymbol
 * @param env 検索を開始する環境
 * @return 見つかった関数オブジェクト。未定義の場合はnil
 */
lisp_val_t os_get_function(lisp_val_t sym, lisp_val_t env);

/**
 * envの関数slotにsymの関数定義としてvalを設定する(既存なら破壊的に上書き、無ければ新規追加)。
 * @param sym 設定するsymbol
 * @param val 設定する関数オブジェクト
 * @param env 設定先の環境
 * @return val 自身
 */
lisp_val_t os_set_function(lisp_val_t sym, lisp_val_t val, lisp_val_t env);

/**
 * envおよびその親を順に辿り、symの関数定義に対応するFunction Cellを取得する。
 * Function CellはImmobilized Space上の固定アドレスに置かれた8byteのセルで、現在の
 * 関数オブジェクトを保持する。cellのアドレス自体は(sym, env)の束縛が存在する間不変
 * だが、中身はos_set_functionが再defunのたびに書き換える。呼び出し側はcellの
 * アドレスだけ握っておけば、中身を読むたびに常に最新の定義を得られる。
 * @param sym 検索するsymbol
 * @param env 検索を開始する環境
 * @return 見つかったFunction Cellを指す、TAG_RAW_POINTER付きのアドレス。未定義の場合はnil
 */
lisp_val_t os_get_function_cell(lisp_val_t sym, lisp_val_t env);

/**
 * os_get_function_cellが返したFunction Cell経由で関数を適用する。cellの中身
 * (現在の関数オブジェクト)を読み出し、os_apply_functionへそのまま委譲する。
 * @param cell os_get_function_cellが返したTAG_RAW_POINTER付きのcellアドレス(nil可)
 * @param evaluated_args 評価済みの引数リスト
 * @param env 呼び出し時の環境
 * @return 関数呼び出しの結果
 */
lisp_val_t os_apply_via_cell(lisp_val_t cell, lisp_val_t evaluated_args, lisp_val_t env);

/**
 * envのpagesスロット(7番目)に、firstから始まるcount個の連続Immobilized Pageの
 * アドレスを1ページ1エントリのフラットなリストとして追加する(TAG_RAW_POINTER
 * タグ付き)。za.cがos_imm_pages_alloc_contiguousで確保したJITコード配置先ページを、
 * コンパイルを実行したenvironmentの所有物として記録するために使う。
 * @param env 登録先の環境
 * @param first_page 確保した先頭ページのアドレス
 * @param count ページ数
 */
void os_environment_register_pages(lisp_val_t env, void *first_page, UINT64 count);

/**
 * envのpagesスロット(7番目)を辿り、登録されている各Immobilized Pageをos_imm_page_free
 * でフリーリストへ返却する。環境破棄時に、そのenvironmentが所有していたJITコード
 * 配置先ページをまとめて回収するために使う(destroy-environmentからの呼び出しは
 * 別フェーズの実装物)。
 * @param env 回収対象の環境
 */
void os_environment_reclaim_pages(lisp_val_t env);

/**
 * envのliteral-slotsスロット(8番目)に、za.c側のリテラルスロットプール
 * (g_za_quote_slots/g_za_number_slots/g_za_lambda_slots)上の1エントリのアドレスを
 * 1件のフラットなリストとして追加する(TAG_RAW_POINTERタグ付き、pagesスロットの
 * 追加パスと同じ考え方)。za.cがコンパイル成功時に、そのコンパイルで確保したスロットを
 * 実行したenvironmentの所有物として記録するために使う(Phase3.6)。
 * @param env 登録先の環境
 * @param slot_addr 登録するスロットのアドレス(g_za_*_slots配列内の1要素へのポインタ)
 */
void os_environment_register_literal_slot(lisp_val_t env, lisp_val_t *slot_addr);

/**
 * envのliteral-slotsスロット(8番目)を辿り、登録されている各アドレスについて
 * free_slotを呼び出してから、スロットを空リストへ戻す。プール自体の構造
 * (どのプールの何番目か、フリーリストへどう返却するか)はza.c側の知識であり、
 * runtime.cはアドレスのライフサイクル(GC root解除含む)をfree_slotへ委譲するだけに
 * とどめる(os_environment_reclaim_pagesがos_imm_page_freeを直接呼べるのは、pagesが
 * runtime.c自身が所有するImmobilized Spaceの資源だからであり、za.c所有のスロット
 * プールについては同じ構造にできないため、コールバック経由にしている)。
 * @param env 回収対象の環境
 * @param free_slot 各アドレスに対して呼ぶ解放コールバック(za_free_literal_slot想定)
 */
void os_environment_reclaim_literal_slots(lisp_val_t env, void (*free_slot)(lisp_val_t *slot_addr));

/**
 * fnptrをネイティブ(C)関数として呼び出すTAG_INSTANCEオブジェクトを作る。
 * @param fnptr 呼び出すC関数のアドレス
 * @return MAGIC_FUNCTION_NATIVEのINSTANCE(word2=NIL、組み込みprimitive扱い)
 */
lisp_val_t os_make_native_function(lisp_addr_t fnptr);

/**
 * fnptrをza.cがコンパイルしたネイティブ(C)関数として呼び出すTAG_INSTANCEオブジェクトを作る。
 * @param fnptr 呼び出すJITコンパイル済み機械語のアドレス
 * @return MAGIC_FUNCTION_NATIVEのINSTANCE(word2=fixnum 1)
 */
lisp_val_t os_make_jit_function(lisp_addr_t fnptr);

/**
 * init.lisp の (make-instance class-sym . initargs) と (signal-condition condition nil) を
 * この順にosApplyFunction経由で呼び出す。sqrt/log(domain-error)やparse-number(parse-error)のように、
 * 生FPU命令やCの文字列走査が必要なため純Lispでは書けないが、条件の発火自体は既存のILOS/
 * コンディションシステム(init.lisp)にそのまま委ねたいプリミティブが使う。
 * @param class_sym signalするコンディションのクラスを表すシンボル(例: g_sym_class_domain_error)
 * @param initargs make-instanceに渡す評価済みの初期化引数リスト(:key val :key val ...)
 * @param env 呼び出し時の環境
 * @return signal-conditionの戻り値(通常はハンドラ経由でトップレベルへabortするため到達しない)。
 *         init.lisp未ロードでmake-instance/signal-conditionが未定義の場合はg_sym_eval_error
 */
lisp_val_t os_signal_condition(lisp_val_t class_sym, lisp_val_t initargs, lisp_val_t env);

/**
 * init.lisp の (%find-class class-name-sym) をosApplyFunction経由で呼び出し、クラスオブジェクトを
 * 得る。signal_domain_error(runtime.c)やos_parse_number(reader.c)がdomain-error/parse-errorの
 * :expected-classスロット値を組み立てるために使う共通ヘルパー。
 * @param class_name_sym 解決したいクラスを表すシンボル(例: g_sym_class_number)
 * @param env 呼び出し時の環境
 * @return 解決されたクラスオブジェクト。init.lisp未ロードで%find-classが未定義の場合はg_sym_eval_error。
 *         %find-class自体が非局所脱出/シグナルを返した場合はそれをそのまま返す
 */
lisp_val_t os_resolve_class(lisp_val_t class_name_sym, lisp_val_t env);

/**
 * 組み込み関数CAR。argsの第一引数のcarを返す。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 第一引数のcar
 */
lisp_val_t primitive_car(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数CDR。argsの第一引数のcdrを返す。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 第一引数のcdr
 */
lisp_val_t primitive_cdr(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数+。argsの全整数(FIXNUM/bignum、負数も可)を合計する。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 合計値の整数(60bit以内ならFIXNUM、それを超えるならbignum)
 */
lisp_val_t primitive_add(lisp_val_t args, lisp_val_t env);

/**
 * primitive_addを2引数固定で呼ぶためのラッパー。JITコンパイル済みコードから呼ばれる想定。
 * @param a 第一オペランド
 * @param b 第二オペランド
 * @return 合計値の整数(60bit以内ならFIXNUM、それを超えるならbignum)
 */
lisp_val_t primitive_add2(lisp_val_t a, lisp_val_t b);

/**
 * 組み込み関数-。argsの第一引数から残りを順に減算する。1引数の場合は単項マイナス(0-x)として符号を反転する。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 減算結果の整数(60bit以内ならFIXNUM、それを超えるならbignum)
 */
lisp_val_t primitive_subtract(lisp_val_t args, lisp_val_t env);

/**
 * primitive_subtractを2引数固定で呼ぶためのラッパー。JITコンパイル済みコードから
 * 呼ばれる想定。
 * @param a 第一オペランド
 * @param b 第二オペランド
 * @return a-b(primitive_subtractと同じ規則で計算)
 */
lisp_val_t primitive_subtract2(lisp_val_t a, lisp_val_t b);

/**
 * 組み込み関数CONS。第一引数をcar、第二引数をcdrとするconsを返す。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 新しく作られたCONS
 */
lisp_val_t primitive_cons(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数EQ。第一引数と第二引数が同一(==)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 同一ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_eq(lisp_val_t args, lisp_val_t env);

/**
 * primitive_eqを2引数固定・非allocatingで呼ぶためのラッパー(za向け)。
 * @param a 第一オペランド
 * @param b 第二オペランド
 * @return aとbが同一ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_eq2(lisp_val_t a, lisp_val_t b);

/**
 * 組み込み関数NULL。第一引数がnilかどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return nilならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_null(lisp_val_t args, lisp_val_t env);

/**
 * primitive_nullを1引数固定・非allocatingで呼ぶためのラッパー(za向け)。
 * @param a オペランド
 * @return aがnilならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_null1(lisp_val_t a);

/**
 * 組み込み関数*。argsの全整数(FIXNUM/bignum、負数も可)を乗算する。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 積の整数(60bit以内ならFIXNUM、それを超えるならbignum)
 */
lisp_val_t primitive_multiply(lisp_val_t args, lisp_val_t env);

/**
 * primitive_multiplyを2引数固定で呼ぶためのラッパー。JITコンパイル済みコードから
 * 呼ばれる想定。
 * @param a 第一オペランド
 * @param b 第二オペランド
 * @return a*b(primitive_multiplyと同じ規則で計算)
 */
lisp_val_t primitive_multiply2(lisp_val_t a, lisp_val_t b);

/**
 * 組み込み関数/。argsの第一引数から残りを順に除算する(整数除算、商のみ返す)。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 除算結果の整数。0除算の場合はg_sym_eval_error
 */
lisp_val_t primitive_divide(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数<。argsが単調増加(a<b<c<...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調増加ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_less_than(lisp_val_t args, lisp_val_t env);

/**
 * primitive_less_thanを2引数固定で呼ぶためのラッパー。JITコンパイル済みコードから
 * 呼ばれる想定。
 * @param a 第一オペランド
 * @param b 第二オペランド
 * @return a<bならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_less_than2(lisp_val_t a, lisp_val_t b);

/**
 * 組み込み関数>。argsが単調減少(a>b>c>...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調減少ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_greater_than(lisp_val_t args, lisp_val_t env);

/**
 * primitive_greater_thanを2引数固定で呼ぶためのラッパー。JITコンパイル済みコードから
 * 呼ばれる想定。
 * @param a 第一オペランド
 * @param b 第二オペランド
 * @return a>bならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_greater_than2(lisp_val_t a, lisp_val_t b);

/**
 * 組み込み関数=。argsがすべて等しいかどうかを判定する。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return すべて等しいならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_num_equal(lisp_val_t args, lisp_val_t env);

/**
 * primitive_num_equalを2引数固定で呼ぶためのラッパー。JITコンパイル済みコードから
 * 呼ばれる想定。
 * @param a 第一オペランド
 * @param b 第二オペランド
 * @return a=bならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_num_equal2(lisp_val_t a, lisp_val_t b);

/**
 * 組み込み関数/=。argsの隣接する要素同士がすべて等しくないかどうかを判定する
 * (厳密な仕様の「全要素が相異なる」ではなく、既存の</=/>と同様に隣接ペア判定に
 * 簡略化している点に注意)。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 隣接ペアがすべて等しくないならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_num_not_equal(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数>=。argsが単調非増加(a>=b>=c>=...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調非増加ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_greater_equal(lisp_val_t args, lisp_val_t env);

/**
 * primitive_greater_equalを2引数固定で呼ぶためのラッパー。JITコンパイル済みコードから
 * 呼ばれる想定。
 * @param a 第一オペランド
 * @param b 第二オペランド
 * @return a>=bならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_greater_equal2(lisp_val_t a, lisp_val_t b);

/**
 * 組み込み関数<=。argsが単調非減少(a<=b<=c<=...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべて整数)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調非減少ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_less_equal(lisp_val_t args, lisp_val_t env);

/**
 * primitive_less_equalを2引数固定で呼ぶためのラッパー。JITコンパイル済みコードから
 * 呼ばれる想定。
 * @param a 第一オペランド
 * @param b 第二オペランド
 * @return a<=bならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_less_equal2(lisp_val_t a, lisp_val_t b);

/**
 * 組み込み関数MAX。argsのうち最大の要素を返す。
 * @param args 評価済みの引数リスト(すべて整数、1個以上)
 * @param env 呼び出し時の環境(未使用)
 * @return 最大の要素
 */
lisp_val_t primitive_max(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数MIN。argsのうち最小の要素を返す。
 * @param args 評価済みの引数リスト(すべて整数、1個以上)
 * @param env 呼び出し時の環境(未使用)
 * @return 最小の要素
 */
lisp_val_t primitive_min(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数ABS。第一引数の絶対値を返す。
 * @param args 評価済みの引数リスト(整数1個)
 * @param env 呼び出し時の環境(未使用)
 * @return 絶対値の整数
 */
lisp_val_t primitive_abs(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数DIV。z1をz2で除した「floor除算」の商を返す(切り捨て除算の/とは異なり、
 * 商は-∞方向へ切り捨てる)。
 * @param args 評価済みの引数リスト(整数2個)
 * @param env 呼び出し時の環境(未使用)
 * @return floor除算の商。z2が0の場合はg_sym_eval_error
 */
lisp_val_t primitive_div(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数MOD。z1をz2で除した「floor除算」の余りを返す(余りの符号は常にz2の符号に
 * 一致する)。
 * @param args 評価済みの引数リスト(整数2個)
 * @param env 呼び出し時の環境(未使用)
 * @return floor除算の余り。z2が0の場合はg_sym_eval_error
 */
lisp_val_t primitive_mod(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数GCD。z1とz2の最大公約数を返す(結果は常に非負)。
 * @param args 評価済みの引数リスト(整数2個)
 * @param env 呼び出し時の環境(未使用)
 * @return 最大公約数(非負整数)
 */
lisp_val_t primitive_gcd(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数LCM。z1とz2の最小公倍数を返す(結果は常に非負)。
 * @param args 評価済みの引数リスト(整数2個)
 * @param env 呼び出し時の環境(未使用)
 * @return 最小公倍数(非負整数)
 */
lisp_val_t primitive_lcm(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数ISQRT。第一引数の整数平方根floor(sqrt(z))を返す。zが負の場合は定義域エラー。
 * @param args 評価済みの引数リスト(非負整数1個)
 * @param env 呼び出し時の環境(未使用)
 * @return floor(sqrt(z))。zが負の場合はg_sym_eval_error
 */
lisp_val_t primitive_isqrt(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数NUMBERP。第一引数が数値(FIXNUMまたはbignum)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 数値ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_numberp(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数FIXNUMP。第一引数がFIXNUMかどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return FIXNUMならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_fixnump(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数BIGNUMP。第一引数が60bitを超える整数(bignum、MAGIC_BIGNUMのINSTANCE)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return bignumならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_bignump(lisp_val_t args, lisp_val_t env);

/**
 * doubleの値をMAGIC_FLOATのINSTANCEとしてヒープに確保する。word1にdoubleの
 * ビットパターンをそのまま格納し、word2/word3は未使用。
 * @param value 格納するdouble値
 * @return MAGIC_FLOATのINSTANCE
 */
lisp_val_t os_make_float(double value);

/**
 * MAGIC_FLOATのINSTANCEからword1のビットパターンを読み出し、doubleへ戻す。
 * @param val MAGIC_FLOATのINSTANCE(タグ付きlisp_val_t)
 * @return 格納されているdouble値
 */
double os_float_value(lisp_val_t val);

/**
 * bignum(MAGIC_BIGNUMのINSTANCE)をdoubleへ変換する。limb配列を上位から
 * result = result * 4294967296.0 + limb で積算し、最後に符号を適用する
 * (精度はdoubleの53bit仮数部に切り詰められる)。
 * @param val MAGIC_BIGNUMのINSTANCE(タグ付きlisp_val_t)
 * @return doubleへ変換した値
 */
double bignum_to_double(lisp_val_t val);

/**
 * 組み込み関数FLOATP。第一引数がfloat(MAGIC_FLOATのINSTANCE)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return floatならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_floatp(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数FLOAT。第一引数を(既にfloatならそのまま、FIXNUM/bignumならdoubleへ変換して)floatとして返す。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(未使用)
 * @return floatに変換した値。数値以外が渡された場合はg_sym_eval_error
 */
lisp_val_t primitive_float(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数SQRT。第一引数の平方根を返す。完全平方の整数はそのまま整数、
 * それ以外はfloatで返す。負数はdomain-error。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(signal_domain_error経由でinit.lispのsignal-conditionを呼ぶ)
 * @return 平方根。負数が渡された場合はsignal_domain_errorの戻り値
 */
lisp_val_t primitive_sqrt(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数LOG。第一引数の自然対数を返す。0以下はdomain-error。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(signal_domain_error経由でinit.lispのsignal-conditionを呼ぶ)
 * @return xの自然対数。0以下が渡された場合はsignal_domain_errorの戻り値
 */
lisp_val_t primitive_log(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数EXP。第一引数を指数とするe(自然対数の底)の累乗を返す。定義域制約は無い。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(未使用)
 * @return e^x
 */
lisp_val_t primitive_exp(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数SIN。第一引数(ラジアン)の正弦を返す。定義域制約は無い。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(未使用)
 * @return sin(x)
 */
lisp_val_t primitive_sin(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数COS。第一引数(ラジアン)の余弦を返す。定義域制約は無い。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(未使用)
 * @return cos(x)
 */
lisp_val_t primitive_cos(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数ATAN2。(atan2 y x) で atan(y/x) を、xの符号を使って正しい象限で返す。
 * 定義域制約は無い。
 * @param args 評価済みの引数リスト(数値2個、第一引数がy、第二引数がx)
 * @param env 呼び出し時の環境(未使用)
 * @return atan2(y, x)
 */
lisp_val_t primitive_atan2(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数FLOOR。第一引数以下の最大の整数を返す。定義域制約は無い。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(未使用)
 * @return floor(x)
 */
lisp_val_t primitive_floor(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数CEILING。第一引数以上の最小の整数を返す。定義域制約は無い。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(未使用)
 * @return ceiling(x)
 */
lisp_val_t primitive_ceiling(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数TRUNCATE。第一引数の小数部を切り捨てた整数を返す。定義域制約は無い。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(未使用)
 * @return truncate(x)
 */
lisp_val_t primitive_truncate(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数ROUND。第一引数を最も近い整数に丸める(ties-to-even)。定義域制約は無い。
 * @param args 評価済みの引数リスト(数値1個)
 * @param env 呼び出し時の環境(未使用)
 * @return round(x)
 */
lisp_val_t primitive_round(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数PARSE-NUMBER。第一引数のSTRINGを数値として読み取る。数値の字句として
 * 解釈できない場合は<parse-error>をsignalする。
 * @param args 評価済みの引数リスト(第一引数はSTRING)
 * @param env 呼び出し時の環境(<parse-error>のsignal-conditionに使う)
 * @return 解析された数値
 */
lisp_val_t primitive_parse_number(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数SYMBOLP。第一引数がsymbolかどうかを判定する。
 * nilはTAG_CONS(自己参照cons)で表現されているが、ISLisp上はsymbolとして扱われるため
 * val == nil も真と判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return symbolならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_symbolp(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数CONSP。第一引数がconsかどうかを判定する。
 * nilは内部表現上TAG_CONSだが、ISLisp上はconsではないため val == nil は偽と判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return consならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_consp(lisp_val_t args, lisp_val_t env);

/**
 * primitive_conspの論理否定にあたる非allocatingな核ロジック(za向け)。nilはconsでは
 * ないためatomとして扱う。
 * @param val 判定対象
 * @return consでなければg_sym_t、consならnil
 */
lisp_val_t primitive_atom1(lisp_val_t val);

/**
 * 組み込み関数ATOM。第一引数がconsでないかどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return consでなければg_sym_t、consならnil
 */
lisp_val_t primitive_atom(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数EQL。第一引数と第二引数が同一かどうかを判定する。
 * 仕様上eqとの違いは数値(同クラスかつ同値)と文字(同文字)の比較。fixnum/charは即値表現
 * (同じ論理値なら同じビットパタン)なのでeqのポインタ比較のままで正しく判定できるが、
 * bignumは同じ値でも異なるヒープオブジェクトになりうるため、両者がMAGIC_BIGNUMの場合は
 * sign+limb内容を比較する(浮動小数点数は未実装のため、それ以外はeqと同じ判定になる)。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 同一ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_eql(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数EQUAL。第一引数と第二引数の構造的な同値性を判定する。
 * CONS/STRING/VECTORは再帰的に内容を比較し、両者がbignum(MAGIC_BIGNUM)ならsign+limb内容を
 * 比較し、それ以外はeqと同じ判定にフォールバックする。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 構造的に同値ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_equal(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数LISTP。第一引数がlist(nilまたはcons。ドットリストも含む)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return listならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_listp(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数CHARACTERP。第一引数がcharacterかどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return characterならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_characterp(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数CHAR=。argsがすべて同じ文字かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてCHAR)
 * @param env 呼び出し時の環境(未使用)
 * @return すべて等しいならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_char_equal(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数CHAR/=。argsの隣接する要素同士がすべて等しくないかどうかを判定する
 * (厳密な仕様の「全要素が相異なる」ではなく、既存の数値比較の/=と同様に隣接ペア判定に
 * 簡略化している点に注意)。
 * @param args 評価済みの引数リスト(すべてCHAR)
 * @param env 呼び出し時の環境(未使用)
 * @return 隣接ペアがすべて等しくないならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_char_not_equal(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数CHAR<。argsが単調増加(a<b<c<...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてCHAR)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調増加ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_char_less_than(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数CHAR>。argsが単調減少(a>b>c>...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてCHAR)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調減少ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_char_greater_than(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数CHAR<=。argsが単調非減少(a<=b<=c<=...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてCHAR)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調非減少ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_char_less_equal(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数CHAR>=。argsが単調非増加(a>=b>=c>=...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてCHAR)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調非増加ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_char_greater_equal(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数STRINGP。第一引数がstringかどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return stringならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_stringp(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数FUNCTIONP。第一引数が関数(MAGIC_FUNCTION_NATIVEまたはMAGIC_FUNCTION_INTERPRETED)
 * かどうかを判定する。MAGIC_MACROは関数ではないため偽と判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 関数ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_functionp(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%ZA-COMPILED-P。第一引数がza.cによって機械語へJITコンパイルされた関数
 * (MAGIC_FUNCTION_NATIVEでword2がfixnum)かどうかを判定する。テスト用の内部primitiveの
 * ため%%を付ける(ISLisp仕様外)。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return JITコンパイル済みならg_sym_t、インタプリタ実行(またはそれ以外)ならnil
 */
lisp_val_t primitive_za_compiled_p(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%MAKE-ENVIRONMENT。os_make_environmentをそのままLispへ公開する薄いラッパー
 * (documents/environment.md Phase4)。特殊形式ではなく通常の関数として実装するため、
 * 名前(第一引数)はシンボルとして評価される(呼び出し側でquoteする)。テスト用の内部
 * primitiveのため%%を付ける(ISLisp仕様外、Lisp側のmake-environmentがラップする)。
 * @param args 評価済みの引数リスト(第一引数: 環境名symbol、第二引数: 親環境)
 * @param env 呼び出し時の環境(未使用)
 * @return 新規作成した環境
 */
lisp_val_t primitive_make_environment(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%CURRENT-ENVIRONMENT。get_current_process()->envを返す
 * (documents/environment.md Phase4)。envはproc->env==0(未初期化を表すセンチネル、
 * process.c:156)の場合、repl.c os_repl_step:19-20と同じロジックでここで遅延生成する。
 * cc_load経由の実行(make test-qemuのboot-entryスクリプト等)はos_repl_stepを一度も
 * 経由しないため、この遅延生成が無いと生の整数0がそのままLisp側へ漏れてしまう
 * (0はTAG_FIXNUMのfixnum 0であり、nilとは異なる値のため後続の環境操作が誤動作する)。
 * @param args 評価済みの引数リスト(未使用)
 * @param env 呼び出し時の環境(未使用、レキシカルなenvであり現在のprocessの実行環境とは別物)
 * @return 現在のprocessの実行環境
 */
lisp_val_t primitive_current_environment(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%GLOBAL-ENVIRONMENT。全プロセス共通のroot environmentである
 * global_environment(C側グローバル、runtime.c:212)をそのまま返す。
 * primitive_current_environmentが返すproc->env(プロセスごとに異なる子env)とは
 * 別物であり、呼び出し元のprocessや呼び出し時のenvに関わらず常に同じ値を返す。
 * init.lisp等をglobal_environmentへ直接loadしたい場合、
 * (with-environment (%%global-environment) (load ...))のように使う想定
 * (documents/environment.md管轄外の変更のため、この用途はコード側コメントにのみ記す)。
 * @param args 評価済みの引数リスト(未使用)
 * @param env 呼び出し時の環境(未使用)
 * @return global_environment
 */
lisp_val_t primitive_global_environment(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%SET-CURRENT-ENVIRONMENT。get_current_process()->envを第一引数へ切り替える
 * (documents/environment.md Phase4)。get_current_process()->envはprocess初期化時に
 * os_gc_register_rootでルート登録済み(プロセス構造体自身のフィールドアドレスを指すため、
 * 値の書き換えはGCに対して安全)。
 * @param args 評価済みの引数リスト(第一引数: 切り替え先の環境)
 * @param env 呼び出し時の環境(未使用)
 * @return 切り替え先の環境(第一引数そのまま)
 */
lisp_val_t primitive_set_current_environment(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%EVAL-IN-ENVIRONMENT。第一引数のformを第二引数のenvのもとで評価する
 * (documents/environment.md Phase4)。with-environment(Lisp、init.lisp)がbodyを
 * 対象環境で実際に評価するために使う: progn/let等のC実装はいずれも呼び出し元から
 * 渡されたenv引数をそのまま子フォームへレキシカルに伝播するだけで、
 * %%set-current-environmentによるproc->envの書き換えを一切参照しないため、
 * bodyを(progn ...)へまとめて%%set-current-environmentするだけでは対象環境での
 * 評価にならない(呼び出し元のマクロ展開時点のレキシカルenvで評価されたままになる)。
 * @param args 評価済みの引数リスト(第一引数: 未評価のform、呼び出し側でquote済み。
 *             第二引数: 評価対象の環境)
 * @param env 呼び出し時の環境(未使用、formの評価には第二引数のenvを使う)
 * @return formを第二引数のenvのもとで評価した結果
 */
lisp_val_t primitive_eval_in_environment(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数GENERIC-FUNCTION-P。本実装にはdefgeneric/defmethodが存在せず
 * generic function自体を表すオブジェクトが作れないため、常にnilを返す。
 * @param args 評価済みの引数リスト(未使用)
 * @param env 呼び出し時の環境(未使用)
 * @return 常にnil
 */
lisp_val_t primitive_generic_function_p(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数BASIC-ARRAY-P。第一引数がbasic-array(MAGIC_VECTORまたはTAG_STRING)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return basic-arrayならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_basic_array_p(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数BASIC-ARRAY*-P / GENERAL-ARRAY*-P。第一引数がrank!=1のMAGIC_VECTORかどうかを
 * 判定する。本実装では特殊化した配列型(bit-vector等)の区別が無く両クラスの外延が一致するため、
 * 同じ実体を両方のシンボルに登録して共用する(NULLをNOTとして共用するのと同じパタン)。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return rank!=1のVECTORならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_array_star_p(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数BASIC-VECTOR-P。第一引数がbasic-vector(rank==1のMAGIC_VECTORまたはTAG_STRING)
 * かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return basic-vectorならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_basic_vector_p(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数GENERAL-VECTOR-P。第一引数がgeneral-vector(rank==1のMAGIC_VECTOR)かどうかを
 * 判定する。STRINGはbasic-vectorだがgeneral-vectorではないため除外する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return general-vectorならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_general_vector_p(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数STREAMP。第一引数がstream(MAGIC_STREAM)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return streamならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_streamp(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数SYMBOL-NAME。第一引数のsymbolの名前をSTRINGとして返す。
 * @param args 評価済みの引数リスト(第一引数はSYMBOL)
 * @param env 呼び出し時の環境(未使用)
 * @return symbol名のSTRING
 */
lisp_val_t primitive_symbol_name(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数STRING-TO-SYMBOL。第一引数のSTRINGをsymbol名としてintern(既存の大文字化ルール)する。
 * @param args 評価済みの引数リスト(第一引数はSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return internされたSYMBOL
 */
lisp_val_t primitive_string_to_symbol(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数GENSYM。呼ぶたびに"G"+連番の名前で新しいuninterned symbol
 * (os_make_uninterned_symbol)を作って返す。g_symbol_tableへの登録は行わない。
 * @param args 未使用
 * @param env 呼び出し時の環境(未使用)
 * @return 新しく作られたuninterned SYMBOL
 */
lisp_val_t primitive_gensym(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数MAKE-ARRAY。第一引数の次元(FIXNUM、またはFIXNUMのリスト)を持つ
 * 多次元配列を確保する。要素はすべてnilで初期化される。
 * @param args 評価済みの引数リスト(第一引数はFIXNUMまたはFIXNUMのリスト)
 * @param env 呼び出し時の環境(未使用)
 * @return 確保したVECTOR
 */
lisp_val_t primitive_make_array(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数AREF。第一引数の配列から、残りの引数(各次元の添字)が指す要素を返す。
 * @param args 評価済みの引数リスト(第一引数はVECTOR、残りはFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 添字が指す要素。範囲外の添字が指定された場合はg_sym_eval_error
 */
lisp_val_t primitive_aref(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数ARRAY-DIMENSIONS。第一引数の配列の各次元のサイズをリストで返す。
 * @param args 評価済みの引数リスト(第一引数はVECTOR)
 * @param env 呼び出し時の環境(未使用)
 * @return 各次元のサイズ(FIXNUM)のリスト
 */
lisp_val_t primitive_array_dimensions(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数SET-CAR。第一引数のconsのcarを第二引数で破壊的に書き換える。
 * @param args 評価済みの引数リスト(第一引数はCONS)
 * @param env 呼び出し時の環境(未使用)
 * @return 書き込んだ値(第二引数)
 */
lisp_val_t primitive_set_car(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数SET-CDR。第一引数のconsのcdrを第二引数で破壊的に書き換える。
 * @param args 評価済みの引数リスト(第一引数はCONS)
 * @param env 呼び出し時の環境(未使用)
 * @return 書き込んだ値(第二引数)
 */
lisp_val_t primitive_set_cdr(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数SET-AREF。第一引数の配列の、続く添字が指す要素を最後の引数で破壊的に書き換える。
 * @param args 評価済みの引数リスト(array idx1 idx2 ... value の並び)
 * @param env 呼び出し時の環境(未使用)
 * @return 書き込んだ値(最後の引数)。範囲外の添字が指定された場合はg_sym_eval_error
 */
lisp_val_t primitive_set_aref(lisp_val_t args, lisp_val_t env);

/**
 * listの各要素をそのままデータとするrank1のVECTOR(MAGIC_VECTOR)を構築する。
 * 組み込み関数VECTORとreader.cの`#(...)`リテラルの両方から使う共通コンストラクタ。
 * @param list 要素を並べたconsリスト
 * @return 構築したVECTOR
 */
lisp_val_t os_make_vector_from_list(lisp_val_t list);

/**
 * VECTOR(TAG_INSTANCE+MAGIC_VECTOR)から、内部の可変長ブロック(rank+dims+data)の
 * 先頭へのポインタを取り出す。print.c等、runtime.c外からVECTORの内部表現に
 * アクセスする必要がある箇所向けの公開版。
 * @param vec VECTOR
 * @return ブロック先頭へのポインタ(word0=rank, word[1..rank]=各次元のサイズ, word[rank+1..]=データ)
 */
lisp_val_t *os_vector_header(lisp_val_t vec);

/**
 * 組み込み関数VECTOR。評価済みの引数列をそのまま要素とするrank1のgeneral-vectorを返す。
 * @param args 評価済みの引数リスト(すべて要素として使う)
 * @param env 呼び出し時の環境(未使用)
 * @return 構築したVECTOR
 */
lisp_val_t primitive_vector(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数CREATE-VECTOR。第一引数の長さ(FIXNUM)のgeneral-vectorを確保する。
 * 第二引数(省略可)を指定すると全要素をその値で初期化する(省略時はnil)。
 * @param args 評価済みの引数リスト(第一引数はFIXNUM、第二引数は省略可)
 * @param env 呼び出し時の環境(未使用)
 * @return 確保したVECTOR
 */
lisp_val_t primitive_create_vector(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数CREATE-STRING。第一引数の長さ(FIXNUM)のSTRINGを確保する。
 * 第二引数(省略可、CHAR)を指定すると全要素をその文字で初期化する(省略時は空白)。
 * @param args 評価済みの引数リスト(第一引数はFIXNUM、第二引数は省略可のCHAR)
 * @param env 呼び出し時の環境(未使用)
 * @return 確保したSTRING
 */
lisp_val_t primitive_create_string(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数STRING-ELT。第一引数のSTRINGの第二引数(0起算)番目の文字を返す。
 * @param args 評価済みの引数リスト(第一引数はSTRING、第二引数はFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 添字が指す文字(CHAR)。範囲外の添字が指定された場合はg_sym_eval_error
 */
lisp_val_t primitive_string_elt(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数STRING=。argsがすべて同じ文字列かどうかを判定する。
 * CHAR=同様、仕様上は2引数だが本実装では隣接ペア連鎖のN項関数として実装する。
 * @param args 評価済みの引数リスト(すべてSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return すべて等しいならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_string_equal(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数STRING/=。argsの隣接する要素同士がすべて等しくないかどうかを判定する
 * (CHAR/=と同様、隣接ペア判定に簡略化している)。
 * @param args 評価済みの引数リスト(すべてSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return 隣接ペアがすべて等しくないならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_string_not_equal(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数STRING<。argsが単調増加(a<b<c<...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調増加ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_string_less_than(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数STRING>。argsが単調減少(a>b>c>...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調減少ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_string_greater_than(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数STRING<=。argsが単調非減少(a<=b<=c<=...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調非減少ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_string_less_equal(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数STRING>=。argsが単調非増加(a>=b>=c>=...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調非増加ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_string_greater_equal(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数CHAR-INDEX。第二引数のSTRING中で第一引数のCHARが最初に現れる位置を
 * 第三引数(省略可、FIXNUM、省略時0)から探して返す。見つからなければnil。
 * @param args 評価済みの引数リスト(CHAR, STRING, [FIXNUM])
 * @param env 呼び出し時の環境(未使用)
 * @return 見つかった位置(FIXNUM)、見つからなければnil
 */
lisp_val_t primitive_char_index(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数STRING-INDEX。第二引数のSTRING中で第一引数のSTRING(部分文字列)が
 * 最初に現れる位置を第三引数(省略可、FIXNUM、省略時0)から探して返す。
 * 見つからなければnil。空文字列は探索開始位置に即マッチする。
 * @param args 評価済みの引数リスト(STRING(部分文字列), STRING, [FIXNUM])
 * @param env 呼び出し時の環境(未使用)
 * @return 見つかった位置(FIXNUM)、見つからなければnil
 */
lisp_val_t primitive_string_index(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数STRING-APPEND。argsの各STRINGを連結した新しいSTRINGを返す
 * (引数が無ければ空文字列)。
 * @param args 評価済みの引数リスト(すべてSTRING)
 * @param env 呼び出し時の環境(未使用)
 * @return 連結結果のSTRING
 */
lisp_val_t primitive_string_append(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数LENGTH。第一引数のシーケンス(LIST/STRING/VECTOR)の要素数を返す。
 * VECTORの場合は次元に関わらず全要素数(各次元のサイズの積)を返す。
 * @param args 評価済みの引数リスト(第一引数はLIST/STRING/VECTOR)
 * @param env 呼び出し時の環境(未使用)
 * @return 要素数(FIXNUM)
 */
lisp_val_t primitive_length(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数ELT。第一引数のシーケンス(LIST/STRING/VECTOR)の第二引数(0起算)
 * 番目の要素を返す。
 * @param args 評価済みの引数リスト(第一引数はLIST/STRING/VECTOR、第二引数はFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 添字が指す要素。範囲外の添字が指定された場合はg_sym_eval_error
 */
lisp_val_t primitive_elt(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数SET-ELT。第二引数のシーケンス(LIST/STRING/VECTOR)の第三引数(0起算)
 * 番目の要素を第一引数で破壊的に書き換える。仕様上「新しい値が最初」という引数順
 * である点に注意(SET-AREF/SET-CAR/SET-CDRとは逆順)。
 * @param args 評価済みの引数リスト(第一引数は新しい値、第二引数はLIST/STRING/VECTOR、
 *             第三引数はFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 書き込んだ値(第一引数)。範囲外の添字が指定された場合はg_sym_eval_error
 */
lisp_val_t primitive_set_elt(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数SUBSEQ。第一引数のシーケンス(LIST/STRING/VECTOR)の[z1, z2)の
 * 範囲を要素とする、同じクラスの新規シーケンスを返す。
 * @param args 評価済みの引数リスト(第一引数はLIST/STRING/VECTOR、第二・第三引数は
 *             FIXNUM(z1, z2))
 * @param env 呼び出し時の環境(未使用)
 * @return 新規に確保したシーケンス(元と同じクラス)
 */
lisp_val_t primitive_subseq(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%MAKE-CLASS-RAW。ILOSクラスオブジェクト(MAGIC_STANDARD_CLASS)を確保する。
 * @param args 評価済みの引数リスト(name symbol, supers クラスオブジェクトのlist, slots スロット記述子のlist)
 * @param env 呼び出し時の環境(未使用)
 * @return 確保したクラスオブジェクト
 */
lisp_val_t primitive_make_class_raw(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%MAKE-BUILTIN-CLASS-RAW。ILOSクラスオブジェクト(MAGIC_BUILTIN_CLASS)を確保する。
 * @param args 評価済みの引数リスト(name symbol, supers クラスオブジェクトのlist, slots スロット記述子のlist)
 * @param env 呼び出し時の環境(未使用)
 * @return 確保したクラスオブジェクト
 */
lisp_val_t primitive_make_builtin_class_raw(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%CLASS-NAME。ILOSクラスオブジェクトのname(symbol)を返す。
 * @param args 評価済みの引数リスト(第一引数はクラスオブジェクト)
 * @param env 呼び出し時の環境(未使用)
 * @return name(symbol)
 */
lisp_val_t primitive_class_name(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%CLASS-SUPERS。ILOSクラスオブジェクトのsuperclasses(クラスオブジェクトのlist)を返す。
 * @param args 評価済みの引数リスト(第一引数はクラスオブジェクト)
 * @param env 呼び出し時の環境(未使用)
 * @return superclasses(クラスオブジェクトのlist)
 */
lisp_val_t primitive_class_supers(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%CLASS-SLOTS。ILOSクラスオブジェクトのslots(スロット記述子のlist、継承分含む)を返す。
 * @param args 評価済みの引数リスト(第一引数はクラスオブジェクト)
 * @param env 呼び出し時の環境(未使用)
 * @return slots(スロット記述子のlist)
 */
lisp_val_t primitive_class_slots(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%CLASSP。第一引数がILOSクラスオブジェクト(MAGIC_BUILTIN_CLASSまたはMAGIC_STANDARD_CLASS)かどうかを判定する。
 * @param args 評価済みの引数リスト(第一引数は任意の値)
 * @param env 呼び出し時の環境(未使用)
 * @return クラスオブジェクトならt、それ以外ならnil
 */
lisp_val_t primitive_classp(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%BUILTIN-CLASSP。第一引数がILOSの組み込みクラスオブジェクト(MAGIC_BUILTIN_CLASS)かどうかを判定する。
 * @param args 評価済みの引数リスト(第一引数は任意の値)
 * @param env 呼び出し時の環境(未使用)
 * @return 組み込みクラスオブジェクトならt、それ以外ならnil
 */
lisp_val_t primitive_builtin_classp(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%STANDARD-CLASSP。第一引数がILOSの標準クラスオブジェクト(MAGIC_STANDARD_CLASS)かどうかを判定する。
 * @param args 評価済みの引数リスト(第一引数は任意の値)
 * @param env 呼び出し時の環境(未使用)
 * @return 標準クラスオブジェクトならt、それ以外ならnil
 */
lisp_val_t primitive_standard_classp(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%MAKE-INSTANCE-RAW。ILOSクラスインスタンス(MAGIC_CLASS_INSTANCE)を確保する。
 * @param args 評価済みの引数リスト(class クラスオブジェクト, slots-vector MAGIC_VECTOR)
 * @param env 呼び出し時の環境(未使用)
 * @return 確保したクラスインスタンス
 */
lisp_val_t primitive_make_instance_raw(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%INSTANCE-CLASS。ILOSクラスインスタンスのclass(クラスオブジェクト)を返す。
 * @param args 評価済みの引数リスト(第一引数はクラスインスタンス)
 * @param env 呼び出し時の環境(未使用)
 * @return class(クラスオブジェクト)
 */
lisp_val_t primitive_instance_class(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%INSTANCE-SLOTS。ILOSクラスインスタンスのslots-vector(MAGIC_VECTOR)を返す。
 * @param args 評価済みの引数リスト(第一引数はクラスインスタンス)
 * @param env 呼び出し時の環境(未使用)
 * @return slots-vector(MAGIC_VECTOR)
 */
lisp_val_t primitive_instance_slots(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%CLASS-INSTANCE-P。第一引数がILOSクラスインスタンス(MAGIC_CLASS_INSTANCE)かどうかを判定する。
 * @param args 評価済みの引数リスト(第一引数は任意の値)
 * @param env 呼び出し時の環境(未使用)
 * @return クラスインスタンスならt、それ以外ならnil
 */
lisp_val_t primitive_class_instance_p(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%SET-DYNAMIC。os_set_dynamicの薄いラッパーで、レキシカルなenvの
 * 親子関係とは無関係なグローバルの動的変数(defdynamicで定義したもの)を、
 * 関数呼び出しの内側からでも書き換えられるようにする。
 * @param args (name value) 評価済みの引数リスト。nameは動的変数名のシンボル
 * @param env 呼び出し時の環境(未使用)
 * @return 書き込んだvalue
 */
lisp_val_t primitive_set_dynamic(lisp_val_t args, lisp_val_t env);

/**
 * nバイト(8byte境界に整列)をLispヒープからアロケータ経由で確保する、os_alloc_bytesの公開版。
 * stream_lisp.cがos_stream_tをLispヒープ上に確保するために使う。
 * @param n 確保するバイト数
 * @return 確保したメモリの先頭アドレス
 */
lisp_addr_t os_alloc_raw(UINT64 n);

#endif /* _RUNTIME_H_ */

