#include "process.h"
#include "runtime.h"
#include "lisp.h"
#include "interrupt.h"
#include "repl.h"

/** @brief PROCESS_COUNT個のprocess_t本体(F1〜F4)。indexで直接アクセスされる */
process_t g_processes[PROCESS_COUNT];

/** @brief 現在アクティブ(表示フォーカスがある)プロセスのindex。switch_active_processが更新する */
UINT32 g_current_process_index = 0;

/**
 * @brief プロセスごとの専用実行スタックのサイズ(バイト)
 * 16KBでは(load "src/lisp/init.lisp")のような初回フルロードでC側の再帰(reader等)が
 * 実測約35〜36KBまで達し、スタックを踏み越えてGPFになることを確認済み(ガードページが
 * 無いため隣接プロセスのスタック領域へ静かに侵入する)ため、一度64KBへ増やした。
 * その後FAT16-M7c(fat16-create-directory)で64KBでも再発することを確認した:
 * eval.cのフック(os_eval呼び出しごとにRSPをサンプリング)で計測できる範囲だけで
 * 既に約48KB(0xC008)消費しており、その先で呼ぶread-sector等のネイティブC呼び出し
 * (IDEドライバのPIO処理)の分はこの粒度では計測できないため実際のピークはさらに深い。
 * 64KBではこの経路で本当にスタックを踏み越え、ガードページが無いために破壊が
 * 例外配送機構自体(IDT/GDTやRSP自体)まで及び、CPU例外が一度も配送されずtriple fault
 * 相当でQEMUが無応答終了することを実機(QEMU)上で確認した。256KBに増やした状態では
 * 複数回の再現実験で例外なく完走することを確認している
 */
#define STACK_SIZE (256 * 1024)

/** @brief プロセスごとの専用実行スタック。GCが関知しないOS層の生メモリなので静的配列で確保する */
/* [原則6] 各スタックの**下**にガード領域を置く。ここを未マップにすることで、
   溢れた瞬間に#PFが出てIDT/GDTへ到達する前に止まる。

   ガードを複数ページ(64KB)取るのは、1ページだと大きなローカル配列を持つ関数が
   SPを一気に下げてガードを**跨いで着地**しうるためである。踏まれなければ
   フォルトは出ず、「IDTを壊してから死ぬ」に戻る。

   レイアウト: slot[i] = [ガード64KB][実スタック256KB] を連続で並べ、**末尾にもガードを1つ足す**。
   こうするとプロセスiの上端は必ずプロセスi+1のガード(未マップ)になり、
   最後のプロセスの上端も末尾ガードで守られる。上下両側が塞がる。

     [G0][stack0][G1][stack1][G2][stack2][G3][stack3][Gtop]

   末尾ガードが要るのは、以前プロセス0の**上端**を越える読み出し(usagesの
   ループ上限が壊れた件)を実際に踏んでいるためである。あのときは隣が
   プロセス1のガードだったので捕まったが、最後のプロセスには隣が無かった。

   配列を2次元ではなく平坦にするのは、末尾ガードとの連続性を宣言順に依存せず
   保証するため(BSSの配置順は保証されない)。
   4KB境界に揃えるのは、ページテーブルの粒度で未マップにするため。 */
#define STACK_GUARD_SIZE (64 * 1024)
#define STACK_SLOT_SIZE  (STACK_GUARD_SIZE + STACK_SIZE)
#define STACK_AREA_SIZE  (PROCESS_COUNT * STACK_SLOT_SIZE + STACK_GUARD_SIZE)
static UINT8 g_stack_area[STACK_AREA_SIZE] __attribute__((aligned(4096)));

/** i番目のスロット先頭(=そのプロセスの下側ガードの先頭) */
static UINT8 *stack_slot(UINT32 i) {
    return g_stack_area + (UINT64)i * STACK_SLOT_SIZE;
}

/** i番目のプロセスが実際に使うスタックの下端(下側ガードの直上) */
static UINT8 *stack_usable_base(UINT32 i) {
    return stack_slot(i) + STACK_GUARD_SIZE;
}

/* ---- ガードページの設営 ----------------------------------------------------
   UEFIが張った恒等マップは2MBページ(実測: %%DIAG-PAGE-LEVELが2を返す)なので、
   ガードにしたい範囲を含む2MBページを4KBページへ**分割**してから、該当ページの
   Presentビットを落とす。分割用のページテーブルは静的に持つ(ヒープはまだ
   使えない段階で呼ぶため)。 */
#define PT_POOL_COUNT 8
static UINT64 g_pt_pool[PT_POOL_COUNT][512] __attribute__((aligned(4096)));
static UINT32 g_pt_pool_used = 0;
/** 分割済みの2MBページの先頭VA(同じ2MBページを二度分割しないための記録) */
static UINT64 g_split_va[PT_POOL_COUNT];

#define PTE_PRESENT 0x1ULL
#define PTE_PS      0x80ULL
#define PTE_ADDR    0x000FFFFFFFFFF000ULL

