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
process_t* get_current_process(void);

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
void __attribute__((sysv_abi)) process_trampoline_c(UINT64 proc_index);

/**
 * PROCESS_COUNT個のPCBをspawnし、*RUN-QUEUE*を構築した上でタイマー割り込みを
 * 許可し、以後はタイマー割り込みだけがプロセスを切り替える。呼び出し元(kernel_main)
 * には戻らない。
 */
void process_scheduler_start(void);

/**
 * make test-qemu用の自動テストモードを有効化する。有効化後、process_scheduler_start
 * によりprocess 0が最初にタイマー割り込み経由で起動された際、通常のREPLループの代わりに
 * test_entryを呼ぶ(process 0専用の16KBスタック上で実行することで、kernel_mainの
 * ブート時スタック上で直接cc_loadを実行した場合に発生するクラッシュを避ける)。
 * @param test_entry process 0の初回起動時に呼ぶ関数(戻ってきた場合は通常のREPLループへ
 *   フォールバックする)
 */
void os_set_qemu_test_mode(void (*test_entry)(void));

#endif /* _PROCESS_H_ */
