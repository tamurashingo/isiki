#include <stdint.h>

#include "types.h"
#include "framebuffer.h"

#include "interrupt.h"

extern frame_buffer g_frame_buffer;



/* ----------------------------------------
 * 変数定義
 * ---------------------------------------- */
static struct gdt_entry g_gdt[3];
static struct gdt_ptr g_gdt_ptr;

static struct idt_entry g_idt[256];
static struct idt_ptr g_idt_ptr;

static volatile uint8_t key_shift_pressed = 0;

static const char SCANCODE_NORMAL[128] = {
    [0x02]='1', [0x03]='2', [0x04]='3', [0x05]='4', [0x06]='5',
    [0x07]='6', [0x08]='7', [0x09]='8', [0x0A]='9', [0x0B]='0',
    [0x0C]='-', [0x0D]='=', [0x0E]='\b',
    [0x10]='q', [0x11]='w', [0x12]='e', [0x13]='r', [0x14]='t',
    [0x15]='y', [0x16]='u', [0x17]='i', [0x18]='o', [0x19]='p',
    [0x1A]='[', [0x1B]=']', [0x1C]='\n',
    [0x1E]='a', [0x1F]='s', [0x20]='d', [0x21]='f', [0x22]='g',
    [0x23]='h', [0x24]='j', [0x25]='k', [0x26]='l',
    [0x27]=';', [0x28]='\'', [0x29]='`',
    [0x2B]='\\', [0x2C]='z', [0x2D]='x', [0x2E]='c', [0x2F]='v',
    [0x30]='b', [0x31]='n', [0x32]='m', [0x33]=',', [0x34]='.', [0x35]='/',
    [0x39]=' ',
};

static const char SCANCODE_SHIFT[128] = {
    [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$', [0x06]='%',
    [0x07]='^', [0x08]='&', [0x09]='*', [0x0A]='(', [0x0B]=')',
    [0x0C]='_', [0x0D]='+', [0x0E]='\b',
    [0x10]='Q', [0x11]='W', [0x12]='E', [0x13]='R', [0x14]='T',
    [0x15]='Y', [0x16]='U', [0x17]='I', [0x18]='O', [0x19]='P',
    [0x1A]='{', [0x1B]='}', [0x1C]='\n',
    [0x1E]='A', [0x1F]='S', [0x20]='D', [0x21]='F', [0x22]='G',
    [0x23]='H', [0x24]='J', [0x25]='K', [0x26]='L',
    [0x27]=':', [0x28]='"', [0x29]='~',
    [0x2B]='|', [0x2C]='Z', [0x2D]='X', [0x2E]='C', [0x2F]='V',
    [0x30]='B', [0x31]='N', [0x32]='M', [0x33]='<', [0x34]='>', [0x35]='?',
    [0x39]=' ',
};



/* ---------------------------------------- */
static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}


void asm_gpf_handler(void);
void asm_pf_handler(void);
void __attribute__((sysv_abi)) c_cpu_exception_handler(ExceptionContext *ctx, uint64_t fault_addr);
asm(
    ".global asm_gpf_handler\n"
    "asm_gpf_handler:\n"
    "    push $13\n"
    "    jmp cpu_exception_common\n"
    ".global asm_pf_handler\n"
    "asm_pf_handler:\n"
    "    push $14\n"
    "cpu_exception_common:\n"
    "    push %rax\n"
    "    push %rbx\n"
    "    push %rcx\n"
    "    push %rdx\n"
    "    push %rbp\n"
    "    push %rdi\n"
    "    push %rsi\n"
    "    push %r8\n"
    "    push %r9\n"
    "    push %r10\n"
    "    push %r11\n"
    "    push %r12\n"
    "    push %r13\n"
    "    push %r14\n"
    "    push %r15\n"
    "    mov %rsp, %rdi\n"
    "    and $-16, %rsp\n"
    "    call c_cpu_exception_handler\n"
    "    hlt\n"
    "    jmp .-1\n"
);


