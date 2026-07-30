#include "types.h"
#include "version.h"
#include "interrupt.h"
#include "framebuffer.h"


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


void kernel_main(UINT64 fb_base, UINT32 fb_width, UINT32 fb_height, UINT32 fb_pixels_per_scanline, UINT64 heap_base, UINT64 heap_size) {

    asm volatile ("cli");

    frame_buffer *fb = initialize_frame_buffer(fb_base, fb_width, fb_height, fb_pixels_per_scanline);

    volatile UINT32 *buf = (volatile UINT32 *)fb_base;

    fb->clear_screen(fb);
    fb->draw_cursor(fb);

    init_gdt();
    init_pic();
    init_idt();

    kernel_show_information(fb);

    for (;;) {
    }

}


