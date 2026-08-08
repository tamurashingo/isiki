#ifndef _RUNTIME_H_
#define _RUNTIME_H_

#include "types.h"

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
/** 多次元配列(general array)へのアドレス(110) */
#define TAG_VECTOR   0x6ULL


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
/** TAG_INSTANCEのword0に入る、ILOSのクラスオブジェクトであることを示すMAGIC NUMBER。word1=name(symbol)、word2=superclasses(クラスオブジェクトのlist)、word3=slots(スロット記述子のlist、継承分含む) */
#define MAGIC_CLASS                0x7ULL
/** TAG_INSTANCEのword0に入る、ILOSのクラスインスタンスであることを示すMAGIC NUMBER。word1=class、word2=slots-vector(TAG_VECTOR)、word3=未使用 */
#define MAGIC_CLASS_INSTANCE       0x8ULL
/** TAG_INSTANCEのword0に入る、catch/throwの非局所脱出シグナルであることを示すMAGIC NUMBER。word1=tag(evalされた値)、word2=throwされた値 */
#define MAGIC_CATCH_EXIT           0x9ULL
/** TAG_INSTANCEのword0に入る、tagbody/goの非局所脱出シグナルであることを示すMAGIC NUMBER。word1=tag(未評価のsymbol) */
#define MAGIC_GO_EXIT              0xAULL

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
 * fixnumオブジェクトを作る(即値、ヒープ確保なし)。
 * @param fixnum 表現する値
 * @return タグ付けされたFIXNUM
 */
lisp_val_t os_make_fixnum(const UINT64 fixnum);

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
 * symがenv自身(親は辿らない)のconstantsスロットに登録されているかどうかを判定する。
 * defconstantで定義された定数をsetqで上書きできないようにするために使う。
 * @param sym 判定するsymbol
 * @param env 判定対象の環境(このenv自身のスロットのみを見る)
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
 * fnptrをネイティブ(C)関数として呼び出すTAG_INSTANCEオブジェクトを作る。
 * @param fnptr 呼び出すC関数のアドレス
 * @return MAGIC_FUNCTION_NATIVEのINSTANCE
 */
