#include "types.h"
#include "version.h"
#include "interrupt.h"
#include "framebuffer.h"
#include "process.h"
#include "runtime.h"
#include "repl.h"
#include "subprimitive.h"
#include "ide_subprimitive.h"
#include "virtio9p.h"
#include "load.h"
#include "eval.h"
#include "stream_lisp.h"
#include "format.h"
#include "clock.h"
#include "stream.h"
#include "za.h"

// src/c/lisp_compiled.c(トランスパイラがsrc/lisp/init_aot.lispから生成する、
// gitignore対象のビルド成果物)で定義される。init.lispから移動したmember/assoc等を
// os_set_function経由でglobal_environmentへ登録する(M13)
extern void os_register_aot_init_functions(void);

/**
 * バージョン文字列・ビルド日時・ハッシュ・VirtIO-9pの検出結果をfbへ表示する
 * @param fb 表示先のframe buffer
 */
static void kernel_show_information(frame_buffer *fb) {
    fb->write_string(fb, "isikiOS version ");
    fb->write_string(fb, ISIKIOS_VERSION);
    fb->write_char(fb, '\n');

    fb->write_string(fb, "build: ");
    fb->write_string(fb, ISIKIOS_BUILD_DATE);
    fb->write_string(fb, " (");
    fb->write_string(fb, ISIKIOS_BUILD_HASH);
    fb->write_string(fb, ")\n");

    char err_msg[160];
    err_msg[0] = '\0';
    if (os_virtio9p_ensure_session(err_msg, sizeof(err_msg))) {
        fb->write_string(fb, "virtio9p: VirtIO-9p device detected\n");
    } else {
        fb->write_string(fb, "virtio9p: not detected: ");
        fb->write_string(fb, err_msg);
        fb->write_char(fb, '\n');
    }
}


/**
 * カーネルのエントリポイント。ヒープ・仮想バッファ・プロセス・GDT/PIC/PIT/IDTを
 * 初期化し、最後にプロセススケジューラを起動する(戻ってこない)
 * @param fb_base 物理frame bufferのアドレス
 * @param fb_width frame bufferの横幅(pixel)
 * @param fb_height frame bufferの高さ(pixel)
 * @param fb_pixels_per_scanline frame buffer1行あたりのピクセル数
 * @param heap_base Lispヒープの先頭アドレス
 * @param heap_size Lispヒープのサイズ(バイト)
 */
/** kernel_mainに渡されたboot_epoch_secondsを保持する(kernel_get_boot_epoch_secondsで読める) */
static UINT64 g_boot_epoch_seconds = 0;

UINT64 kernel_get_boot_epoch_seconds(void) {
    return g_boot_epoch_seconds;
}

/** os_set_qemu_test_mode経由でprocess 0の専用スタック上から呼ぶpower_off(kernel_mainの引数を保持) */
static void (*g_qemu_test_power_off)(void) = 0;

/** .qemu-test-triggerの内容(cc_loadするboot-entryスクリプトのパス)を保持する */
static char g_qemu_test_boot_script[256] = "test/lisp/qemu_boot_test.lisp";

/** g_qemu_test_boot_scriptが指すboot-entryスクリプトをcc_loadしてからpower_offする(process 0専用スタック上で実行される) */
static void run_qemu_boot_test(void) {
    cc_load(os_make_cons(os_make_string(g_qemu_test_boot_script), nil), global_environment);
    g_qemu_test_power_off();
}

void kernel_main(UINT64 fb_base, UINT32 fb_width, UINT32 fb_height, UINT32 fb_pixels_per_scanline, UINT64 heap_base, UINT64 heap_size, UINT64 boot_epoch_seconds, void (*power_off)(void)) {

    asm volatile ("cli");

    g_boot_epoch_seconds = boot_epoch_seconds;

    os_heap_init(heap_base, heap_size);
    os_bootstrap();
    os_register_subprimitives();
    os_register_ide_subprimitives();
    os_register_load();
    os_register_eval_primitives();
    os_register_streams();
    os_register_format();
    os_register_clock();
    os_register_za_primitives();
    os_register_aot_init_functions();

    frame_buffer *fb = initialize_virtual_buffers(fb_base, fb_width, fb_height, fb_pixels_per_scanline);
    initialize_processes(fb);

    volatile UINT32 *buf = (volatile UINT32 *)fb_base;

    fb->clear_screen(fb);
    fb->draw_cursor(fb);

    init_fpu();
    init_gdt();
    init_pic();
    init_pit();
    init_idt();

    // リポジトリルートに.qemu-test-triggerがあれば、その内容が指すLispファイルを
    // 自動loadしてから電源を切る(make test-qemu/test-qemu-milestoneからの全自動実行用)。
    // 内容が空(従来通りのtouchのみ)ならg_qemu_test_boot_scriptの初期値
    // (test/lisp/qemu_boot_test.lisp、全milestoneを通しで実行する)をそのまま使う。
    // kernel_mainのブート時スタック上で直接cc_loadを呼ぶとスタックサイズ不足で
    // クラッシュするため、process 0がタイマー割り込み経由で最初に起動される際に
    // (16KBの専用スタック上で)実行させる(process_trampoline_c参照)
    os_stream_t trigger_probe;
    char trigger_err[64];
    int qemu_test_mode = 0;
    if (os_stream_open_9p_file(&trigger_probe, ".qemu-test-trigger", trigger_err, sizeof(trigger_err))) {
        UINT64 len = 0;
        char ch;
        while (len + 1 < sizeof(g_qemu_test_boot_script) && os_stream_read_char(&trigger_probe, &ch)) {
            if (ch == '\n' || ch == '\r') {
                break;
            }
            g_qemu_test_boot_script[len++] = ch;
        }
        os_stream_close(&trigger_probe);
        if (len > 0) {
            g_qemu_test_boot_script[len] = '\0';
        }
        qemu_test_mode = 1;
        g_qemu_test_power_off = power_off;
        os_set_qemu_test_mode(run_qemu_boot_test);
    }

    if (!qemu_test_mode) {
        kernel_show_information(fb);
    }

    process_scheduler_start();
}


