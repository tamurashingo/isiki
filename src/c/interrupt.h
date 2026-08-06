#ifndef _INTERRUPT_H_
#define _INTERRUPT_H_

#include <stdint.h>

/** GDT (Global Descriptor Table) の1エントリ */
struct gdt_entry {
    /** セグメントリミット下位16bit */
    uint16_t limit_low;
    /** ベースアドレス下位16bit */
    uint16_t base_low;
    /** ベースアドレス中位8bit */
    uint8_t base_mid;
    /** アクセス権(タイプ, DPL, present等) */
    uint8_t access;
    /** リミット上位4bit + フラグ(粒度, サイズ等) */
    uint8_t granularity;
    /** ベースアドレス上位8bit */
    uint8_t base_high;
} __attribute__((packed));

/** lgdt命令に渡すGDT */
struct gdt_ptr {
    /** GDT全体のサイズ - 1 (バイト数) */
    uint16_t limit;
    /** GDTの先頭アドレス */
    uint64_t base;
} __attribute__((packed));


/** IDT (Interrupt Descriptor Table) の1エントリ */
struct idt_entry {
    /** ハンドラアドレス下位16bit */
    uint16_t offset_low;
    /** ハンドラ実行時に使うコードセグメントセレクタ */
    uint16_t selector;
    /** Interrupt Stack Table のインデックス */
    uint8_t ist;
    /** ゲートタイプ, DPL, present フラグ */
    uint8_t type_attr;
    /** ハンドラアドレス中位16bit */
    uint16_t offset_mid;
    /** ハンドラアドレス上位32bit */
    uint32_t offset_high;
    /** 予約領域(0固定) */
    uint32_t zero;
} __attribute__((packed));

/** lidt命令に渡すIDT */
struct idt_ptr {
    /** IDT全体のサイズ - 1 (バイト数) */
    uint16_t limit;
    /** IDTの先頭アドレス */
    uint64_t base;
} __attribute__((packed));

/** 割り込みハンドラの引数型として使う前方宣言のみの構造体 */
struct interrupt_frame;


/** 割り込み/例外発生時にスタックに積まれるレジスタ一式 */
typedef struct {
    /** 割り込みハンドラ内でpushされた汎用レジスタ(pushad相当、push順の逆順) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rsi, rdi, rbp, rdx, rcx, rbx, rax;
    /** 発生した割り込み/例外番号 */
    uint64_t vector;
    /** CPUが積むエラーコード(無い場合は0など) */
    uint64_t error_code;
    /** 割り込み発生時にCPUが自動的に積む実行コンテキスト */
    uint64_t rip, cs, rflags, rsp, ss;
} ExceptionContext;


/** GDTを構築し、lgdt/lretqでコード・データセグメントを切り替える */
void init_gdt(void);

/** PICを初期化し、IRQ1(キーボード)のみを許可した状態にする(IRQ0は未許可のまま) */
void init_pic(void);

/** IDTを構築し、GPF/PF/タイマー/キーボードの各ハンドラを登録してlidt/stiする */
void init_idt(void);

/** PITをチャンネル0・lobyte/hibyte・モード3で約100Hzに設定する */
void init_pit(void);

/**
 * IRQ0(PIT)のマスクを解除し、タイマー割り込みによるプリエンプションを開始する。
 * *RUN-QUEUE* と PCB がすべて構築された後に呼ぶこと。
 */
void enable_timer_irq(void);

#endif /* _INTERRUPT_H_ */