lisp_val_t os_make_native_function(lisp_addr_t fnptr);

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
 * 組み込み関数+。argsの全fixnumを合計する。
 * @param args 評価済みの引数リスト(すべてFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 合計値のFIXNUM
 */
lisp_val_t primitive_add(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数-。argsの第一引数から残りを順に減算する(単項の符号反転は未サポート)。
 * @param args 評価済みの引数リスト(すべてFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 減算結果のFIXNUM
 */
lisp_val_t primitive_subtract(lisp_val_t args, lisp_val_t env);

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
 * 組み込み関数NULL。第一引数がnilかどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return nilならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_null(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数*。argsの全fixnumを乗算する。
 * @param args 評価済みの引数リスト(すべてFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 積のFIXNUM
 */
lisp_val_t primitive_multiply(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数/。argsの第一引数から残りを順に除算する(整数除算)。
 * @param args 評価済みの引数リスト(すべてFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 除算結果のFIXNUM。0除算の場合はg_sym_eval_error
 */
lisp_val_t primitive_divide(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数<。argsが単調増加(a<b<c<...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調増加ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_less_than(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数>。argsが単調減少(a>b>c>...)かどうかを判定する。
 * @param args 評価済みの引数リスト(すべてFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return 単調減少ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_greater_than(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数=。argsがすべて等しいかどうかを判定する。
 * @param args 評価済みの引数リスト(すべてFIXNUM)
 * @param env 呼び出し時の環境(未使用)
 * @return すべて等しいならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_num_equal(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数NUMBERP。第一引数が数値(現状はFIXNUMのみ)かどうかを判定する。
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
 * 組み込み関数EQL。第一引数と第二引数が同一かどうかを判定する。
 * 仕様上eqとの違いは数値(同クラスかつ同値)と文字(同文字)の比較だが、本実装のfixnum/charは
 * いずれも即値表現(同じ論理値なら同じビットパタン)であり、かつ浮動小数点数が未実装のため、
 * 現状ではeqと完全に同じ判定になる(将来floatを追加する際に差が出る想定でeqとは別実装にしている)。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return 同一ならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_eql(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数EQUAL。第一引数と第二引数の構造的な同値性を判定する。
 * CONS/STRING/VECTORは再帰的に内容を比較し、それ以外はeqと同じ判定にフォールバックする。
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
 * 組み込み関数GENERIC-FUNCTION-P。本実装にはdefgeneric/defmethodが存在せず
 * generic function自体を表すオブジェクトが作れないため、常にnilを返す。
 * @param args 評価済みの引数リスト(未使用)
 * @param env 呼び出し時の環境(未使用)
 * @return 常にnil
 */
lisp_val_t primitive_generic_function_p(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数BASIC-ARRAY-P。第一引数がbasic-array(TAG_VECTORまたはTAG_STRING)かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return basic-arrayならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_basic_array_p(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数BASIC-ARRAY*-P / GENERAL-ARRAY*-P。第一引数がrank!=1のTAG_VECTORかどうかを
 * 判定する。本実装では特殊化した配列型(bit-vector等)の区別が無く両クラスの外延が一致するため、
 * 同じ実体を両方のシンボルに登録して共用する(NULLをNOTとして共用するのと同じパタン)。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return rank!=1のVECTORならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_array_star_p(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数BASIC-VECTOR-P。第一引数がbasic-vector(rank==1のTAG_VECTORまたはTAG_STRING)
 * かどうかを判定する。
 * @param args 評価済みの引数リスト
 * @param env 呼び出し時の環境(未使用)
 * @return basic-vectorならg_sym_t、そうでなければnil
 */
lisp_val_t primitive_basic_vector_p(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数GENERAL-VECTOR-P。第一引数がgeneral-vector(rank==1のTAG_VECTOR)かどうかを
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
 * 組み込み関数GENSYM。呼ぶたびに"G"+連番の名前で新しいsymbolをintern して返す
 * (真の非intern symbolは未サポート。連番が一巡しない限り重複は起きない)。
 * @param args 未使用
 * @param env 呼び出し時の環境(未使用)
 * @return 新しくinternされたSYMBOL
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
 * 組み込み関数LENGTH。第一引数のシーケンス(LIST/STRING/VECTOR)の要素数を返す。
 * VECTORの場合は次元に関わらず全要素数(各次元のサイズの積)を返す。
 * @param args 評価済みの引数リスト(第一引数はLIST/STRING/VECTOR)
 * @param env 呼び出し時の環境(未使用)
 * @return 要素数(FIXNUM)
 */
lisp_val_t primitive_length(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%MAKE-CLASS-RAW。ILOSクラスオブジェクト(MAGIC_CLASS)を確保する。
 * @param args 評価済みの引数リスト(name symbol, supers クラスオブジェクトのlist, slots スロット記述子のlist)
 * @param env 呼び出し時の環境(未使用)
 * @return 確保したクラスオブジェクト
 */
lisp_val_t primitive_make_class_raw(lisp_val_t args, lisp_val_t env);

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
 * 組み込み関数%%CLASSP。第一引数がILOSクラスオブジェクト(MAGIC_CLASS)かどうかを判定する。
 * @param args 評価済みの引数リスト(第一引数は任意の値)
 * @param env 呼び出し時の環境(未使用)
 * @return クラスオブジェクトならt、それ以外ならnil
 */
lisp_val_t primitive_classp(lisp_val_t args, lisp_val_t env);

/**
 * 組み込み関数%%MAKE-INSTANCE-RAW。ILOSクラスインスタンス(MAGIC_CLASS_INSTANCE)を確保する。
 * @param args 評価済みの引数リスト(class クラスオブジェクト, slots-vector TAG_VECTOR)
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
 * 組み込み関数%%INSTANCE-SLOTS。ILOSクラスインスタンスのslots-vector(TAG_VECTOR)を返す。
 * @param args 評価済みの引数リスト(第一引数はクラスインスタンス)
 * @param env 呼び出し時の環境(未使用)
 * @return slots-vector(TAG_VECTOR)
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

