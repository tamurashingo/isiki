#include "print.h"
#include "lisp.h"
#include "framebuffer.h"

static void print_value(frame_buffer *fb, lisp_val_t val);

/**
 * UINT64を10進数の数字列にして出力する(標準ライブラリのitoa等は使えないため自前実装)。
 * @param fb 出力先のframe buffer
 * @param value 出力する値
 */
static void print_fixnum(frame_buffer *fb, UINT64 value) {
    if (value == 0) {
        fb->write_char(fb, '0');
        return;
    }
    char digits[20]; // UINT64の最大10進桁数
    int len = 0;
    while (value > 0) {
        digits[len++] = '0' + (char)(value % 10);
        value /= 10;
    }
    while (len > 0) {
        fb->write_char(fb, (UINT8)digits[--len]);
    }
}

/**
 * STRINGオブジェクトのレイアウト([len(8byte)][chars...])に従ってバイト列を出力する。
 * @param fb 出力先のframe buffer
 * @param str_addr STRINGオブジェクト本体(タグを除いた先頭)のアドレス
 */
static void print_bytes(frame_buffer *fb, lisp_addr_t str_addr) {
    UINT64 len = ((UINT64 *)str_addr)[0];
    const char *bytes = (const char *)(str_addr + 8);
    for (UINT64 i = 0; i < len; i++) {
        fb->write_char(fb, (UINT8)bytes[i]);
    }
}

/**
 * SYMBOLの名前を出力する。
 * @param fb 出力先のframe buffer
 * @param val 出力するSYMBOL
 */
static void print_symbol(frame_buffer *fb, lisp_val_t val) {
    lisp_val_t name_str = ((lisp_val_t *)(val & ~TAG_MASK))[0];
    print_bytes(fb, name_str & ~TAG_MASK);
}

/**
 * STRINGをダブルクオートで囲んで出力する。
 * @param fb 出力先のframe buffer
 * @param val 出力するSTRING
 */
static void print_string(frame_buffer *fb, lisp_val_t val) {
    fb->write_char(fb, '"');
    print_bytes(fb, val & ~TAG_MASK);
    fb->write_char(fb, '"');
}

/**
 * CONSのリストを"(a b c)"形式で出力する。nilで終端しない場合はドット対記法で表示する。
 * @param fb 出力先のframe buffer
 * @param val 出力するCONS
 */
static void print_list(frame_buffer *fb, lisp_val_t val) {
    fb->write_char(fb, '(');
    lisp_val_t current = val;
    int first = 1;
    while (current != nil && (current & TAG_MASK) == TAG_CONS) {
        if (!first) {
            fb->write_char(fb, ' ');
        }
        first = 0;
        print_value(fb, cc_car(current));
        current = cc_cdr(current);
    }
    if (current != nil) {
        // 末尾がnilで終わらないconsはドット対記法で表示する
        fb->write_string(fb, " . ");
        print_value(fb, current);
    }
    fb->write_char(fb, ')');
}

/**
 * valをTAGに応じて出力する(os_printの実処理本体)。
 * @param fb 出力先のframe buffer
 * @param val 出力するLisp値
 */
static void print_value(frame_buffer *fb, lisp_val_t val) {
    if (val == nil) {
        // NILはcar/cdrが自分自身を指す循環consのため、専用処理せず
        // TAG_CONSの分岐に入ると無限再帰するので先に判定する
        fb->write_string(fb, "NIL");
        return;
    }

    switch (val & TAG_MASK) {
        case TAG_FIXNUM:
            print_fixnum(fb, val >> 3);
            return;
        case TAG_SYMBOL:
            print_symbol(fb, val);
            return;
        case TAG_STRING:
            print_string(fb, val);
            return;
        case TAG_CHAR:
            fb->write_char(fb, (UINT8)(val >> 3));
            return;
        case TAG_CONS:
            print_list(fb, val);
            return;
        case TAG_INSTANCE: {
            UINT64 magic = ((UINT64 *)(val & ~TAG_MASK))[0];
            if (magic == MAGIC_PROCESS) {
                fb->write_string(fb, "#<PROCESS>");
            } else {
                fb->write_string(fb, "#<FUNCTION>");
            }
            return;
        }
    }
}

/**
 * val を TAG に応じて fb に表示する。
 * 表示した val 自身を返す(REPLでの `(print (eval (read)))` 的な合成のため)。
 * @param val 表示するLisp値
 * @param fb 表示先のframe buffer
 * @return val 自身
 */
lisp_val_t os_print(lisp_val_t val, frame_buffer *fb) {
    print_value(fb, val);
    return val;
}