/** vaに対応するPDエントリへのポインタを返す(辿れなければ0) */
static UINT64 *pd_entry_for(UINT64 va) {
    UINT64 cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    UINT64 *pml4 = (UINT64 *)(cr3 & ~0xFFFULL);
    UINT64 e4 = pml4[(va >> 39) & 0x1FF];
    if (!(e4 & PTE_PRESENT)) { return 0; }
    UINT64 *pdpt = (UINT64 *)(e4 & PTE_ADDR);
    UINT64 e3 = pdpt[(va >> 30) & 0x1FF];
    if (!(e3 & PTE_PRESENT) || (e3 & PTE_PS)) { return 0; }
    UINT64 *pd = (UINT64 *)(e3 & PTE_ADDR);
    return &pd[(va >> 21) & 0x1FF];
}

/** vaを含む2MBページを4KBページ512個へ分割する(既に4KBならそのまま) */
static UINT64 *ensure_4k_table(UINT64 va) {
    UINT64 *pde = pd_entry_for(va);
    if (pde == 0) { return 0; }
    if (!(*pde & PTE_PS)) {
        return (UINT64 *)(*pde & PTE_ADDR); /* 既に4KB粒度 */
    }
    UINT64 base2m = va & ~0x1FFFFFULL;
    for (UINT32 k = 0; k < g_pt_pool_used; k++) {
        if (g_split_va[k] == base2m) {
            return g_pt_pool[k];
        }
    }
    if (g_pt_pool_used >= PT_POOL_COUNT) { return 0; }
    UINT64 flags = *pde & ~(PTE_ADDR | PTE_PS);
    UINT64 *pt = g_pt_pool[g_pt_pool_used];
    for (UINT32 j = 0; j < 512; j++) {
        pt[j] = (base2m + (UINT64)j * 4096ULL) | flags | PTE_PRESENT;
    }
    g_split_va[g_pt_pool_used] = base2m;
    g_pt_pool_used++;
    *pde = ((UINT64)(lisp_addr_t)pt & PTE_ADDR) | flags | PTE_PRESENT;
    return pt;
}

/** [va, va+len) を未マップにする(4KB単位) */
static void unmap_range(UINT64 va, UINT64 len) {
    for (UINT64 off = 0; off < len; off += 4096) {
        UINT64 a = va + off;
        UINT64 *pt = ensure_4k_table(a);
        if (pt == 0) { continue; }
        pt[(a >> 12) & 0x1FF] &= ~PTE_PRESENT;
        __asm__ __volatile__("invlpg (%0)" : : "r"(a) : "memory");
    }
}

/** 全プロセスのスタック直下のガード領域を未マップにする。
    kernel_mainがプロセスを起動する前に1回だけ呼ぶ */
void os_process_install_stack_guards(void) {
    /* UEFIが張ったページテーブルは書き込み保護されている(実測: PDエントリへの
       書き込みが error_code=0x3 の#PFになった)。CPL0でもCR0.WP=1だと
       read-onlyページへの書き込みはフォルトするため、この間だけWPを落とす。 */
    UINT64 cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0 & ~(1ULL << 16)));

    /* 各スロット先頭のガード + 末尾ガード(最後のプロセスの上端側) */
    for (UINT32 i = 0; i <= PROCESS_COUNT; i++) {
        unmap_range((UINT64)stack_slot(i), STACK_GUARD_SIZE);
    }

    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));
}

UINT64 os_process_guard_base(UINT32 i) {
    if (i > PROCESS_COUNT) { return 0; }
    return (UINT64)stack_slot(i);
}

UINT64 os_process_guard_size(void) { return STACK_GUARD_SIZE; }

/** vaがいずれかのガード領域の中か。例外ハンドラが「ガードページを踏んだ」と
    明示するために使う */
/* [原則6] ガードのどちら側かで**原因が違う**ので区別する。
   下端側 = スタック溢れ(再帰が深すぎる)。
   上端側 = 溢れではなく、基底や上限の破壊・バッファオーバーラン。
   実際に`usages`のループ上限が壊れて上端を越えた事例がある。
   0=ガードでない 1=どれかのスタックの下端側 2=どれかのスタックの上端側 */
int os_process_guard_side(UINT64 va) {
    for (UINT32 i = 0; i <= PROCESS_COUNT; i++) {
        UINT64 lo = (UINT64)stack_slot(i);
        if (va < lo || va >= lo + STACK_GUARD_SIZE) { continue; }
        /* スロットiのガードは「プロセスiの下端側」であり、同時に
           「プロセスi-1の上端側」でもある。rspがどちらのスタックに居るかで決める */
        return 1;
    }
    return 0;
}

/** vaがガードのとき、rspから見て上端側の踏み越しかどうか。
    rspがスロットi-1のスタック内にあり、vaがスロットiのガードなら上端側 */
int os_process_guard_is_upper(UINT64 va, UINT64 rsp) {
    for (UINT32 i = 0; i <= PROCESS_COUNT; i++) {
        UINT64 lo = (UINT64)stack_slot(i);
        if (va < lo || va >= lo + STACK_GUARD_SIZE) { continue; }
        if (i == 0) { return 0; }  /* 先頭スロットの下は誰の上端でもない */
        UINT64 prev_lo = (UINT64)stack_usable_base(i - 1);
        if (rsp >= prev_lo && rsp <= prev_lo + STACK_SIZE) { return 1; }
        return 0;
    }
    return 0;
}