static void backspace(void) {
    if (g_frame_buffer.cursor_position.x <= 0) {
        return;
    }
    g_frame_buffer.erase_cursor(&g_frame_buffer);

    // clear prev char
    g_frame_buffer.cursor_position.x -= 1;
    g_frame_buffer.erase_cursor(&g_frame_buffer);

    g_frame_buffer.draw_cursor(&g_frame_buffer);
}

void c_keyboard_handler(uint8_t scancode) {
    if (scancode == 0x2A || scancode == 0x36) {
        key_shift_pressed = 1;
        return;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        key_shift_pressed = 0;
        return;
    }
    if (scancode & 0x80) {
        return;  // その他のBreakコードは無視
    }

    char c = key_shift_pressed ? SCANCODE_SHIFT[scancode] : SCANCODE_NORMAL[scancode];
    if (c == 0) {
        return;
    }
    if (c == '\b') {
        backspace();
        return;
    }
    if (c == '\n') {
        g_frame_buffer.write_char(&g_frame_buffer, (UINT8)c);
        return;
    }
    g_frame_buffer.write_char(&g_frame_buffer, (UINT8)c);
}

__attribute__((interrupt))
void asm_keyboard_handler(struct interrupt_frame *frame) {
    (void)frame; // unused
    uint8_t scancode = inb(0x60);
    c_keyboard_handler(scancode);
    outb(0x20, 0x20); // EOI
}

void __attribute__((sysv_abi)) c_cpu_exception_handler(ExceptionContext *ctx, uint64_t fault_addr) {
    g_frame_buffer.write_char(&g_frame_buffer, 'c');
}

static void set_gdt_entry(int idx, uint8_t access, uint8_t granularity) {
    g_gdt[idx].limit_low = 0;
    g_gdt[idx].base_low = 0;
    g_gdt[idx].base_mid = 0;
    g_gdt[idx].access = access;
    g_gdt[idx].granularity = granularity;
    g_gdt[idx].base_high = 0;
}


static void set_idt_entry(int vec, void *handler) {
    uint64_t addr = (uint64_t)handler;
    g_idt[vec].offset_low = addr & 0xFFFF;
    g_idt[vec].selector = 0x08;
    g_idt[vec].ist = 0;
    g_idt[vec].type_attr = 0x8E;
    g_idt[vec].offset_mid = (addr >> 16) & 0xFFFF;
    g_idt[vec].offset_high = (addr >> 32) & 0xFFFFFFFF;
    g_idt[vec].zero = 0;
}

void init_gdt(void) {
    set_gdt_entry(0, 0x00, 0x00);
    set_gdt_entry(1, 0x9A, 0x20); // コード: access 0x9A, granularity 0x20(64bitフラグ)
    set_gdt_entry(2, 0x92, 0x00); // データ: access 0x92

    g_gdt_ptr.limit = sizeof(g_gdt) - 1;
    g_gdt_ptr.base = (uint64_t)&g_gdt;

    asm volatile(
        "lgdt %0\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "push $0x08\n"
        "lea 1f(%%rip), %%rax\n"
        "push %%rax\n"
        "lretq\n"
        "1:\n"
        : : "m"(g_gdt_ptr) : "rax"
    );
}

void init_pic(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11); // ICW1
    outb(0x21, 0x20); outb(0xA1, 0x28); // ICW2: マスタ -> 32番台, スレーブ -> 40番台
    outb(0x21, 0x04); outb(0xA1, 0x21); // ICW3: カスケード設定
    outb(0x21, 0x01); outb(0xA1, 0x01); // ICW4: 8086モード
    outb(0x21, 0xFD); // マスク: IRQ1(キーボード)のみ許可
    outb(0xA1, 0xFF); // スレーブは全マスク
}

void init_idt(void) {
    set_idt_entry(13, (void *)asm_gpf_handler);
    set_idt_entry(14, (void *)asm_pf_handler);
    set_idt_entry(33, (void *)asm_keyboard_handler);

    g_idt_ptr.limit = sizeof(g_idt) - 1;
    g_idt_ptr.base = (uint64_t)&g_idt;

    asm volatile ("lidt %0" : : "m"(g_idt_ptr));
    asm volatile ("sti");
}



