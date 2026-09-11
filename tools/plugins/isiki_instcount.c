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

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb) {
    (void)id;
    size_t n = qemu_plugin_tb_n_insns(tb);
    qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_tb_exec_total,
                                          QEMU_PLUGIN_CB_NO_REGS,
                                          (void *)(uintptr_t)n);
    if (have_range) {
        uint64_t vaddr = qemu_plugin_tb_vaddr(tb);
        if (vaddr >= range_start && vaddr < range_end) {
            qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_tb_exec_range,
                                                  QEMU_PLUGIN_CB_NO_REGS,
                                                  (void *)(uintptr_t)n);
        }
    }
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
        }
    }
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
