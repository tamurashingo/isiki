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
 * 終了時(qemu_plugin_register_atexit_cb)にstderrへ
 *   [isiki_instcount] total_insns=<N> range_insns=<M>
 * の形式で出力する。
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t total_insns = 0;
static uint64_t range_insns = 0;
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
