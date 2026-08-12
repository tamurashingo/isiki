#include "process.h"
#include "runtime.h"
#include "lisp.h"
#include "interrupt.h"
#include "repl.h"

/** @brief PROCESS_COUNT個のprocess_t本体(F1〜F4)。indexで直接アクセスされる */
static process_t g_processes[PROCESS_COUNT];

/** @brief 現在アクティブ(表示フォーカスがある)プロセスのindex。switch_active_processが更新する */
static UINT32 g_current_process_index = 0;

/** @brief プロセスごとの専用実行スタックのサイズ(バイト) */
#define STACK_SIZE (16 * 1024)

/** @brief プロセスごとの専用実行スタック。GCが関知しないOS層の生メモリなので静的配列で確保する */
static UINT8 g_stacks[PROCESS_COUNT][STACK_SIZE] __attribute__((aligned(16)));

/**
 * @brief PCB(word2)に保存されているsaved_rspを読み出す
 * @param pcb os_make_instance(MAGIC_PROCESS, ...)で作られたPCB
 * @return タイマー割り込みで復元すべきスタックポインタ(生アドレス、タグなし)
 */
UINT64 os_process_get_saved_rsp(lisp_val_t pcb) {
    return ((UINT64 *)(pcb & ~TAG_MASK))[2];
}

/**
 * @brief PCB(word2)にsaved_rspを書き込む
 * @param pcb os_make_instance(MAGIC_PROCESS, ...)で作られたPCB
 * @param rsp 保存するスタックポインタ(生アドレス、タグなし)
 */
void os_process_set_saved_rsp(lisp_val_t pcb, UINT64 rsp) {
    ((UINT64 *)(pcb & ~TAG_MASK))[2] = rsp;
}

/**
 * @brief spawnが積んだ偽のIRETQフレームからタイマー割り込み経由で最初に着地する関数。
 * rdi(proc_index)で自分がどのprocess_tを動かすかを受け取り、そのままREPLループへ入る
 * (呼び出し元には戻らない)
 * @param proc_index 動かすprocess_tのindex(0〜PROCESS_COUNT-1)
 */
void __attribute__((sysv_abi)) process_trampoline_c(UINT64 proc_index) {
    process_t *proc = get_process((UINT32)proc_index);
    for (;;) {
        os_repl_step(proc);
    }
}

/**
 * @brief index番目のプロセス用スタックに、タイマー割り込みが積むであろう
 * 15レジスタ+IRETQフレームを模した偽のフレームを構築し、PCBを*RUN-QUEUE*へ登録する
 * @param proc_index 対象プロセスのindex(0〜PROCESS_COUNT-1)
 * @return 構築したPCB
 */
static lisp_val_t spawn(UINT32 proc_index) {
    UINT64 stack_top = (UINT64)(g_stacks[proc_index] + STACK_SIZE);
    // iretq後のRSPがmod 16 == 8となるよう調整(SysV ABIの関数入口の想定に揃える)
    if ((stack_top & 0xFULL) != 8) {
        stack_top -= 8;
    }

    // 15レジスタ(120B) + IRETQフレーム(40B)。frame_baseはasm_timer_handlerがGPR15個を
    // push/popし終えた時点のrsp(=current_rsp=saved_rsp)と同じ意味の位置
    UINT64 frame_base = stack_top - 160;

    UINT64 *regs = (UINT64 *)frame_base;
    for (UINT64 i = 0; i < 15; i++) {
        regs[i] = 0;
    }

    // regs[] は frame_base 起点の昇順アドレス配列だが、push は実行順に降順アドレスへ積まれる
    // (rax,rbx,rcx,rdx,rbp,rdi,rsi,r8,r9,r10,r11,r12,r13,r14,r15 の順でpushされるので、
    //  最後にpushされたr15が最も低位のアドレスに来る)
    regs[9] = proc_index; // rdi

    // asm_timer_handlerのFXSAVE/FXRSTORはframe_base(=current_rsp)から528byte下に
    // 確保した領域を(rsp+15)&-16でマスクして16byte境界のアドレスを求める。
    // 初回起動時にfxrstorが読む内容をここで同じ式で計算し、init_fpuのデフォルト状態で
    // 初期化する(MXCSR等を0埋めのままにするとSIMD例外が全解禁され、通常のfloat演算でトラップする)
    UINT8 *fxsave_area = (UINT8 *)(((frame_base - 528 + 15) & ~0xFULL));
    const UINT8 *fpu_default = (const UINT8 *)get_fpu_default_state();
    for (UINT64 i = 0; i < 512; i++) {
        fxsave_area[i] = fpu_default[i];
    }

    UINT64 *iretq_frame = (UINT64 *)(stack_top - 40);
    iretq_frame[0] = (UINT64)(void *)process_trampoline_c; // RIP
    iretq_frame[1] = 0x08;                                 // CS
    iretq_frame[2] = 0x202;                                // RFLAGS (IF=1)
    iretq_frame[3] = stack_top - 16;                       // RSP
    iretq_frame[4] = 0x10;                                 // SS

    lisp_val_t pcb = os_make_instance(MAGIC_PROCESS, os_make_fixnum(proc_index), frame_base, g_sym_process_ready);

    lisp_val_t anchor = os_get_variable(g_sym_run_queue, global_environment);
    if (anchor == nil) {
        // 初回のprocess登録: 自己参照させて循環リストの起点にする
        lisp_val_t new_cell = os_make_cons(pcb, nil);
        cc_set_cdr(new_cell, new_cell);
        os_set_variable(g_sym_run_queue, new_cell, global_environment);
    } else {
        lisp_val_t next_cell = cc_cdr(anchor);
        lisp_val_t new_cell = os_make_cons(pcb, next_cell);
        cc_set_cdr(anchor, new_cell);
    }

    return pcb;
}

