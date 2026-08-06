#include "types.h"
#include "version.h"
#include "interrupt.h"
#include "framebuffer.h"
#include "process.h"
#include "runtime.h"
#include "repl.h"


/**
 * バージョン文字列とビルド日時・ハッシュをfbへ表示する
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
void kernel_main(UINT64 fb_base, UINT32 fb_width, UINT32 fb_height, UINT32 fb_pixels_per_scanline, UINT64 heap_base, UINT64 heap_size) {

    asm volatile ("cli");

    os_heap_init(heap_base, heap_size);
    os_bootstrap();

    frame_buffer *fb = initialize_virtual_buffers(fb_base, fb_width, fb_height, fb_pixels_per_scanline);
    initialize_processes(fb);

    volatile UINT32 *buf = (volatile UINT32 *)fb_base;

    fb->clear_screen(fb);
    fb->draw_cursor(fb);

    init_gdt();
    init_pic();
    init_pit();
    init_idt();

    kernel_show_information(fb);

    process_scheduler_start();
}


