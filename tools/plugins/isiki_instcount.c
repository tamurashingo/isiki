/*
 * isiki_instcount.c
 *
 * QEMU TCGプラグイン: ゲストが実際に実行した命令数を、ホストの実行速度や
 * スケジューリングノイズと無関係にカウントする(documents/performance-
 * measurement.md参照)。TB(translation block)ごとの命令数(qemu_plugin_tb_
 * n_insns)を、そのTBが実行されるたびに加算する方式(TB粒度)を採る。
 * per-instructionコールバックは1命令ごとにコールバックが発生し極端に遅い
 * ため、実用上はこのTB粒度で十分な精度が得られる(ホストの実行速度に
 * 依存しない決定論的な指標である点が本質で、命令単位の厳密さより
 * ホストノイズの排除を優先する)。
 *
 * オプション引数(-plugin file=isiki_instcount.so,start=0xADDR,end=0xADDR):
 *   start/end を指定すると、その仮想アドレス範囲[start,end)に含まれる
 *   TBの命令数だけを別カウンタ(range_insns)に集計する。将来的に
 *   ide_wait_drq等の特定関数区間だけを計測したい場合に使う
 *   (JIT生成コードはヒープ上の動的アドレスのため、AOT/Cで静的アドレスが
 *   既知な関数区間の切り分けに主に有効)。
 *
 * 【重要】start/endに渡す実行時アドレスは、ビルド成果物(BOOTX64.EFI)の
 * シンボルテーブルが示すファイル上の相対仮想アドレス(RVA)そのままでは
 * 使えない。UEFIローダが実際にイメージをロードする実行時ベースアドレスは
 * 起動のたびに変わりうるだけでなく、**アタッチするディスクイメージの構成
 * (サイズ・パーティション形式の違い)によっても変わる**ことを実測で確認済み
 * (documents/performance-measurement.md「IDE読み込みの命令数内訳」節参照)。
 * 同一のディスク構成であれば起動ごとの再現性はある(2回検証済み)ため、
 * 計測対象と全く同じQEMUコマンドライン(同じディスクイメージ)で、まず
 * 実行時アドレスを取得してから(例: src/c/ide_subprimitive.cの
 * cc_diag_ide_read_sectors_addrのような診断用組み込み関数でos_ide_
 * read_sectors等の実行時ポインタ値を取得)、start/endを計算すること。
 *
 * オプション引数 profile=1: どのアドレスに命令数が集中しているかを
 * 事前に推測できない場合の探索用。TB単位で(開始アドレス, 命令数,
 * 実行回数)を記録し、終了時に「命令数×実行回数」降順でtop 40件を
 * stderrへ出力する(`[isiki_instcount] HOT vaddr=0x.. n_insns=.. exec=.. total=..`)。
 * これを`x86_64-w64-mingw32-objdump -t`のシンボルテーブルと突き合わせれば、
 * アドレス範囲を静的に予想せずに実際のホットスポットを特定できる
 * (ABI-M6/M8のAOT-to-AOT固定引数ABI化により、呼び出し元によって
 * __step(consリストABI)と__step_fixed(固定引数ABI)のどちらが実際に
 * 使われるかが変わるため、静的な予想だけでは外しうることを本調査で
 * 確認済み)。TB単位でGHashTableを使うため大量のTBがある場合はprofile
 * 無効時よりわずかにオーバーヘッドが増える。
 *
 * オプション引数 gate_start=0xADDR,gate_end=0xADDR: **測定区間を「時間で」
 * 区切る。** 指定すると、gate_start のTBが実行された時点からgate_endのTBが
 * 実行される時点までの間だけ、**実行している場所によらず全命令**をgate_insnsへ
 * 集計する。start/endによるアドレスのフィルタとは目的が違う:
 *
 *   start/end  ... その範囲に「居る」間だけ数える(アドレスの選別)
 *   gate_*     ... その区間の「間」はどこに居ても数える(時間の選別)
 *
 * インライン化の効果測定にはgate_*でなければならない。インラインで消えるのは
 * **呼び出し側のcallと引数の受け渡し**であって呼び先の中身ではないため、呼び先の
 * アドレス範囲だけを数えるとインライン化した瞬間にその範囲が0になり、
 * 「コストが呼び出し元へ移っただけ」なのに「全部消えた」と読めてしまう。
 * また呼び先(primitive_*)は.textの別の場所に、JIT生成コードはヒープ上の動的
 * アドレスに居るため、連続した1つのアドレス範囲では囲えない
 * (documents/performance-measurement.md「手法1の改良案」)。
 *
 * ゲートのマーカーにはos_bench_gate_open/os_bench_gate_close
 * (src/c/bench_subprimitive.c)を使い、実行時アドレスは%%GATE-PLUGIN-ARGSで
 * ゲスト側から取得する(ロード先はディスク構成でも変わるため、計測と同じ
 * QEMUコマンドラインで取ること)。マーカーのTB自身は数えない。
 *
 * 終了時(qemu_plugin_register_atexit_cb)にstderrへ
 *   [isiki_instcount] total_insns=<N> range_insns=<M>
 *   [isiki_instcount] gate_insns=<N> gate_opens=<K> gate_closes=<K>
 * の形式で出力する。**gate_opens が期待した回数でなければ、ゲートが開いて
 * いないか開きっぱなしなので、命令数は信用してはならない。**
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t total_insns = 0;
static uint64_t range_insns = 0;
static uint64_t gate_insns = 0;
/* 区間ごとの内訳。1ブートで複数の区間(床・対照・本命)を測れるようにする。
   1つの数字しか出さないと、区間ごとにブートし直すことになり、
   そのたびにブート間のぶれが乗る */
