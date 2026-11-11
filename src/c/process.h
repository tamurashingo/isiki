#ifndef _PROCESS_H_
#define _PROCESS_H_

#include "types.h"
#include "framebuffer.h"

/** プロセス数(F1〜F4に割り当てる。1プロセス=1仮想バッファ) */
#define PROCESS_COUNT VBUF_COUNT
/** 標準入力として蓄積できる最大バイト数 */
#define PROCESS_STDIN_BUF_SIZE 256

/** [#216] 素の入力で 1 キーずつ溜める受け皿の大きさ。
 *
 * **行の溜め場(stdin_buf)とは別に持つ。** 行の溜め場は「改行で確定する 1 行」
 * 専用で、確定の印(ready)を合図に読み手が動く作りになっており、
 * 1 キーずつ返す用途には使えない(調査 §12 の C 側 3 番目)。
 *
 * 16 で足りる: 素の入力を読むアプリは 1 打鍵ごとに描き直すので、
 * 溜まるのは「描いている間に打たれたぶん」だけである。**溢れたら捨てる**
 * (行の溜め場と同じ約束。ただしエコーしないので「画面には出たのに
 * 入力に入っていない」という食い違いは起きない)。 */
#define PROCESS_RAW_KEY_BUF_SIZE 16

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

    /** [#216] 素の入力(raw)かどうか。1 ならキーボード割り込みは
     * エコーせず、行の確定を待たず、読み切り待ちでキーを捨てず、
     * raw_keys へ 1 キーずつ積む。
     *
     * **GC が触る値を増やしていない**(ここも下の受け皿も素のスカラである)。
     * process_t は GC ヒープの外にある静的な配列なので、lisp_val_t を足すと
     * 根の登録が要るが、この 4 つはどれもそうではない。
     *
     * 割り込みハンドラから非同期に書き換えられるので volatile を付ける */
    volatile UINT32 raw_input;

    /** 素の入力のときの 1 キーずつの受け皿(環状に使う) */
    volatile UINT8 raw_keys[PROCESS_RAW_KEY_BUF_SIZE];
    /** 次に読む位置 */
    volatile UINT32 raw_head;
    /** 次に書く位置。raw_head == raw_tail が「空」である */
    volatile UINT32 raw_tail;

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

/**
 * @brief rspが属するプロセススタックの「実際に使える下端」を返す(issue #110)
 *
 * JIT のプロローグが置くスタック残量の検査は、**プロセスごとに違う下端**と
 * 比べなければならない(g_stack_areaは「64KBガード + 256KBスタック」の
 * スロットを並べた平坦な配列で、スロット長320KBは2の冪ではないのでrspから
 * 算術で求められない)。文脈切り替えのときにここで引いて、閾値を更新する。
 *
 * @param rsp 調べるスタックポインタ
 * @return 使えるスタックの下端アドレス。どのプロセススタックにも属さない場合は0
 *         (起動直後のkernelのidleループ上では0が返る)
 */
UINT64 os_process_stack_low_for(UINT64 rsp);

int os_process_stack_check(UINT64 rsp, UINT64 *out_low, UINT64 *out_used);

/** プロセス1つあたりのスタックサイズ。**ここが唯一の定義である。**
 *
 * **2026-11-08 までこの値は二重に書かれていた**: このヘッダの
 * STACK_SIZE_FOR_PANIC と process.c の STACK_SIZE に同じ `(256 * 1024)` があり、
 * **両者を縛るものが無かった**(片方だけ動かしてもビルドは通る)。
 * za.c の ZA_MAX_COMPILE_NEST の予算検査もこの値を使うので、1 か所にまとめた。
 * documents/stack-guard.md §8。 */
#define PROCESS_STACK_SIZE (256 * 1024)

/** 旧名。panic の診断表示で使っている(値は PROCESS_STACK_SIZE と同一) */
#define STACK_SIZE_FOR_PANIC PROCESS_STACK_SIZE

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
 * [#216] 素の入力の受け皿へ 1 キー積む。満杯なら捨てる。
 *
 * **積んだら ready を立てる。** 素の入力でも読み手を起こす合図は
 * 既存の os_wait_for_more_input(ready が立つまで hlt)を使い回す。
 * 新しい待ち関数を足さないのは、それを足すと reader.c を
 * リンクしている母艦の単体試験**全部**にダミー実装が要るからである
 * (os_wait_for_more_input が 8 本の試験でそうなっている)。
 *
 * @param proc 対象プロセス
 * @param code 積むキーの文字コード
 */
void process_raw_key_push(process_t *proc, UINT8 code);

/**
 * [#216] 素の入力の受け皿から 1 キー取り出す。
 * @param proc 対象プロセス
 * @param out_code 取り出したキーの格納先
 * @return 取り出せたら 1、空なら 0
 */
int process_raw_key_pop(process_t *proc, UINT8 *out_code);

/**
 * [#216] 素の入力の入切を切り替える。
 * @param proc 対象プロセス
 * @param on 非 0 で素の入力にする
 * @return 切り替える前の状態(1 なら素の入力だった)
 */
int process_set_raw_input(process_t *proc, int on);

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
 * 「起動時にやること」を登録する。登録後、process_scheduler_startにより
 * process 0が最初にタイマー割り込み経由で起動された際、通常のREPLループへ入る**前**に
 * boot_entryを呼ぶ(process 0専用のスタック(STACK_SIZE、process.c参照)上で
 * 実行することで、kernel_mainのブート時スタック上で直接cc_loadを実行した場合に
 * 発生するクラッシュを避ける)。
 *
 * [L4] **この間接化があるので、process.cは読み込みの下回り(load.c・reader.c・
 * 同梱したソースの配列)に依存しない。** process.cはTEST_SRC_*の16本に
 * リンクされているため、ここへ直接書くとその全部に依存を足すことになる。
 * 登録が無いユニット試験では何も起きない(0判定で素通りする)。
 *
 * 中身(kernel.cのrun_boot_entry)は「同梱したLispソースの読み込み →
 * (test-qemu時のみ)boot-entry台本」の順に実行する。**フックは1つである。**
 *
 * @param boot_entry process 0の初回起動時に呼ぶ関数(戻ってきた場合は通常の
 *   REPLループへ進む。台本を走らせた場合は電源断するので戻ってこない)
 */
void os_set_boot_entry(void (*boot_entry)(void));

#endif /* _PROCESS_H_ */
