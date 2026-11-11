#ifndef _KEYMAP_H_
#define _KEYMAP_H_

#include "types.h"

/* [#216] **キーの取り扱いの「核」**。スキャンコードを見て「何をするか」だけを決める。
 *
 * ## なぜ interrupt.c から切り出したのか
 *
 * 調査(docs/investigation/app-terminal-io.logic.md §9-15)は
 * 「割り込みの層はどの単体試験にもリンクされておらず、**キー入力の仕組みは
 * 全体が無試験**」と報告している。無試験だった理由は中身ではなく**在りか**で、
 * interrupt.c は GDT/IDT/TSS と手書きアセンブラを抱えているため
 * 母艦の単体試験からリンクできない(refs.md §12 の ★)。
 *
 * そこで **決める部分(ここ)と、効果を起こす部分(interrupt.c)を分けた。**
 * このファイルはハードウェアに触らず、画面にもプロセスにも依存しない。
 * だから test/c/keymap_test.c から単体でリンクできる。
 * apps/pager.lisp / apps/inspect.lisp が「核はストリームを受け取る関数、
 * 皮だけが画面とキーボードを知っている」形にしてあるのと同じ分け方である。
 *
 * ## 判定の順序は変えていない
 *
 * 修飾キー → Break コードの破棄 → F1〜F4 → Shift+PgUp/PgDn → (**ここに番号の分岐を足した**)
 * → 変換表。F1〜F4 を変換表より先に見る理由は interrupt.c のコメントに
 * 残っていたものをそのまま引き継いでいる(逆にするとさかのぼり位置が
 * バッファごとに保たれるという規則と矛盾する)。
 *
 * ## 拡張キーの前置き(0xE0)の扱いは変えていない
 *
 * 矢印が今まで取れなかったのは前置きを捨てているからではなく、
 * **変換表にその番号が載っていなかったから**である(logic §9-2 の前提の訂正)。
 * 前置きは Break コードと同じ上位ビットを持つのでここでも捨てられ、
 * 矢印の番号は前置き無しで届く。だから**変換表を引く前に番号で分岐する**だけで取れる。
 * 前例は Shift+PgUp/PgDn(もとの interrupt.c:320-324)である。
 *
 * ★ 未確認: 拡張キーの一部は偽の修飾キー列(0xE0 0x2A など)を伴うことがあり、
 * 前置きが剥がれた 0x2A が届くと Shift の状態が勝手に変わりうる(logic §9-6)。
 * **走らせて確かめていない。** 実機で矢印を押すと Shift が立つ症状が出たら、
 * まずここを疑うこと。
 */

/** キーを押したときに何をするか */
typedef enum {
    /** 何もしない(Break コード・未登録のキー) */
    KEY_ACTION_NONE = 0,
    /** 修飾キーの状態が変わっただけ */
    KEY_ACTION_MODIFIER,
    /** 表示フォーカスを value 番のプロセスへ移す(F1〜F4) */
    KEY_ACTION_SWITCH_PROCESS,
    /** さかのぼり表示を動かす。value は 1 なら過去へ、0 なら未来へ 1 画面 */
    KEY_ACTION_SCROLLBACK,
    /** 文字コード value を入力として流す */
    KEY_ACTION_CHAR,
} key_action_kind_t;

typedef struct {
    key_action_kind_t kind;
    UINT32 value;
} key_action_t;

/* 変換表に無いキーに割り当てる文字コード。
 *
 * **1 バイトに収めている。** 入力の溜め場(process_t の stdin_buf)と
 * 1 キーずつの受け皿がどちらも UINT8 の列で、read-char は
 * `os_make_char((UINT8)ch)`(stream_lisp.c)で**ゼロ拡張**して CHARACTER にするため、
 * 0x80〜0xFF はそのまま Lisp へ渡る。
 *
 * ESC/Tab/Backspace/Delete は ASCII の定位置を使う(0x1B/0x09/0x08/0x7F)。
 * 矢印などは ASCII に場所が無いので 0x80 からの並びに置いた。
 * **格子には入るが字形は 7bit ぶんしか無い**ので、エコーすれば '?' に見える
 * (framebuffer.c:52-54)。素の入力ではエコーしないので問題にならない。 */
#define KEY_CODE_ESCAPE    0x1B
#define KEY_CODE_TAB       0x09
#define KEY_CODE_BACKSPACE 0x08
#define KEY_CODE_DELETE    0x7F
#define KEY_CODE_UP        0x80
#define KEY_CODE_DOWN      0x81
#define KEY_CODE_LEFT      0x82
#define KEY_CODE_RIGHT     0x83
#define KEY_CODE_HOME      0x84
#define KEY_CODE_END       0x85
#define KEY_CODE_PAGE_UP   0x86
#define KEY_CODE_PAGE_DOWN 0x87
#define KEY_CODE_INSERT    0x88

/**
 * スキャンコード 1 つを見て、何をするかを決める。修飾キーの状態はこの中で更新する。
 * @param scancode PS/2 キーボードから受け取ったスキャンコード
 * @return 何をするか
 */
key_action_t os_keymap_decode(UINT8 scancode);

/** 修飾キーの状態を初期状態へ戻す(試験が 1 件ごとに独立できるようにするため) */
void os_keymap_reset(void);

/** Shift が押されているか(試験と診断のため) */
int os_keymap_shift_pressed(void);

/** Ctrl が押されているか(試験と診断のため) */
int os_keymap_ctrl_pressed(void);

#endif /* _KEYMAP_H_ */