/**
 * @brief PROCESS_COUNT個のPCBをspawnし、*RUN-QUEUE*を構築した上でタイマー割り込みを
 * 許可する。以後はタイマー割り込みだけがプロセスを切り替える(呼び出し元には戻らない)
 */
void process_scheduler_start(void) {
    for (UINT32 i = 0; i < PROCESS_COUNT; i++) {
        spawn(i);
    }

    enable_timer_irq();

#ifndef ISIKIOS_UNIT_TEST
    for (;;) {
        asm volatile ("hlt");
    }
#endif
}


/**
 * @brief PROCESS_COUNT個のプロセスを初期化し、それぞれに仮想バッファを1つずつ紐付ける
 * @param buffers PROCESS_COUNT個の frame_buffer が連続して並んだ配列の先頭
 */
void initialize_processes(frame_buffer *buffers) {
    for (UINT32 i = 0; i < PROCESS_COUNT; i++) {
        process_t *proc = &g_processes[i];
        proc->id = i;
        proc->name[0] = 'F';
        proc->name[1] = (char)('1' + i);
        proc->name[2] = '\0';
        proc->state = (i == 0) ? PROCESS_STATE_RUNNING : PROCESS_STATE_READY;
        proc->stdout_buffer = &buffers[i];
        proc->stdin_len = 0;
        proc->read_pos = 0;
        proc->ready = 0;
        proc->env = 0;
        os_gc_register_root(&proc->env);
        proc->gc_roots = 0;
    }

    g_current_process_index = 0;
}

/**
 * @brief 現在アクティブなプロセスを返す
 * @return 現在アクティブなプロセス
 */
process_t* get_current_process(void) {
    return &g_processes[g_current_process_index];
}

/**
 * @brief 表示フォーカスとは無関係に、固定indexでプロセスを返す(スケジューラが全プロセスを巡回するために使う)
 * @param index プロセス番号(0〜PROCESS_COUNT-1)
 * @return indexに対応するプロセス
 */
process_t* get_process(UINT32 index) {
    return &g_processes[index];
}

/**
 * @brief アクティブなプロセスを切り替える。表示中の仮想バッファも同時に切り替わる
 * @param index 切り替え先のプロセス番号(0〜PROCESS_COUNT-1)
 */
void switch_active_process(UINT32 index) {
    if (index >= PROCESS_COUNT || index == g_current_process_index) {
        return;
    }

    g_processes[g_current_process_index].state = PROCESS_STATE_READY;
    g_current_process_index = index;
    g_processes[g_current_process_index].state = PROCESS_STATE_RUNNING;

    switch_active_frame_buffer(index);
}

/**
 * @brief プロセスの標準入力に1文字積む。'\n'を積むとreadyが立つ
 * @param proc 対象プロセス
 * @param c 積む文字
 */
void process_stdin_push(process_t *proc, UINT8 c) {
    if (proc->stdin_len < PROCESS_STDIN_BUF_SIZE - 1) {
        proc->stdin_buf[proc->stdin_len++] = c;
    }
    if (c == '\n') {
        proc->ready = 1;
    }
}