#define GATE_MAX_SEGMENTS 64
static uint64_t seg_insns[GATE_MAX_SEGMENTS];
static uint64_t seg_count = 0;
static uint64_t gate_cur = 0;
static uint64_t gate_opens = 0;
static uint64_t gate_closes = 0;
static uint64_t gate_start_addr = 0;
static uint64_t gate_end_addr = 0;
static int have_gate = 0;
/* ゲートが開いているか。TB実行コールバックからのみ触る */
static int gate_open = 0;
static uint64_t range_start = 0;
static uint64_t range_end = 0;
static int have_range = 0;
static int have_profile = 0;

typedef struct {
    uint64_t vaddr;
    uint64_t n_insns;
    uint64_t exec_count;
} hot_entry_t;

/* vaddr -> hot_entry_t*。tb_transはTB再翻訳時に複数回呼ばれうるため、
 * 同じvaddrへは既存エントリを使い回す(重複エントリでの二重カウントを防ぐ) */
static GHashTable *hot_table = NULL;
static GMutex hot_table_lock;

static void vcpu_tb_exec_total(unsigned int cpu_index, void *udata) {
    (void)cpu_index;
    uint64_t n = (uint64_t)(uintptr_t)udata;
    __atomic_add_fetch(&total_insns, n, __ATOMIC_RELAXED);
}

static void vcpu_tb_exec_range(unsigned int cpu_index, void *udata) {
    (void)cpu_index;
    uint64_t n = (uint64_t)(uintptr_t)udata;
    __atomic_add_fetch(&range_insns, n, __ATOMIC_RELAXED);
}

/* ゲートを開く。マーカーのTB自身は数えない(開くのは実行後の扱いでよい) */
static void vcpu_tb_exec_gate_open(unsigned int cpu_index, void *udata) {
    (void)cpu_index;
    (void)udata;
    __atomic_store_n(&gate_cur, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&gate_open, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&gate_opens, 1, __ATOMIC_RELAXED);
}

static void vcpu_tb_exec_gate_close(unsigned int cpu_index, void *udata) {
    (void)cpu_index;
    (void)udata;
    __atomic_store_n(&gate_open, 0, __ATOMIC_RELAXED);
    uint64_t cur = __atomic_load_n(&gate_cur, __ATOMIC_RELAXED);
    if (seg_count < GATE_MAX_SEGMENTS) {
        seg_insns[seg_count] = cur;
    }
    seg_count++;
    __atomic_add_fetch(&gate_insns, cur, __ATOMIC_RELAXED);
    __atomic_add_fetch(&gate_closes, 1, __ATOMIC_RELAXED);
}

/* ゲートが開いている間だけ数える。**アドレスは見ない**(どこに居ても数える) */
static void vcpu_tb_exec_gated(unsigned int cpu_index, void *udata) {
    (void)cpu_index;
    if (__atomic_load_n(&gate_open, __ATOMIC_RELAXED)) {
        uint64_t n = (uint64_t)(uintptr_t)udata;
        __atomic_add_fetch(&gate_cur, n, __ATOMIC_RELAXED);
    }
}

static void vcpu_tb_exec_hot(unsigned int cpu_index, void *udata) {
    (void)cpu_index;
    hot_entry_t *e = (hot_entry_t *)udata;
    __atomic_add_fetch(&e->exec_count, 1, __ATOMIC_RELAXED);
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    (void)id;
    size_t n = qemu_plugin_tb_n_insns(tb);
    uint64_t vaddr = qemu_plugin_tb_vaddr(tb);
    qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_tb_exec_total,
                                          QEMU_PLUGIN_CB_NO_REGS,
                                          (void *)(uintptr_t)n);
    if (have_range && vaddr >= range_start && vaddr < range_end) {
        qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_tb_exec_range,
                                              QEMU_PLUGIN_CB_NO_REGS,
                                              (void *)(uintptr_t)n);
    }
    if (have_gate) {
        if (vaddr == gate_start_addr) {
            qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_tb_exec_gate_open,
                                                  QEMU_PLUGIN_CB_NO_REGS, NULL);
        } else if (vaddr == gate_end_addr) {
            qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_tb_exec_gate_close,
                                                  QEMU_PLUGIN_CB_NO_REGS, NULL);
        } else {
            qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_tb_exec_gated,
                                                  QEMU_PLUGIN_CB_NO_REGS,
                                                  (void *)(uintptr_t)n);
        }
    }
    if (have_profile) {
        g_mutex_lock(&hot_table_lock);
        hot_entry_t *e = g_hash_table_lookup(hot_table, (gpointer)(uintptr_t)vaddr);
        if (e == NULL) {
            e = g_new0(hot_entry_t, 1);
            e->vaddr = vaddr;
            e->n_insns = n;
            g_hash_table_insert(hot_table, (gpointer)(uintptr_t)vaddr, e);
        }
        g_mutex_unlock(&hot_table_lock);
        qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_tb_exec_hot,
                                              QEMU_PLUGIN_CB_NO_REGS, e);
    }
}

