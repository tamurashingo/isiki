#include "print.h"
#include "lisp.h"
#include "framebuffer.h"

static void print_value(os_char_sink_t *sink, lisp_val_t val, int escaped);

static void sink_write_char(os_char_sink_t *sink, UINT8 c) {
    sink->write_char(sink->ctx, c);
}

static void sink_write_string(os_char_sink_t *sink, const char *s) {
    while (*s != '\0') {
        sink_write_char(sink, (UINT8)*s);
        s++;
    }
}

/**
 * UINT64を10進数の数字列にして出力する(標準ライブラリのitoa等は使えないため自前実装)。
 * @param sink 出力先のシンク
 * @param value 出力する値
 */
static void print_fixnum(os_char_sink_t *sink, UINT64 value) {
    if (value == 0) {
        sink_write_char(sink, '0');
        return;
    }
    char digits[20]; // UINT64の最大10進桁数
    int len = 0;
    while (value > 0) {
        digits[len++] = '0' + (char)(value % 10);
        value /= 10;
    }
    while (len > 0) {
        sink_write_char(sink, (UINT8)digits[--len]);
    }
}

/**
 * bignum(MAGIC_BIGNUM)のlimb配列を10進数で出力する。limb配列はmag_divmod_smallで
 * 破壊的に10で割り続けるため、出力前に別バッファへコピーしておく。
 * @param sink 出力先のシンク
 * @param val 出力するbignum(TAG_INSTANCE、word0==MAGIC_BIGNUM)
 */
static void print_bignum(os_char_sink_t *sink, lisp_val_t val) {
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    UINT64 sign = obj[1];
    UINT64 count = obj[2];
    UINT64 *src = (UINT64 *)obj[3];

    UINT64 *work = (UINT64 *)os_alloc_raw(8 * count);
    for (UINT64 i = 0; i < count; i++) {
        work[i] = src[i];
    }

    if (sign) {
        sink_write_char(sink, '-');
    }

    // 1limb(基数2^32)あたり最大10進10桁(log10(2^32) < 9.63)なので10*countで十分
    char *digits = (char *)os_alloc_raw(10 * count);
    int len = 0;
    while (count > 1 || work[0] != 0) {
        UINT64 rem;
        count = mag_divmod_small(work, count, 10, &rem);
        digits[len++] = '0' + (char)rem;
    }
    while (len > 0) {
        sink_write_char(sink, (UINT8)digits[--len]);
    }
}

/**
 * STRINGオブジェクトのレイアウト([len(8byte)][chars...])に従ってバイト列を出力する。
 * @param sink 出力先のシンク
 * @param str_addr STRINGオブジェクト本体(タグを除いた先頭)のアドレス
 */
static void print_bytes(os_char_sink_t *sink, lisp_addr_t str_addr) {
    UINT64 len = ((UINT64 *)str_addr)[0];
    const char *bytes = (const char *)(str_addr + 8);
    for (UINT64 i = 0; i < len; i++) {
        sink_write_char(sink, (UINT8)bytes[i]);
    }
}

/**
 * SYMBOLの名前を出力する。
 * @param sink 出力先のシンク
 * @param val 出力するSYMBOL
 */
static void print_symbol(os_char_sink_t *sink, lisp_val_t val) {
    lisp_val_t name_str = ((lisp_val_t *)(val & ~TAG_MASK))[0];
    print_bytes(sink, name_str & ~TAG_MASK);
}

/**
 * STRINGを出力する。escapedが真ならダブルクオートで囲む(prin1相当)、
 * 偽なら内容のみをそのまま出力する(princ相当)。
 * @param sink 出力先のシンク
 * @param val 出力するSTRING
 * @param escaped ダブルクオートで囲むかどうか
 */
static void print_string(os_char_sink_t *sink, lisp_val_t val, int escaped) {
    if (escaped) {
        sink_write_char(sink, '"');
    }
    print_bytes(sink, val & ~TAG_MASK);
    if (escaped) {
        sink_write_char(sink, '"');
    }
}

/**
 * CONSのリストを"(a b c)"形式で出力する。nilで終端しない場合はドット対記法で表示する。
 * @param sink 出力先のシンク
 * @param val 出力するCONS
 * @param escaped 要素の出力にprin1/princどちらの規則を使うか
 */
