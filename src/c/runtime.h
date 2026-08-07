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


/** TAG_INSTANCEのword0に入る、ネイティブ(C)関数であることを示すMAGIC NUMBER */
#define MAGIC_FUNCTION_NATIVE      0x1ULL
/** TAG_INSTANCEのword0に入る、Lisp(defun)で定義された関数であることを示すMAGIC NUMBER */
#define MAGIC_FUNCTION_INTERPRETED 0x2ULL
/** TAG_INSTANCEのword0に入る、プロセスのPCBであることを示すMAGIC NUMBER */
#define MAGIC_PROCESS              0x3ULL
/** TAG_INSTANCEのword0に入る、Lisp(defmacro)で定義されたマクロであることを示すMAGIC NUMBER */
#define MAGIC_MACRO                0x4ULL

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

/** ルートの環境(全プロセスの環境が最終的にこれを親として辿る) */
extern lisp_val_t global_environment;

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
 * parent_envを親とする新しい環境(name/variables/functions/parentの4slotを持つリスト)を作る。
 * @param env_symbol 環境の名前を表すsymbol
 * @param parent_env 親環境。ルート環境の場合はnil
 * @return 作成した環境
 */
lisp_val_t os_make_environment(lisp_val_t env_symbol, lisp_val_t parent_env);

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

#endif /* _RUNTIME_H_ */

