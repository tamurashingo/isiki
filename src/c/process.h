#ifndef _PROCESS_H_
#define _PROCESS_H_

#include "types.h"
#include "framebuffer.h"

/** プロセス数(F1〜F4に割り当てる。1プロセス=1仮想バッファ) */
#define PROCESS_COUNT VBUF_COUNT
/** 標準入力として蓄積できる最大バイト数 */
#define PROCESS_STDIN_BUF_SIZE 256

typedef enum {
    PROCESS_STATE_READY,
    PROCESS_STATE_RUNNING,
} process_state_t;

typedef struct _process {
    UINT32 id;
    char name[8];

    process_state_t state;

    /** 標準出力: このプロセスに紐付いた仮想バッファ */
    frame_buffer *stdout_buffer;

    /** 標準入力: キー入力の蓄積先 */
    UINT8 stdin_buf[PROCESS_STDIN_BUF_SIZE];
    UINT32 stdin_len;

    /** os_read の読取カーソル(stdin_buf内の未読み取り位置) */
    UINT32 read_pos;

    /** 直前のEnterで1行が確定し、readerの消費を待っている状態かどうか。
     * キーボード割り込みハンドラから非同期に書き換えられ、os_wait_for_more_input の
     * busy-waitループから読まれるため volatile が必要(無いと-O1でループが最適化で消える) */
    volatile UINT32 ready;

    /** このプロセスのLisp環境(global_environmentの子環境)。0は未初期化を表す */
    lisp_val_t env;

    /** インタプリタが評価中の(動的extentにある)blockの名前のリスト(内側が先頭)。
     * eval_block/eval_return_from(eval.c)が既に抜けたblockへのreturn-fromを
     * <control-error>にするために使う。0は未初期化(空)を表す */
    lisp_val_t live_blocks;

    /** このプロセスのshadow stackの先頭(GC_PROTECTで保護中のCローカル変数のリスト) */
    gc_rootnode *gc_roots;
} process_t;

/**
 * PROCESS_COUNT個のプロセスを初期化し、それぞれに仮想バッファを1つずつ紐付ける
 * @param buffers PROCESS_COUNT個の frame_buffer が連続して並んだ配列の先頭
 */
void initialize_processes(frame_buffer *buffers);

/**
 * 現在アクティブなプロセスを返す
 */
/* [性能測定] Phase4 第1部: GC_PROTECTはこの関数を1箇所につき3回呼ぶ
   (マクロ本体で2回、スコープ脱出時のgc_unprotect_nodeで1回)。process.cに
   実体を置いたままだと-O1・LTO無しでは3回とも本物のクロスTU呼び出しになり、
   実測でGC_PROTECT 1箇所あたり34.9命令のうち大半を占めていた。中身は配列参照
   だけなのでヘッダのstatic inlineへ移し、呼び出しを消す。za.cのJITランタイム
   ヘルパー(za_gc_*)も同じ関数を呼ぶため、そちらにも効く。
   アドレスを取っている箇所は無いことを確認済み */
/**
 * 現在のプロセスのスタックが溢れていないかを検査する([性能測定] Phase5 第0部)。
 * カナリアの破壊と、rspがスタック範囲(下端からSTACK_GUARD_MARGINの余裕を含む)を
 * 外れていないかの両方を見る。タイマ割り込みから呼ぶ想定で、通常パスには乗らない。
 * @param rsp 検査する(割り込み時の)スタックポインタ
 * @param out_low スタック下端アドレスの格納先
 * @param out_used 消費バイト数の格納先
 * @return 正常なら1、溢れていれば0
 */
/** rspがいずれかのプロセススタックの範囲内かどうか。例外ハンドラがスタックを
    dumpしてよいかの判定に使う(スタック溢れではrspが範囲外を指しており、
    そのまま読むとハンドラ自身がフォルトしてダブルフォルトになる) */
/** 全プロセスのスタック直下のガード領域を未マップにする(ブート時に1回) */
void os_process_install_stack_guards(void);
/** i番目のガード領域の先頭とサイズ、およびvaがガード内かの判定 */
UINT64 os_process_guard_base(UINT32 i);
UINT64 os_process_guard_size(void);
int os_process_in_stack_guard(UINT64 va);
/** ガードのどちら側かを区別する。上端側は「溢れ」ではなく基底/上限の破壊である */
int os_process_guard_is_upper(UINT64 va, UINT64 rsp);
UINT64 os_process_stack_base(UINT32 i);
int os_process_stack_contains(UINT64 rsp);

int os_process_stack_check(UINT64 rsp, UINT64 *out_low, UINT64 *out_used);

/** プロセス1つあたりのスタックサイズ(panicの診断表示用) */
#define STACK_SIZE_FOR_PANIC (256 * 1024)

extern process_t g_processes[PROCESS_COUNT];
extern UINT32 g_current_process_index;

static inline process_t* get_current_process(void) {
    return &g_processes[g_current_process_index];
}

/**
 * 表示フォーカスとは無関係に、固定indexでプロセスを返す(スケジューラが全プロセスを巡回するために使う)
 * @param index プロセス番号(0〜PROCESS_COUNT-1)
 */
process_t* get_process(UINT32 index);

/**
 * アクティブなプロセスを切り替える。表示中の仮想バッファも同時に切り替わる
 * @param index 切り替え先のプロセス番号(0〜PROCESS_COUNT-1)
 */
void switch_active_process(UINT32 index);

/**
 * プロセスの標準入力に1文字積む
 * @param proc 対象プロセス
 * @param c 積む文字
 */
void process_stdin_push(process_t *proc, UINT8 c);

/**
 * PCB(TAG_INSTANCE, MAGIC_PROCESS)に保存されているsaved_rspを読み書きする。
 * タイマー割り込みハンドラ(c_timer_switch)がプロセス切替え時に使う。
 */
UINT64 os_process_get_saved_rsp(lisp_val_t pcb);
void os_process_set_saved_rsp(lisp_val_t pcb, UINT64 rsp);

/**
 * spawnが積んだ偽のIRETQフレームからタイマー割り込み経由で最初に着地する関数。
 * rdi(proc_index)で自分がどのprocess_tを動かすかを受け取り、REPLループへ入る。
 * SysV ABI(rdi渡し)を前提に組んだ偽フレームから直接RIPとして使われるため、
 * MS ABI(mingw)側とズレないようsysv_abiを強制する
 */
void SYSV_ABI process_trampoline_c(UINT64 proc_index);

/**
 * PROCESS_COUNT個のPCBをspawnし、*RUN-QUEUE*を構築した上でタイマー割り込みを
 * 許可し、以後はタイマー割り込みだけがプロセスを切り替える。呼び出し元(kernel_main)
 * には戻らない。
 */
void process_scheduler_start(void);

/**
 * make test-qemu用の自動テストモードを有効化する。有効化後、process_scheduler_start
 * によりprocess 0が最初にタイマー割り込み経由で起動された際、通常のREPLループの代わりに
 * test_entryを呼ぶ(process 0専用のスタック(STACK_SIZE、process.c参照)上で実行することで、kernel_mainの
 * ブート時スタック上で直接cc_loadを実行した場合に発生するクラッシュを避ける)。
 * @param test_entry process 0の初回起動時に呼ぶ関数(戻ってきた場合は通常のREPLループへ
 *   フォールバックする)
 */
void os_set_qemu_test_mode(void (*test_entry)(void));

#endif /* _PROCESS_H_ */
