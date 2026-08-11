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
 * doubleをISLisp §19.2相当の10進表記でsinkへ出力する(strtod/printf系が無い前提の
 * 手書き実装)。符号・0を特別扱いした後、1<=|x|/10^e<10となる10進指数eを求め、
 * 仮数から有効17桁(doubleを可逆変換できる最大桁数)を上位から順に取り出し、
 * 末尾の0を(小数点以下最低1桁を残して)取り除く。指数の大小で固定小数点表記
 * ("123.456")かE表記("1.23456E20")かを切り替える。
 * @param sink 出力先のシンク
 * @param value 出力するdouble値
 */
void os_print_double_to_sink(os_char_sink_t *sink, double value) {
    if (value != value) {
        sink_write_string(sink, "NAN");
        return;
    }

    int negative = 0;
    if (value < 0.0) {
        negative = 1;
        value = -value;
    }
    if (negative) {
        sink_write_char(sink, '-');
    }

    if (value == 0.0) {
        sink_write_string(sink, "0.0");
        return;
    }

    // value == +infになるケース(±1.7976931348623157E308を超える等)はinfとして扱う
    double check_inf = value * 10.0;
    if (check_inf == value && value > 1.0) {
        sink_write_string(sink, "INF");
        return;
    }

    int exp10 = 0;
    while (value >= 10.0) {
        value /= 10.0;
        exp10++;
    }
    while (value < 1.0) {
        value *= 10.0;
        exp10--;
    }

    #define FLOAT_SIG_DIGITS 17
    char digits[FLOAT_SIG_DIGITS];
    double m = value;
    for (int i = 0; i < FLOAT_SIG_DIGITS; i++) {
        int digit = (int)m;
        if (digit > 9) {
            digit = 9; // 浮動小数点誤差で10になるのを防ぐ
        }
        digits[i] = (char)('0' + digit);
        m = (m - digit) * 10.0;
    }

    int ndigits = FLOAT_SIG_DIGITS;
    while (ndigits > 1 && digits[ndigits - 1] == '0') {
        ndigits--;
    }

    if (exp10 >= -3 && exp10 < FLOAT_SIG_DIGITS) {
        // 固定小数点表記
        if (exp10 >= 0) {
            for (int i = 0; i <= exp10; i++) {
                sink_write_char(sink, (UINT8)((i < ndigits) ? digits[i] : '0'));
            }
            sink_write_char(sink, '.');
            if (exp10 + 1 >= ndigits) {
                sink_write_char(sink, '0');
            } else {
                for (int i = exp10 + 1; i < ndigits; i++) {
                    sink_write_char(sink, (UINT8)digits[i]);
                }
            }
        } else {
            sink_write_string(sink, "0.");
            for (int i = 0; i < -exp10 - 1; i++) {
                sink_write_char(sink, '0');
            }
            for (int i = 0; i < ndigits; i++) {
                sink_write_char(sink, (UINT8)digits[i]);
            }
        }
    } else {
        // E表記
        sink_write_char(sink, (UINT8)digits[0]);
        sink_write_char(sink, '.');
        if (ndigits == 1) {
            sink_write_char(sink, '0');
        } else {
            for (int i = 1; i < ndigits; i++) {
                sink_write_char(sink, (UINT8)digits[i]);
            }
        }
        sink_write_char(sink, 'E');
        if (exp10 < 0) {
            sink_write_char(sink, '-');
            exp10 = -exp10;
        }
        print_fixnum(sink, (UINT64)exp10);
    }
    #undef FLOAT_SIG_DIGITS
}

/**
 * MAGIC_FLOATのINSTANCEをdoubleへ戻してprint_doubleで出力する。
 * @param sink 出力先のシンク
 * @param val 出力するfloat(TAG_INSTANCE、word0==MAGIC_FLOAT)
 */
static void print_float(os_char_sink_t *sink, lisp_val_t val) {
    os_print_double_to_sink(sink, os_float_value(val));
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
            } else if (magic == MAGIC_FLOAT) {
                print_float(sink, val);
            } else if (magic == MAGIC_STREAM) {
                sink_write_string(sink, "#<STREAM>");
            } else if (magic == MAGIC_BUILTIN_CLASS || magic == MAGIC_STANDARD_CLASS) {
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
