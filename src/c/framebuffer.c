#include "framebuffer.h"
#include "font8x16.h"


// 現在はframe_bufferはひとつ
// TODO: frame buffer を複数持ち、F1等で切り替えられるようにする
frame_buffer g_frame_buffer;



static UINT32 cursor_x(frame_buffer *self) {
    return self->cursor_position.x * 8;
}

static UINT32 cursor_y(frame_buffer *self) {
    return self->cursor_position.y * 16;
}

static void draw_char(volatile UINT32 *buffer, UINT32 pixels_per_scanline, UINT32 x, UINT32 y, char c, UINT32 color) {
    unsigned char code = (unsigned char)c;
    if (code > 127) {
        code = '?';
    }
    const unsigned char *glyph = font8x16[code];

    for (int row = 0; row < 16; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                buffer[(y + row) * pixels_per_scanline + (x + col)] = color;
            }
        }
    }
}

static void draw_cursor(frame_buffer *self) {
    for (UINT32 row = 0; row < 16; row++) {
        for (UINT32 col = 0; col < 8; col++) {
            self->buffer[(cursor_y(self) + row) * self->pixels_per_scanline + (cursor_x(self) + col)] = 0x00FFFFFF;
        }
    }
}

static void erase_cursor(frame_buffer *self) {
    for (UINT32 row = 0; row < 16; row++) {
        for (UINT32 col = 0; col < 8; col++) {
            self->buffer[(cursor_y(self) + row) * self->pixels_per_scanline + (cursor_x(self) + col)] = 0x00000000;
        }
    }
}

static void scroll_up(frame_buffer *self) {
    UINT32 y;
    for (y = 0; y + 16 < self->height; y++) {
        for (UINT32 x = 0; x < self->width; x++) {
            self->buffer[y * self->pixels_per_scanline + x] = self->buffer[(y + 16) * self->pixels_per_scanline + x];
        }
    }
    for (; y < self->height; y++) {
        for (UINT32 x = 0; x < self->width; x++) {
            self->buffer[y * self->pixels_per_scanline + x] = 0x00000000;
        }
    }
}

static void newline(frame_buffer *self) {
    self->cursor_position.x = 0;
    self->cursor_position.y += 1;
    if (cursor_y(self) >= self->height) {
        scroll_up(self);
        self->cursor_position.y -= 1;
    }
}

static void write_char(frame_buffer *self, UINT8 c) {
    erase_cursor(self);
    if (c == '\n') {
        newline(self);
        draw_cursor(self);
        return;
    }

    draw_char(self->buffer, self->pixels_per_scanline, cursor_x(self), cursor_y(self), (char)c, 0x00FFFFFF);
    self->cursor_position.x += 1;
    if (cursor_x(self) > self->width) {
        newline(self);
    }
    draw_cursor(self);
}

static void write_string(frame_buffer *self, const char *s) {
    while (*s) {
        write_char(self, (UINT8)*s);
        s++;
    }
}

static void clear_screen(frame_buffer *self) {
    for (UINT32 y = 0; y < self->height; y++) {
        for (UINT32 x = 0; x < self->width; x++) {
            self->buffer[y * self->pixels_per_scanline + x] = 0x00000000;
        }
    }
}


frame_buffer* initialize_frame_buffer(UINT64 base, UINT32 width, UINT32 height, UINT32 pixels_per_scanline) {

    g_frame_buffer.buffer = (volatile UINT32 *)base;
    g_frame_buffer.width = width;
    g_frame_buffer.height = height;
    g_frame_buffer.pixels_per_scanline = pixels_per_scanline;
    g_frame_buffer.cursor_position.x = 0;
    g_frame_buffer.cursor_position.y = 0;

    g_frame_buffer.clear_screen = clear_screen;
    g_frame_buffer.draw_cursor = draw_cursor;
    g_frame_buffer.erase_cursor = erase_cursor;
    g_frame_buffer.write_char = write_char;
    g_frame_buffer.write_string = write_string;
    g_frame_buffer.cursor_x = cursor_x;
    g_frame_buffer.cursor_y = cursor_y;

    return &g_frame_buffer;
}