static void print_list(os_char_sink_t *sink, lisp_val_t val, int escaped) {
    sink_write_char(sink, '(');
    lisp_val_t current = val;
    int first = 1;
    while (current != nil && (current & TAG_MASK) == TAG_CONS) {
        if (!first) {
            sink_write_char(sink, ' ');
        }
        first = 0;
        print_value(sink, cc_car(current), escaped);
        current = cc_cdr(current);
    }
    if (current != nil) {
        // 末尾がnilで終わらないconsはドット対記法で表示する
        sink_write_string(sink, " . ");
        print_value(sink, current, escaped);
    }
    sink_write_char(sink, ')');
}

/**
 * VECTORを"#(a b c)"形式で出力する。多次元の場合も次元の区切りは付けず、
 * 要素本体をrow-major順にフラットに並べて表示する。
 * @param sink 出力先のシンク
 * @param val 出力するVECTOR
 * @param escaped 要素の出力にprin1/princどちらの規則を使うか
 */
static void print_vector(os_char_sink_t *sink, lisp_val_t val, int escaped) {
    lisp_val_t *header = os_vector_header(val);
    UINT64 rank = header[0];
    UINT64 total = 1;
    for (UINT64 i = 0; i < rank; i++) {
        total *= header[1 + i];
    }
    lisp_val_t *data = header + 1 + rank;

    sink_write_char(sink, '#');
    sink_write_char(sink, '(');
    for (UINT64 i = 0; i < total; i++) {
        if (i != 0) {
            sink_write_char(sink, ' ');
        }
        print_value(sink, data[i], escaped);
    }
    sink_write_char(sink, ')');
}

/**
 * valをTAGに応じて出力する(os_print_to_sinkの実処理本体)。
 * @param sink 出力先のシンク
 * @param val 出力するLisp値
 * @param escaped STRINGをprin1相当(ダブルクオート付き)で出力するかprinc相当で出力するか
 */
static void print_value(os_char_sink_t *sink, lisp_val_t val, int escaped) {
    if (val == nil) {
        // NILはcar/cdrが自分自身を指す循環consのため、専用処理せず
        // TAG_CONSの分岐に入ると無限再帰するので先に判定する
        sink_write_string(sink, "NIL");
        return;
    }

    switch (val & TAG_MASK) {
        case TAG_FIXNUM:
            if (os_fixnum_is_negative(val)) {
                sink_write_char(sink, '-');
            }
            print_fixnum(sink, os_fixnum_magnitude(val));
            return;
        case TAG_SYMBOL:
            print_symbol(sink, val);
            return;
        case TAG_STRING:
            print_string(sink, val, escaped);
            return;
        case TAG_CHAR:
            sink_write_char(sink, (UINT8)(val >> 3));
            return;
        case TAG_CONS:
            print_list(sink, val, escaped);
            return;
        case TAG_INSTANCE: {
            UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
            UINT64 magic = obj[0];
            if (magic == MAGIC_PROCESS) {
                sink_write_string(sink, "#<PROCESS>");
            } else if (magic == MAGIC_BIGNUM) {
                print_bignum(sink, val);
            } else if (magic == MAGIC_VECTOR) {
                print_vector(sink, val, escaped);
            } else if (magic == MAGIC_STREAM) {
                sink_write_string(sink, "#<STREAM>");
            } else if (magic == MAGIC_CLASS) {
                sink_write_string(sink, "#<CLASS ");
                print_value(sink, obj[1], escaped);
                sink_write_char(sink, '>');
            } else if (magic == MAGIC_CLASS_INSTANCE) {
                UINT64 *cls = (UINT64 *)(obj[1] & ~TAG_MASK);
                sink_write_string(sink, "#<INSTANCE-OF ");
                print_value(sink, cls[1], escaped);
                sink_write_char(sink, '>');
            } else {
                sink_write_string(sink, "#<FUNCTION>");
            }
            return;
        }
    }
}

void os_print_to_sink(lisp_val_t val, os_char_sink_t *sink, int escaped) {
    print_value(sink, val, escaped);
}

static void frame_buffer_sink_write_char(void *ctx, UINT8 c) {
    frame_buffer *fb = (frame_buffer *)ctx;
    fb->write_char(fb, c);
}

lisp_val_t os_print(lisp_val_t val, frame_buffer *fb) {
    os_char_sink_t sink = { .ctx = fb, .write_char = frame_buffer_sink_write_char };
    print_value(&sink, val, 1);
    return val;
}
