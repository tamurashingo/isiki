#ifndef _INTERRUPT_H_
#define _INTERRUPT_H_

#include <stdint.h>

/* GDT (Global Descriptor Table) */
struct gdt_entry {
    uint16_t limit_low;     /* セグメントリミット下位16bit */
    uint16_t base_low;      /* ベースアドレス下位16bit */
    uint8_t base_mid;       /* ベースアドレス中位8bit */
    uint8_t access;         /* アクセス権(タイプ, DPL, present等) */
    uint8_t granularity;    /* リミット上位4bit + フラグ(粒度, サイズ等) */
    uint8_t base_high;      /* ベースアドレス上位8bit */
} __attribute__((packed));

/* lgdt命令に渡すGDT */
struct gdt_ptr {
    uint16_t limit;  /* GDT全体のサイズ - 1 (バイト数) */
    uint64_t base;   /* GDTの先頭アドレス */
} __attribute__((packed));


/* IDT (Interrupt Descriptor Table) */
struct idt_entry {
    uint16_t offset_low;   /* ハンドラアドレス下位16bit */
    uint16_t selector;     /* ハンドラ実行時に使うコードセグメントセレクタ */
    uint8_t ist;           /* Interrupt Stack Table のインデックス */
    uint8_t type_attr;     /* ゲートタイプ, DPL, present フラグ */
    uint16_t offset_mid;   /* ハンドラアドレス中位16bit */
    uint32_t offset_high;  /* ハンドラアドレス上位32bit */
    uint32_t zero;         /* 予約領域(0固定) */
} __attribute__((packed));

/* lidt命令に渡すIDT */
struct idt_ptr {
    uint16_t limit;  /* IDT全体のサイズ - 1 (バイト数) */
    uint64_t base;   /* IDTの先頭アドレス */
} __attribute__((packed));

/* 割り込みハンドラの引数型として使う前方宣言のみの構造体 */
struct interrupt_frame;


/* 割り込み/例外発生時にスタックに積まれるレジスタ一式 */
typedef struct {
    /* 割り込みハンドラ内でpushされた汎用レジスタ(pushad相当、push順の逆順) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rsi, rdi, rbp, rdx, rcx, rbx, rax;
    uint64_t vector;       /* 発生した割り込み/例外番号 */
    uint64_t error_code;   /* CPUが積むエラーコード(無い場合は0など) */
    /* 割り込み発生時にCPUが自動的に積む実行コンテキスト */
    uint64_t rip, cs, rflags, rsp, ss;
} ExceptionContext;


void init_gdt(void);
void init_pic(void);
void init_idt(void);

#endif /* _INTERRUPT_H_ */