int os_process_in_stack_guard(UINT64 va) {
    for (UINT32 i = 0; i <= PROCESS_COUNT; i++) {
        UINT64 lo = (UINT64)stack_slot(i);
        if (va >= lo && va < lo + STACK_GUARD_SIZE) { return 1; }
    }
    return 0;
}

/* [性能測定] Phase5 第0部: スタックガード。
   スタックにはガードページが無く、溢れてもページフォルトにならない。検出が無いと
   ゲストは無反応のまま停止し、外からは「極端に遅い処理」と区別できない
   (documents/pitfalls.md 原則5)。ページング機構をこの段階で触るのは影響範囲が
   大きいため、(1)各スタックの最下端へカナリアを置き (2)タイマ割り込みで
   rspの範囲とカナリアの両方を検査する、という方式にする。通常パスのコストは
   ゼロで、検査はスケジューラの切り替え時にのみ走る。
   ただし検出粒度はタイマ周期(約10ms)であり、深い再帰は10ms未満で256KBを
   消費しうる。その場合でも「溢れた後の次のtickで診断付きpanicに到達する」ため、
   無言の停止よりは大幅に切り分けやすくなる(完全な即時検出にはガードページか
   関数プロローグでの検査が要るが、後者はPhase4で削った呼び出しコストを
   再び載せることになる) */
#define STACK_CANARY 0x5441434B47554152ULL /* "STACKGUAR" 相当のマジック */
/** カナリアの直上に置く安全マージン。rspがここより下に来た時点で溢れたと判定する */
#define STACK_GUARD_MARGIN 4096

static void stack_canary_init(UINT32 proc_index) {
    *(UINT64 *)stack_usable_base(proc_index) = STACK_CANARY;
}

/** [第0部] 診断用: i番目のプロセススタックの下端アドレス。フォルト時のrspが
    どのスタックのどこにあったかを、外から突き合わせるために公開する */
UINT64 os_process_stack_base(UINT32 i) {
    if (i >= PROCESS_COUNT) { return 0; }
    return (UINT64)stack_usable_base(i);
}

int os_process_stack_contains(UINT64 rsp) {
    for (UINT32 i = 0; i < PROCESS_COUNT; i++) {
        UINT64 low = (UINT64)stack_usable_base(i);
        if (rsp >= low && rsp <= low + STACK_SIZE) {
            return 1;
        }
    }
    return 0;
}

int os_process_stack_check(UINT64 rsp, UINT64 *out_low, UINT64 *out_used) {
    // まず全プロセスのカナリアを見る。溢れたスタックのrspはg_stacksの範囲外へ
    // 出てしまい下のループでは捕まらないため、破壊の痕跡はこちらで検出する
    // (PROCESS_COUNTは数個なのでtickごとに全部見ても無視できるコスト)
    for (UINT32 i = 0; i < PROCESS_COUNT; i++) {
        if (*(UINT64 *)stack_usable_base(i) != STACK_CANARY) {
            *out_low = (UINT64)stack_usable_base(i);
            *out_used = STACK_SIZE;
            return 0;
        }
    }
    // rspが属するスタックを特定し、下端のマージンに食い込んでいないかを見る。
    // 実行中のプロセスはスケジューラが*CURRENT-PROCESS*で管理しており
    // g_current_process_index(表示フォーカス用)とは別物なので、indexではなく
    // rspがどの範囲に入るかで判定する
    for (UINT32 i = 0; i < PROCESS_COUNT; i++) {
        UINT64 low = (UINT64)stack_usable_base(i);
        UINT64 high = low + STACK_SIZE;
        if (rsp >= low && rsp <= high) {
            *out_low = low;
            *out_used = high - rsp;
            return rsp >= low + STACK_GUARD_MARGIN;
        }
    }
    // どのプロセススタックにも属さない。起動直後の初回tickはkernelのidleループの
    // スタック上で走るため、ここへ来るのは正常
    *out_low = 0;
    *out_used = 0;
    return 1;
}

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
/** os_set_qemu_test_modeで登録されたテスト実行関数(未登録時は0=通常のREPLモード) */
static void (*g_qemu_test_entry)(void) = 0;

void os_set_qemu_test_mode(void (*test_entry)(void)) {
    g_qemu_test_entry = test_entry;
}

void SYSV_ABI process_trampoline_c(UINT64 proc_index) {
    process_t *proc = get_process((UINT32)proc_index);
    if (proc_index == 0 && g_qemu_test_entry != 0) {
        g_qemu_test_entry();
    }
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
    stack_canary_init(proc_index);
    UINT64 stack_top = (UINT64)(stack_usable_base(proc_index) + STACK_SIZE);
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
/* [性能測定] Phase4 第1部: process.hのstatic inlineへ移した
   (GC_PROTECT 1箇所につきこの関数が3回呼ばれており、クロスTU呼び出しのままだと
   その3回分がGC_PROTECTのコストの大半を占めていた) */

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
