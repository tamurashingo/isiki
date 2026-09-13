#include <stdint.h>

#include "types.h"
#include "framebuffer.h"
#include "process.h"
#include "reader.h"
#include "runtime.h"
#include "lisp.h"

#include "interrupt.h"



/** GDT本体(null/コード/データの3エントリ) */
/* [原則6] スタック溢れを**報告可能にする**ためのTSS + IST。
   従来はタイマー割り込みでのカナリア/rsp検査だけだったが、100Hzのサンプリングでは
   1フレーム160byteで降りていくCスタックに追いつけない。実測では深度4000で
   GP例外 → (ハンドラ自身が枯渇スタックへpushできず)ダブルフォルト → トリプル
   フォルトとなり、QEMUがリセットして終わっていた(`-d int`で確認)。
   外からは「電源が落ちた」としか見えず、原則6が指す無言の失敗そのものだった。

   IST1に専用スタックを割り当て、GP(13)・PF(14)・ダブルフォルト(8)をそこで
   受けるようにする。枯渇したスタックの上で動かないので、診断を出し切れる。 */
#define IST_STACK_SIZE 16384
static UINT8 g_ist_stack[IST_STACK_SIZE] __attribute__((aligned(16)));

/** x86-64のTSS(長モードではタスク切り替えには使わず、RSP0とISTの置き場として使う) */
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

static struct tss64 g_tss;

/* GDTはTSSディスクリプタ(長モードでは16byte = エントリ2つ分)のぶん広げる。
   0:null 1:code 2:data 3-4:TSS */
static struct gdt_entry g_gdt[5];
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


/** COM1(0x3F8)のI/Oポート群。診断出力をQEMUの-serialへ流すためだけに使う */
#define SERIAL_COM1_PORT 0x3F8

/**
 * COM1を38400bps/8N1/FIFO有効で初期化する。QEMUは-serialオプション無しでも
 * デバイス自体はデフォルトで存在するため、outbが失敗することはない
 * (実機の欠品ハードウェアは対象外)
 */
static void serial_init(void) {
    outb(SERIAL_COM1_PORT + 1, 0x00); // 割り込み無効
    outb(SERIAL_COM1_PORT + 3, 0x80); // DLAB=1
    outb(SERIAL_COM1_PORT + 0, 0x03); // divisor下位(115200/3=38400bps)
    outb(SERIAL_COM1_PORT + 1, 0x00); // divisor上位
    outb(SERIAL_COM1_PORT + 3, 0x03); // 8bit, no parity, 1 stop bit(DLAB=0に戻る)
    outb(SERIAL_COM1_PORT + 2, 0xC7); // FIFO有効化・クリア・14byte閾値
    outb(SERIAL_COM1_PORT + 4, 0x0B); // RTS/DSRセット
}

/** THR(送信バッファ)が空くまで待ってから1バイト送出する */
static void serial_write_char(char c) {
    while ((inb(SERIAL_COM1_PORT + 5) & 0x20) == 0) {
    }
    outb(SERIAL_COM1_PORT, (uint8_t)c);
}

/** 文字列を先頭からserial_write_charで送出する */
static void serial_write_string(const char *s) {
    while (*s) {
        serial_write_char(*s);
        s++;
    }
}

/* [原則6] panicの診断をフレームバッファだけに出すと、-display noneでは誰も読めない。
   スタックガードは正しく発動していたのに、外からは「電源が落ちた」としか見えず、
   深度4000で溢れていることに気づけなかった。runtime.cのpanic経路から呼べるよう
   serialへの出力を公開する(診断専用。通常の実行経路では使わない) */
/** [第0部] 診断: IDT/GDT/TSSの置き場。暴走したスタックがこれらを踏んでいないかを
    外から突き合わせるために公開する(スタックの下に何があるかはBSSの配置次第) */
UINT64 os_diag_idt_addr(void) { return (UINT64)(void *)&g_idt[0]; }
UINT64 os_diag_gdt_addr(void) { return (UINT64)(void *)&g_gdt[0]; }

void os_diag_serial_write(const char *s) {
    serial_write_string(s);
}