static gint hot_cmp(gconstpointer a, gconstpointer b) {
    const hot_entry_t *ea = *(const hot_entry_t **)a;
    const hot_entry_t *eb = *(const hot_entry_t **)b;
    uint64_t ta = ea->n_insns * ea->exec_count;
    uint64_t tb_ = eb->n_insns * eb->exec_count;
    if (ta < tb_) return 1;
    if (ta > tb_) return -1;
    return 0;
}

static void plugin_exit(qemu_plugin_id_t id, void *p) {
    (void)id;
    (void)p;
    if (have_range) {
        fprintf(stderr, "[isiki_instcount] total_insns=%" PRIu64
                         " range_insns=%" PRIu64
                         " range=[0x%" PRIx64 ",0x%" PRIx64 ")\n",
                total_insns, range_insns, range_start, range_end);
    } else {
        fprintf(stderr, "[isiki_instcount] total_insns=%" PRIu64 "\n",
                total_insns);
    }
    if (have_gate) {
        fprintf(stderr, "[isiki_instcount] gate_insns=%" PRIu64
                         " gate_opens=%" PRIu64 " gate_closes=%" PRIu64
                         " gate=[0x%" PRIx64 ",0x%" PRIx64 "]\n",
                gate_insns, gate_opens, gate_closes,
                gate_start_addr, gate_end_addr);
        if (gate_opens == 0) {
            fprintf(stderr, "[isiki_instcount] WARNING: ゲートが一度も開いていない。"
                             "アドレスが違うか、マーカーがインライン化されている\n");
        }
        uint64_t shown = seg_count < GATE_MAX_SEGMENTS ? seg_count : GATE_MAX_SEGMENTS;
        for (uint64_t i = 0; i < shown; i++) {
            fprintf(stderr, "[isiki_instcount] SEG %" PRIu64 " insns=%" PRIu64 "\n",
                    i, seg_insns[i]);
        }
        if (seg_count > GATE_MAX_SEGMENTS) {
            fprintf(stderr, "[isiki_instcount] WARNING: 区間が %" PRIu64
                             " 個あり、先頭 %d 個しか記録していない\n",
                    seg_count, GATE_MAX_SEGMENTS);
        }
        if (gate_opens != gate_closes) {
            fprintf(stderr, "[isiki_instcount] WARNING: open(%" PRIu64
                             ") と close(%" PRIu64 ") の回数が合わない\n",
                    gate_opens, gate_closes);
        }
    }
    if (have_profile) {
        GPtrArray *arr = g_ptr_array_new();
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, hot_table);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            g_ptr_array_add(arr, value);
        }
        g_ptr_array_sort(arr, hot_cmp);
        guint limit = arr->len < 40 ? arr->len : 40;
        for (guint i = 0; i < limit; i++) {
            hot_entry_t *e = (hot_entry_t *)g_ptr_array_index(arr, i);
            fprintf(stderr, "[isiki_instcount] HOT vaddr=0x%" PRIx64
                             " n_insns=%" PRIu64 " exec=%" PRIu64
                             " total=%" PRIu64 "\n",
                    e->vaddr, e->n_insns, e->exec_count,
                    e->n_insns * e->exec_count);
        }
        g_ptr_array_free(arr, TRUE);
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                            const qemu_info_t *info,
                                            int argc, char **argv) {
    (void)info;
    for (int i = 0; i < argc; i++) {
        if (g_str_has_prefix(argv[i], "start=")) {
            range_start = g_ascii_strtoull(argv[i] + 6, NULL, 0);
            have_range = 1;
        } else if (g_str_has_prefix(argv[i], "end=")) {
            range_end = g_ascii_strtoull(argv[i] + 4, NULL, 0);
            have_range = 1;
        } else if (g_str_has_prefix(argv[i], "gate_start=")) {
            gate_start_addr = g_ascii_strtoull(argv[i] + 11, NULL, 0);
            have_gate = 1;
        } else if (g_str_has_prefix(argv[i], "gate_end=")) {
            gate_end_addr = g_ascii_strtoull(argv[i] + 9, NULL, 0);
            have_gate = 1;
        } else if (g_str_has_prefix(argv[i], "profile=")) {
            have_profile = atoi(argv[i] + 8) != 0;
        }
    }
    if (have_profile) {
        g_mutex_init(&hot_table_lock);
        hot_table = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
