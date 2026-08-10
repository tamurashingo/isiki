#include <stdint.h>

#include "types.h"
#include "framebuffer.h"
#include "process.h"
#include "reader.h"
#include "runtime.h"
#include "lisp.h"

#include "interrupt.h"



/** GDT本体(null/コード/データの3エントリ) */
static struct gdt_entry g_gdt[3];
/** lgdt命令に渡すGDTポインタ */
static struct gdt_ptr g_gdt_ptr;

/** IDT本体(256エントリ) */
static struct idt_entry g_idt[256];
/** lidt命令に渡すIDTポインタ */
static struct idt_ptr g_idt_ptr;

/** Shiftキーが押されている間は1になる */
static volatile uint8_t key_shift_pressed = 0;

/** スキャンコード→ASCII文字の変換テーブル(Shift未押下時) */
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

/** スキャンコード→ASCII文字の変換テーブル(Shift押下時) */
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


/**
 * I/Oポートへ1バイト出力する
 * @param port 出力先のポート番号
 * @param val 出力する値
 */
void outb(uint16_t port, uint8_t val) {
    asm volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * I/Oポートから1バイト読み込む
 * @param port 読み込み元のポート番号
 * @return 読み込んだ値
 */
uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/**
 * I/Oポートへ2バイト出力する
 * @param port 出力先のポート番号
 * @param val 出力する値
 */
void outw(uint16_t port, uint16_t val) {
    asm volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * I/Oポートから2バイト読み込む
 * @param port 読み込み元のポート番号
 * @return 読み込んだ値
 */
uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/**
 * I/Oポートへ4バイト出力する
 * @param port 出力先のポート番号
 * @param val 出力する値
 */
void outl(uint16_t port, uint32_t val) {
    asm volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

/**
 * I/Oポートから4バイト読み込む
 * @param port 読み込み元のポート番号
 * @return 読み込んだ値
 */
uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}


/** GP例外(vector 13)のエントリポイント。エラーコードとvectorを積んでcpu_exception_commonへ入る */
void asm_gpf_handler(void);
/** ページフォルト(vector 14)のエントリポイント。エラーコードとvectorを積んでcpu_exception_commonへ入る */
void asm_pf_handler(void);
/**
 * asm_gpf_handler/asm_pf_handlerが積んだレジスタ・エラーコード・vectorをExceptionContextとして受け取り、
 * cpu_exception_common(手書きasm)から呼ばれる
 * @param ctx 割り込み発生時のレジスタ・実行コンテキスト一式
 * @param fault_addr GPFでは未使用の引数(esiに載る値、現状は捨てている)
 */
void __attribute__((sysv_abi)) c_cpu_exception_handler(ExceptionContext *ctx, uint64_t fault_addr);
// asm_gpf_handler/asm_pf_handlerの共通後続処理。15汎用レジスタとexceptionのコンテキストを
// ExceptionContextとしてスタックに積み、c_cpu_exception_handlerを呼ぶ(戻ってこない)
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


/** アクティブなプロセスの画面上でカーソルを1文字分戻し、その位置を消す */
static void backspace(void) {
    frame_buffer *fb = get_active_frame_buffer();
    if (fb->cursor_position.x <= 0) {
        return;
    }
    fb->erase_cursor(fb);

    // clear prev char
    fb->cursor_position.x -= 1;
    fb->erase_cursor(fb);

    fb->draw_cursor(fb);
}

/**
 * projectのreadyフラグが立つ(1行分の入力が確定する)まで待つ。
 * タイマー割り込みが自動的に他プロセスへ切り替えてくれるので、明示的なyieldは不要。
 * hltで次の割り込みまでCPUを休ませておく
 * @param proc 入力待ちするプロセス
 */
void os_wait_for_more_input(process_t *proc) {
    while (!proc->ready) {
        asm volatile ("hlt");
    }
}

/**
 * キーボード割り込みの本体処理。スキャンコードをASCII文字へ変換し、
 * カレントプロセスの標準入力へ積む(F1〜F4はプロセス切替えとして処理する)
 * @param scancode PS/2キーボードから受け取ったスキャンコード
 */
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

    if (scancode >= 0x3B && scancode <= 0x3E) {
        // F1〜F4: アクティブなプロセスを切り替える(表示中の仮想バッファも同時に切り替わる)
        switch_active_process(scancode - 0x3B);
        return;
    }

    char c = key_shift_pressed ? SCANCODE_SHIFT[scancode] : SCANCODE_NORMAL[scancode];
    if (c == 0) {
        return;
    }

    process_t *current = get_current_process();

    if (current->ready) {
        // 直前のEnterで確定した行をreaderが消費し切るまでは入力を無視する
        return;
    }

    if (c == '\b') {
        backspace();
        if (current->stdin_len > 0) {
            current->stdin_len -= 1;
        }
        return;
    }

    frame_buffer *fb = current->stdout_buffer;
    fb->write_char(fb, (UINT8)c);
    process_stdin_push(current, (UINT8)c);
}

/**
 * キーボード割り込み(IRQ1)のエントリポイント。scancodeを読んでc_keyboard_handlerへ渡し、
 * 最後にEOIを送る。GCC自動生成のprologue/epilogueで十分なので__attribute__((interrupt))を使う
 * @param frame 未使用(GCCが要求する引数)
 */
__attribute__((interrupt))
void asm_keyboard_handler(struct interrupt_frame *frame) {
    (void)frame; // unused
    uint8_t scancode = inb(0x60);
    c_keyboard_handler(scancode);
    outb(0x20, 0x20); // EOI
}

/**
 * タイマー割り込み(IRQ0)のエントリポイント。__attribute__((interrupt))は使わない:
 * GCCの自動生成するprologue/epilogueはiretq前に任意のrspへ入れ替える
 * (次に実行するプロセスのコンテキストへ切り替える)ことができないため、
 * asm_gpf_handler/asm_pf_handlerと同じ手書きasmで実装する。
 * IRQ0はCPUがエラーコードを積まない外部割り込みなので、push $vectorは不要で、
 * 15汎用レジスタのみをpush/popする
 */
void asm_timer_handler(void);

/**
 * current_rspを現在のプロセスのPCBへ保存し、*RUN-QUEUE*上の次のプロセスの
 * saved_rspを返す(asm_timer_handlerがそれをrspへ入れ替えてiretqする)
 * @param current_rsp 割り込み発生時にasm_timer_handlerが積んだ15レジスタの先頭アドレス
 * @return 次に実行するプロセスのsaved_rsp
 */
UINT64 __attribute__((sysv_abi)) c_timer_switch(UINT64 current_rsp);
asm(
    ".global asm_timer_handler\n"
    "asm_timer_handler:\n"
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
    "    call c_timer_switch\n"
    "    mov %rax, %rsp\n"
    "    pop %r15\n"
    "    pop %r14\n"
    "    pop %r13\n"
    "    pop %r12\n"
    "    pop %r11\n"
    "    pop %r10\n"
    "    pop %r9\n"
    "    pop %r8\n"
    "    pop %rsi\n"
    "    pop %rdi\n"
    "    pop %rbp\n"
    "    pop %rdx\n"
    "    pop %rcx\n"
    "    pop %rbx\n"
    "    pop %rax\n"
    "    iretq\n"
);

/** PIT tick数のカウンタ。get-internal-real-time/get-universal-time等の基礎になる */
static UINT64 g_tick_counter = 0;

UINT64 get_tick_counter(void) {
    return g_tick_counter;
}

UINT64 __attribute__((sysv_abi)) c_timer_switch(UINT64 current_rsp) {
    outb(0x20, 0x20); // EOI を先に返す
    g_tick_counter++;

    lisp_val_t current_cell = os_get_variable(g_sym_current_process, global_environment);
    lisp_val_t next_cell;
    if (current_cell == nil) {
        // 起動直後の初回tick: kernelのidleループのrspはどのPCBにも属さないため捨てる
        next_cell = os_get_variable(g_sym_run_queue, global_environment);
    } else {
        os_process_set_saved_rsp(cc_car(current_cell), current_rsp);
        next_cell = cc_cdr(current_cell); // 循環しているので巻き戻り不要
    }
    os_set_variable(g_sym_current_process, next_cell, global_environment);

    return os_process_get_saved_rsp(cc_car(next_cell));
}

/**
 * IRQ0(PIT)のマスクだけを解除する。process_scheduler_startが全PCB/*RUN-QUEUE*を
 * 構築した後に呼ぶことで、初期化中にタイマーが暴発するのを防ぐ
 */
void enable_timer_irq(void) {
    outb(0x21, inb(0x21) & ~0x01);
}

/**
 * PIT(Programmable Interval Timer)をチャンネル0・lobyte/hibyte・モード3(矩形波)で
 * 約100Hz(1193182Hz / 11932 ≒ 100Hz)に設定する
 */
void init_pit(void) {
    outb(0x43, 0x36);
    uint16_t divisor = 11932;
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

void __attribute__((sysv_abi)) c_cpu_exception_handler(ExceptionContext *ctx, uint64_t fault_addr) {
    frame_buffer *fb = get_active_frame_buffer();
    fb->write_char(fb, 'c');
}

/**
 * GDTの1エントリを設定する(ベース/リミットは0固定、フラットメモリモデルのため)
 * @param idx 設定先のインデックス
 * @param access アクセス権(タイプ, DPL, present等)
 * @param granularity リミット上位4bit + フラグ(粒度, サイズ等)
 */
static void set_gdt_entry(int idx, uint8_t access, uint8_t granularity) {
    g_gdt[idx].limit_low = 0;
    g_gdt[idx].base_low = 0;
    g_gdt[idx].base_mid = 0;
    g_gdt[idx].access = access;
    g_gdt[idx].granularity = granularity;
    g_gdt[idx].base_high = 0;
}


/**
 * IDTの1エントリを設定し、vec番の割り込みが発生した際にhandlerへ飛ぶようにする
 * @param vec 設定先の割り込み番号
 * @param handler ハンドラのエントリポイント
 */
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

/** GDTを構築し、lgdt/lretqでコード・データセグメントを切り替える */
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

/** PICを初期化し、IRQ1(キーボード)のみを許可した状態にする(IRQ0は未許可のまま) */
void init_pic(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11); // ICW1
    outb(0x21, 0x20); outb(0xA1, 0x28); // ICW2: マスタ -> 32番台, スレーブ -> 40番台
    outb(0x21, 0x04); outb(0xA1, 0x21); // ICW3: カスケード設定
    outb(0x21, 0x01); outb(0xA1, 0x01); // ICW4: 8086モード
    outb(0x21, 0xFD); // マスク: IRQ1(キーボード)のみ許可
    outb(0xA1, 0xFF); // スレーブは全マスク
}

/** IDTを構築し、GPF/PF/タイマー/キーボードの各ハンドラを登録してlidt/stiする */
void init_idt(void) {
    set_idt_entry(13, (void *)asm_gpf_handler);
    set_idt_entry(14, (void *)asm_pf_handler);
    set_idt_entry(32, (void *)asm_timer_handler);
    set_idt_entry(33, (void *)asm_keyboard_handler);

    g_idt_ptr.limit = sizeof(g_idt) - 1;
    g_idt_ptr.base = (uint64_t)&g_idt;

    asm volatile ("lidt %0" : : "m"(g_idt_ptr));
    asm volatile ("sti");
}