/** 64bit値を"0x"付き16桁でserialへ出す(フレームバッファを経由しない) */
static void serial_write_hex64(uint64_t v) {
    static const char hex_digits[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = hex_digits[(v >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\0';
    serial_write_string(buf);
}

/** GP例外(vector 13)のエントリポイント。エラーコードとvectorを積んでcpu_exception_commonへ入る */
/** ダブルフォルト(vector 8)のエントリポイント。IST1の専用スタックで動く */
void asm_df_handler(void);
void asm_gpf_handler(void);
/** ページフォルト(vector 14)のエントリポイント。エラーコードとvectorを積んでcpu_exception_commonへ入る */
void asm_pf_handler(void);
/**
 * asm_gpf_handler/asm_pf_handlerが積んだレジスタ・エラーコード・vectorをExceptionContextとして受け取り、
 * cpu_exception_common(手書きasm)から呼ばれる
 * @param ctx 割り込み発生時のレジスタ・実行コンテキスト一式
 * @param fault_addr GPFでは未使用の引数(esiに載る値、現状は捨てている)
 */
void SYSV_ABI c_cpu_exception_handler(ExceptionContext *ctx, uint64_t fault_addr);

// asm_gpf_handler/asm_pf_handlerの共通後続処理。15汎用レジスタとexceptionのコンテキストを
// ExceptionContextとしてスタックに積み、c_cpu_exception_handlerを呼ぶ(戻ってこない)
asm(
    ".global asm_df_handler\n"
    "asm_df_handler:\n"
    /* ダブルフォルトはCPUがエラーコード(常に0)を積むので、vectorだけ足せば
       GP/PFと同じExceptionContextの形になる */
    "    push $8\n"
    "    jmp cpu_exception_common\n"
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
        // stdin_lenが0(バッファに何も入力していない)ならプロンプト自体を消してしまう
        // ため、その場合は何もしない
        if (current->stdin_len > 0) {
            backspace();
            current->stdin_len -= 1;
        }
        return;
    }

    frame_buffer *fb = current->stdout_buffer;
    fb->write_char(fb, (UINT8)c);
    process_stdin_push(current, (UINT8)c);
}

/** asm_keyboard_handlerから呼ばれるC本体。scancodeを読んでc_keyboard_handlerへ渡し、最後にEOIを送る */
void SYSV_ABI c_keyboard_isr(void);

/**
 * キーボード割り込み(IRQ1)のエントリポイント。__attribute__((interrupt))は使わない:
 * SSE/FPUを有効化した状態では、GCCが__attribute__((interrupt))関数向けにFXSAVE/FXRSTOR相当の
 * コードを生成できず("sorry, unimplemented: SSE instructions aren't allowed in an interrupt
 * service routine")、-mgeneral-regs-only無しではビルドできないため、asm_gpf_handler/
 * asm_timer_handlerと同じ手書きasmで実装する。IRQ1はCPUがエラーコードを積まない外部割り込み
 * なので、push $vectorは不要で、15汎用レジスタのみをpush/popする。プロセス切り替えを
 * 行わない(呼び出し元のコンテキストへそのままiretqで戻る)点がasm_timer_handlerと異なるため、
 * c呼び出し前後でのrsp退避にはrbxをスクラッチに使う(asm_timer_handlerと同じ理由で、
 * fxsave/fxrstorは16byte境界を要求するがGPR push前のrspの境界は保証されないため、
 * push後のrsp(常にrbxへ保存)を基準に(rsp-528+15)&-16でマスクして動的に求める)
 */
void asm_keyboard_handler(void);
asm(
    ".global asm_keyboard_handler\n"
    "asm_keyboard_handler:\n"
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
    "    mov %rsp, %rbx\n"
    "    sub $528, %rsp\n"
    "    mov %rsp, %rax\n"
    "    add $15, %rax\n"
    "    and $-16, %rax\n"
    "    fxsave (%rax)\n"
    "    and $-16, %rsp\n"
    "    call c_keyboard_isr\n"
    "    mov %rbx, %rsp\n"
    "    lea -528(%rbx), %rax\n"
    "    add $15, %rax\n"
    "    and $-16, %rax\n"
    "    fxrstor (%rax)\n"
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

void SYSV_ABI c_keyboard_isr(void) {
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
UINT64 SYSV_ABI c_timer_switch(UINT64 current_rsp);
// GPR15個のpush/popの外側でFXSAVE/FXRSTORによりFPU/SSEレジスタ(x87/xmm0-15/MXCSR)も
// 保存・復元する。fxsave/fxrstorは対象アドレスが16byte境界であることを要求するが、
// 割り込み発生時のrsp(=IRETQフレームのすぐ上)は「実行中のプロセスがどの命令の直後で
// 中断されたか」に依存し、16byte境界とは限らない(callの直後はmod16==8になる等)。
// そのため「GPR15個をpushし終えたrsp(=current_rsp、PCBのsaved_rspと同じ意味)」を
// 基準に、そこから528byte(512byte本体+16byteの余裕)下を確保し、実行時に
// (rsp+15)&-16 でマスクして16byte境界を動的に求める。current_rspの値さえ分かれば
// save/restoreどちらも同じ式で同じアドレスを再現できるため、追加のPCB状態は不要
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
    "    sub $528, %rsp\n"
    "    mov %rsp, %rax\n"
    "    add $15, %rax\n"
    "    and $-16, %rax\n"
    "    fxsave (%rax)\n"
    "    and $-16, %rsp\n"
    "    call c_timer_switch\n"
    "    mov %rax, %rdi\n"
    "    lea -528(%rdi), %rax\n"
    "    add $15, %rax\n"
    "    and $-16, %rax\n"
    "    fxrstor (%rax)\n"
    "    mov %rdi, %rsp\n"
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

/* [測定] 無音のハングを追うための簡易サンプラ。0でなければ、そのtick数ごとに
   「割り込まれた命令のアドレス」をシリアルへ出す。例外もパニックも出ないまま
   止まる場合、どこを回っているかはこれでしか分からない。
   asm_timer_handlerは15レジスタをpushしてからcurrent_rspを渡すので、
   IRETQフレーム(rip,cs,rflags,rsp,ss)はcurrent_rsp+15*8にある。

   **GC_DEBUG限定にしない。** GC_DEBUGビルドはcc_car内に呼び出しを入れるため
   インライン化の効果を打ち消してしまい、cc_car/cc_cdrのインライン化回帰は
   検出器を有効にすると消える。通常ビルドで観測できる手段が要る。
   コストはtickごとの剰余1回で、無効時(0)は比較1回だけである。 */
UINT64 g_tick_sample_interval = 0;

UINT64 SYSV_ABI c_timer_switch(UINT64 current_rsp) {
    outb(0x20, 0x20); // EOI を先に返す
    g_tick_counter++;
    if (g_tick_sample_interval != 0 && (g_tick_counter % g_tick_sample_interval) == 0) {
        /* 逆引きの基準点。ロードアドレスは実行ごとに変わるので毎回添える */
        os_diag_serial_write("[tick] anchor=");
        serial_write_hex64((UINT64)(lisp_addr_t)(void *)c_timer_switch);
        os_diag_serial_write(" rip=");
        serial_write_hex64(((UINT64 *)current_rsp)[15]);
        os_diag_serial_write(" gc=");
        serial_write_hex64(os_gc_collect_count());
        os_diag_serial_write(" zacalls=");
        serial_write_hex64(g_za_compile_calls);
        os_diag_serial_write(" jit=");
        serial_write_hex64(g_jit_used_for_diag());
        os_diag_serial_write(" mx=");
        for (int mi = 0; mi < 3; mi++) { serial_write_hex64(g_za_mx_calls[mi]); os_diag_serial_write("/"); }

        os_diag_serial_write("\n");
    }

    // [性能測定] Phase5 第0部: スタックガード。通常パスには乗らない位置で、
    // 割り込み時のrspとカナリアからスタック溢れを検出する(process.c参照)
    UINT64 stack_low = 0;
    UINT64 stack_used = 0;
    if (!os_process_stack_check(current_rsp, &stack_low, &stack_used)) {
        os_panic_stack_overflow(current_rsp, stack_low, stack_used);
    }

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

    UINT64 next_rsp = os_process_get_saved_rsp(cc_car(next_cell));
#ifdef ISIKIOS_GC_DEBUG
    /* [GC監査] GCの実行中に入ったtickを数える。上の*current-process* / run-queue/PCBの
       読み書きは、その瞬間には半端な状態を触っている可能性がある */
    if (g_gc_debug_in_gc) { g_gc_tick_during_gc++; }
#endif
#ifdef ISIKIOS_GC_DEBUG
    /* [GC監査] 復元しようとしているrspがどのプロセススタックにも属さないなら、
       ここが破壊の**発生源**である。asm_timer_handlerはこの値をrspに入れて
       15回popしてからiretqするので、実際に落ちるのはiretqの位置になり、
       例外ダンプだけを見ると「rspが壊れている」としか分からない
       (実測: rip=asm_timer_handler+0x67, rsp=ヒープ末尾+0x78 = 本値+0x78)。
       戻す前に捕まえて、PCBとrun-queueの中身をそのまま出す。 */
    if (!os_process_stack_contains(next_rsp)) {
        lisp_val_t pcb = cc_car(next_cell);
        UINT64 *w = (UINT64 *)(pcb & ~TAG_MASK);
        os_diag_serial_write("\nPANIC: c_timer_switch: 復元先rspがどのプロセススタックにも無い\n  next_rsp=");
        serial_write_hex64(next_rsp);
        os_diag_serial_write(" region=");
        serial_write_hex64((UINT64)os_addr_region((lisp_addr_t)next_rsp));
        os_diag_serial_write("\n  current_rsp=");
        serial_write_hex64(current_rsp);
        os_diag_serial_write("\n  next_cell=");
        serial_write_hex64((UINT64)next_cell);
        os_diag_serial_write(" tag=");
        serial_write_hex64((UINT64)(next_cell & TAG_MASK));
        os_diag_serial_write(" region=");
        serial_write_hex64((UINT64)os_addr_region((lisp_addr_t)(next_cell & ~TAG_MASK)));
        os_diag_serial_write("\n  pcb=");
        serial_write_hex64((UINT64)pcb);
        os_diag_serial_write(" tag=");
        serial_write_hex64((UINT64)(pcb & TAG_MASK));
        os_diag_serial_write(" region=");
        serial_write_hex64((UINT64)os_addr_region((lisp_addr_t)(pcb & ~TAG_MASK)));
        os_diag_serial_write("\n  pcb[0..3]=");
        for (int i = 0; i < 4; i++) { serial_write_hex64(w[i]); os_diag_serial_write(" "); }
        os_diag_serial_write("\n  current_cell=");
        serial_write_hex64((UINT64)current_cell);
        os_diag_serial_write(" gc_count=");
        serial_write_hex64(os_gc_collect_count());
        os_diag_serial_write("\n  in_gc=");
        serial_write_hex64((UINT64)g_gc_debug_in_gc);
        os_diag_serial_write(" tick_during_gc=");
        serial_write_hex64(g_gc_tick_during_gc);
        os_diag_serial_write(" current_process_sym_val=");
        serial_write_hex64((UINT64)os_get_variable(g_sym_current_process, global_environment));
        os_diag_serial_write(" run_queue=");
        serial_write_hex64((UINT64)os_get_variable(g_sym_run_queue, global_environment));
        os_diag_serial_write("\n");
        os_panic("c_timer_switch: saved_rsp corrupted (see serial)");
    }
#endif
    return next_rsp;
}

/**
 * IRQ0(PIT)のマスクだけを解除する。process_scheduler_startが全PCB / *RUN-QUEUE*を
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

/**
 * 64bit値を"0x"付き16桁16進数文字列としてfbへ書き込む(snprintf等が使えない
 * フリースタンディング環境向けの、この診断出力専用の簡易フォーマッタ)
 * @param fb 出力先
 * @param v 出力する値
 */
static void fb_write_hex64(frame_buffer *fb, uint64_t v) {
    static const char hex_digits[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = hex_digits[(v >> ((15 - i) * 4)) & 0xF];
    }
    buf[18] = '\0';
    fb->write_string(fb, buf);
    serial_write_string(buf);
}

/** fbへの出力と同時にserialへも同じ文字列を送る(-display noneでも診断内容を読めるようにする) */
static void diag_write_string(frame_buffer *fb, const char *s) {
    fb->write_string(fb, s);
    serial_write_string(s);
}

/**
 * GPF/PF発生時に、原因追跡に必要な最低限の情報(vector, error_code, rip, rsp, rflags、
 * PFの場合はCR2=フォルトしたアドレス)をfbへ表示してから停止する。
 * 元は単に'c'を1文字表示するだけのスタブだったため、どの命令が何のフォルトを
 * 起こしたのか一切分からず、GPF/PFが発生するとキーボード割り込みも二度と
 * 入らず無応答になる(cpu_exception_commonのhlt;jmp .-1がinterrupt gate経由=IF=0の
 * まま無限loopするため)問題そのものは今回のスコープ外だが、原因調査のため
 * 表示内容だけ拡張する
 */
void SYSV_ABI c_cpu_exception_handler(ExceptionContext *ctx, uint64_t fault_addr) {
    (void)fault_addr;
    /* [原則6] **まずシリアルへ最小限を出し切る。** この後のフレームバッファ描画も
       スタックdumpも、状況によってはハンドラ自身をフォルトさせる。実測では
       スタック溢れのとき、下のdumpがrsp(範囲外)を読んでダブルフォルト→
       トリプルフォルトになり、外からは「電源が落ちた」としか見えなかった */
    os_diag_serial_write("\n!! CPU EXCEPTION vector=");
    serial_write_hex64(ctx->vector);
    os_diag_serial_write(" rip=");
    serial_write_hex64(ctx->rip);
    os_diag_serial_write(" rsp=");
    serial_write_hex64(ctx->rsp);
    /* ハンドラ自身がどのスタックで動いているか。IST1が効いていればg_ist_stackの
       範囲(専用スタック)になる。効いていなければ溢れたスタックの続きで、
       そもそも報告できない */
    os_diag_serial_write(" handler_sp=");
    serial_write_hex64((uint64_t)(void *)&ctx);
    os_diag_serial_write(" ist=");
    serial_write_hex64((uint64_t)(void *)g_ist_stack);
    if (ctx->vector == 14) {
        uint64_t cr2v = 0;
        __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2v));
        os_diag_serial_write(" cr2=");
        serial_write_hex64(cr2v);
        if (os_process_in_stack_guard(cr2v)) {
            /* [原則6] ガードページを踏んだ。**どちら側かで原因が違う**ので区別する */
            if (os_process_guard_is_upper(cr2v, ctx->rsp)) {
                os_diag_serial_write("\n   ** GUARD HIT (上端側): スタック溢れではない。"
                                     "基底/上限の破壊かバッファオーバーラン **\n   stack=");
            } else {
                os_diag_serial_write("\n   ** STACK OVERFLOW (下端側): 再帰が深すぎる **\n   stack=");
            }
            serial_write_hex64(os_process_stack_base(0));
            os_diag_serial_write(" guard=");
            serial_write_hex64(os_process_guard_base(0));
            os_diag_serial_write(" guard_size=");
            serial_write_hex64(os_process_guard_size());
        }
    }
    if (ctx->vector == 8) {
        os_diag_serial_write("\n   (double fault: Cスタックの溢れが最有力)");
    } else if (!os_process_stack_contains(ctx->rsp)) {
        /* [原則6] rspがスタック範囲外なのは**溢れとは限らない**。
           ガードページ導入後は溢れなら境界で#PFになるので、ここへ来るのは
           むしろ「rspそのものが壊れている」場合である(実測で、監査ビルドの
           za_testがrsp=ヒープ末尾+0x78でasm_timer_handler内フォルトを起こした)。
           断定せず、事実だけを出す。 */
        os_diag_serial_write("\n   (rspがどのプロセススタックの範囲にもない: "
                             "溢れではなくrspの破壊の可能性。ガード側の表示も確認すること)");
    }
    os_diag_serial_write("\n");

    frame_buffer *fb = get_active_frame_buffer();

    diag_write_string(fb, "\n!! CPU EXCEPTION !!\nvector=");
    fb_write_hex64(fb, ctx->vector);
    diag_write_string(fb, " error_code=");
    fb_write_hex64(fb, ctx->error_code);
    diag_write_string(fb, "\nrip=");
    fb_write_hex64(fb, ctx->rip);
    diag_write_string(fb, " cs=");
    fb_write_hex64(fb, ctx->cs);
    diag_write_string(fb, "\nrsp=");
    fb_write_hex64(fb, ctx->rsp);
    diag_write_string(fb, " rflags=");
    fb_write_hex64(fb, ctx->rflags);

    if (ctx->vector == 14) {
        uint64_t cr2 = 0;
        asm volatile ("mov %%cr2, %0" : "=r"(cr2));
        diag_write_string(fb, "\ncr2(fault addr)=");
        fb_write_hex64(fb, cr2);
    }

    // [GC監査] ripだけでは発生地点がソースのどこか分からない。UEFIがイメージを
    // 読み込むアドレスは実行ごとに変わりうるので、既知のシンボル(このハンドラ
    // 自身)のアドレスを併記して、rip - handler の差からPE内のオフセットを
    // 逆算できるようにする(objdump -d でその差を持つ命令を探す)
    diag_write_string(fb, "\nhandler=");
    fb_write_hex64(fb, (uint64_t)(void *)&c_cpu_exception_handler);

    // [GC監査] 汎用レジスタ一式。塗り潰し(ISIKIOS_GC_PAINT)のトラップパターン
    // 0xDEADDEADDEADDEA6 は上位16bitが0xDEADでcanonicalでないため、これを
    // デリファレンスするとページフォルトではなく**GP例外(vector=13,
    // error_code=0)**になる。つまりGPの瞬間のレジスタにトラップパターンが
    // 載っていれば、その例外はstaleポインタの参照そのものである
    {
        static const char *const names[15] = {
            "r15", "r14", "r13", "r12", "r11", "r10", "r9", "r8",
            "rsi", "rdi", "rbp", "rdx", "rcx", "rbx", "rax"
        };
        const uint64_t *regs = &ctx->r15;
        int trap_seen = 0;
        for (int i = 0; i < 15; i++) {
            diag_write_string(fb, (i % 4 == 0) ? "\n" : " ");
            diag_write_string(fb, names[i]);
            diag_write_string(fb, "=");
            fb_write_hex64(fb, regs[i]);
            if ((regs[i] & ~(uint64_t)0x7) == (0xDEADDEADDEADDEA6ULL & ~(uint64_t)0x7)) {
                trap_seen = 1;
            }
        }
        if (trap_seen) {
            diag_write_string(fb, "\n** GC PAINT TRAP in register: stale pointer dereference **");
        }
    }

    // [GC監査] ripだけでは「staleを読んだ関数」(cc_car/cc_cdr等)しか分からず、
    // **保護を怠った呼び出し元**が分からない。呼び出し元は戻りアドレスとして
    // スタックに載っているので、フォルト時点のrspから一定語数を生のまま出す。
    // ホスト側(tools/bench/locate_rip.sh)がイメージ範囲に入る値だけを拾って
    // 関数名へ逆引きする。スタックは上位アドレス方向へ読むので、この深さで
    // 範囲外へ出ることはない
    /* rspが範囲外のときは読まない(読むとハンドラ自身がフォルトする) */
    if (!os_process_stack_contains(ctx->rsp)) {
        diag_write_string(fb, "\nstack: rspが範囲外のためdumpしない\n");
        return;
    }
    diag_write_string(fb, "\nstack(rsp..):");
    {
        const uint64_t *sp = (const uint64_t *)ctx->rsp;
        for (int i = 0; i < 32; i++) {
            diag_write_string(fb, (i % 4 == 0) ? "\n  " : " ");
            fb_write_hex64(fb, sp[i]);
        }
    }
    diag_write_string(fb, "\n");
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
/* istが非0なら、そのIST番号の専用スタックへ切り替えてからハンドラへ入る。
   スタック溢れで落ちる例外(GP/PF/ダブルフォルト)は、枯渇したスタックの上では
   報告すらできないので必ずISTを使う(原則6) */
static void set_idt_entry_ist(int vec, void *handler, uint8_t ist) {
    uint64_t addr = (uint64_t)handler;
    g_idt[vec].offset_low = addr & 0xFFFF;
    g_idt[vec].selector = 0x08;
    g_idt[vec].ist = ist;
    g_idt[vec].type_attr = 0x8E;
    g_idt[vec].offset_mid = (addr >> 16) & 0xFFFF;
    g_idt[vec].offset_high = (addr >> 32) & 0xFFFFFFFF;
    g_idt[vec].zero = 0;
}

static void set_idt_entry(int vec, void *handler) {
    set_idt_entry_ist(vec, handler, 0);
}

/** GDTを構築し、lgdt/lretqでコード・データセグメントを切り替える */
void init_gdt(void) {
    set_gdt_entry(0, 0x00, 0x00);
    set_gdt_entry(1, 0x9A, 0x20); // コード: access 0x9A, granularity 0x20(64bitフラグ)
    set_gdt_entry(2, 0x92, 0x00); // データ: access 0x92

    /* TSSディスクリプタ(インデックス3。長モードでは16byteなので4も占有する)。
       ISTの置き場として使うだけで、タスク切り替えには使わない */
    {
        uint64_t base = (uint64_t)&g_tss;
        uint32_t limit = (uint32_t)(sizeof(g_tss) - 1);
        uint8_t *d = (uint8_t *)&g_gdt[3];
        for (int i = 0; i < 16; i++) { d[i] = 0; }
        d[0] = (uint8_t)(limit & 0xFF);
        d[1] = (uint8_t)((limit >> 8) & 0xFF);
        d[2] = (uint8_t)(base & 0xFF);
        d[3] = (uint8_t)((base >> 8) & 0xFF);
        d[4] = (uint8_t)((base >> 16) & 0xFF);
        d[5] = 0x89; /* present, type=9 (available 64-bit TSS) */
        d[6] = (uint8_t)((limit >> 16) & 0x0F);
        d[7] = (uint8_t)((base >> 24) & 0xFF);
        d[8] = (uint8_t)((base >> 32) & 0xFF);
        d[9] = (uint8_t)((base >> 40) & 0xFF);
        d[10] = (uint8_t)((base >> 48) & 0xFF);
        d[11] = (uint8_t)((base >> 56) & 0xFF);
    }
    /* IST1 = 専用スタックの**上端**(スタックは下方向に伸びる) */
    g_tss.ist[0] = (uint64_t)(g_ist_stack + IST_STACK_SIZE);
    g_tss.iomap_base = (uint16_t)sizeof(g_tss);

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
    /* TSSセレクタ(GDTインデックス3 = オフセット0x18)をロードする */
    asm volatile("mov $0x18, %%ax\n ltr %%ax\n" : : : "ax");
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

/** init_fpuがfxsaveしたFPU/SSEのデフォルト初期状態(512byte)。spawn()が各プロセスの
 * 偽フレームのFXSAVE領域をこの内容で初期化するために参照する */
static UINT8 g_fpu_default_state[512] __attribute__((aligned(16)));

const void *get_fpu_default_state(void) {
    return g_fpu_default_state;
}

/**
 * CR0/CR4のFPU/SSE関連ビットを設定し、fninit+ldmxcsrでFPU/SSE状態を初期化する。
 * 以後どのC関数もx87/SSE命令(double演算等)を使えるようになる。
 * x86-64はSSE2/FXSRを仕様上必ず備えるため、CPUIDによる機能検出は行わない
 */
void init_fpu(void) {
    uint64_t cr0, cr4;
    asm volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);               // EM=0 (x87/SSEをソフトウェアエミュレーションしない)
    cr0 |= (1ULL << 1) | (1ULL << 5);  // MP=1, NE=1(ネイティブ#MFレポート)
    asm volatile ("mov %0, %%cr0" : : "r"(cr0));

    asm volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9) | (1ULL << 10); // OSFXSR=1, OSXMMEXCPT=1
    asm volatile ("mov %0, %%cr4" : : "r"(cr4));

    asm volatile ("fninit");
    uint32_t default_mxcsr = 0x1F80;   // 全SIMD例外マスク、round-to-nearest
    asm volatile ("ldmxcsr %0" : : "m"(default_mxcsr));

    asm volatile ("fxsave %0" : "=m"(g_fpu_default_state));
}

/** IDTを構築し、GPF/PF/タイマー/キーボードの各ハンドラを登録してlidt/stiする */
void init_idt(void) {
    serial_init();
    /* スタック溢れはGP/PFとして現れ、ハンドラ自身がpushできずダブルフォルトへ
       進む。IST1(専用スタック)で受けることで診断を出せるようにする */
    set_idt_entry_ist(8, (void *)asm_df_handler, 1);
    set_idt_entry_ist(13, (void *)asm_gpf_handler, 1);
    set_idt_entry_ist(14, (void *)asm_pf_handler, 1);
    set_idt_entry(32, (void *)asm_timer_handler);
    set_idt_entry(33, (void *)asm_keyboard_handler);

    g_idt_ptr.limit = sizeof(g_idt) - 1;
    g_idt_ptr.base = (uint64_t)&g_idt;

    asm volatile ("lidt %0" : : "m"(g_idt_ptr));
    asm volatile ("sti");
}



