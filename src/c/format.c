#include "format.h"
#include "runtime.h"
#include "lisp.h"
#include "stream.h"
#include "stream_lisp.h"
#include "print.h"

/** STRINGオブジェクトのレイアウト([len(8byte)][chars...])からデータ先頭とバイト数を取り出す */
static void format_string_bytes(lisp_val_t str, const UINT8 **out_data, UINT32 *out_len) {
    lisp_addr_t addr = str & ~TAG_MASK;
    UINT64 len = ((UINT64 *)addr)[0];
    *out_data = (const UINT8 *)(addr + 8);
    *out_len = (UINT32)len;
}

static void stream_sink_write_char(void *ctx, UINT8 c) {
    os_stream_write_char((os_stream_t *)ctx, (char)c);
}

/** streamに直接書き込むos_char_sink_tを組み立てる(columnはos_stream_write_char内で追跡される) */
static os_char_sink_t make_stream_sink(os_stream_t *raw) {
    os_char_sink_t sink;
    sink.ctx = raw;
    sink.write_char = stream_sink_write_char;
    return sink;
}

/**
 * integerをradix進数の数字列でsinkへ出力する。bignum(MAGIC_BIGNUM)はradix=10のみ対応
 * (既存のprint機構をそのまま再利用する)。それ以外のradixでのbignumは未対応。
 */
static void format_integer_write(os_char_sink_t *sink, lisp_val_t integer, int radix) {
    if ((integer & TAG_MASK) == TAG_INSTANCE) {
        os_print_to_sink(integer, sink, 0);
        return;
    }
    int negative = os_fixnum_is_negative(integer);
    UINT64 magnitude = os_fixnum_magnitude(integer);
    if (negative) {
        sink->write_char(sink->ctx, '-');
    }
    if (magnitude == 0) {
        sink->write_char(sink->ctx, '0');
        return;
    }
    char digits[64]; // UINT64の2進表記でも64桁で足りる
    int len = 0;
    while (magnitude > 0) {
        UINT64 d = magnitude % (UINT64)radix;
        digits[len++] = (d < 10) ? ('0' + (char)d) : ('A' + (char)(d - 10));
        magnitude /= (UINT64)radix;
    }
    while (len > 0) {
        sink->write_char(sink->ctx, (UINT8)digits[--len]);
    }
}

/** floatをprint.cのos_print_double_to_sinkと同じロジックで10進表記にして出力する */
static void format_float_write(os_char_sink_t *sink, lisp_val_t obj) {
    os_print_double_to_sink(sink, os_float_value(obj));
}

lisp_val_t cc_format_char(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_t *raw = os_stream_from_lisp(cc_car(args));
    lisp_val_t ch = cc_car(cc_cdr(args));
    os_stream_write_char(raw, (char)(ch >> 3));
    return nil;
}

lisp_val_t cc_format_float(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_t *raw = os_stream_from_lisp(cc_car(args));
    lisp_val_t obj = cc_car(cc_cdr(args));
    os_char_sink_t sink = make_stream_sink(raw);
    format_float_write(&sink, obj);
    return nil;
}

lisp_val_t cc_format_fresh_line(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_t *raw = os_stream_from_lisp(cc_car(args));
    if (raw->column != 0) {
        os_stream_write_char(raw, '\n');
    }
    return nil;
}

lisp_val_t cc_format_integer(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_t *raw = os_stream_from_lisp(cc_car(args));
    lisp_val_t integer = cc_car(cc_cdr(args));
    lisp_val_t radix_val = cc_car(cc_cdr(cc_cdr(args)));
    int radix = (int)os_fixnum_magnitude(radix_val);
    os_char_sink_t sink = make_stream_sink(raw);
    format_integer_write(&sink, integer, radix);
    return nil;
}

lisp_val_t cc_format_object(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_t *raw = os_stream_from_lisp(cc_car(args));
    lisp_val_t obj = cc_car(cc_cdr(args));
    lisp_val_t escape_p = cc_car(cc_cdr(cc_cdr(args)));
    os_char_sink_t sink = make_stream_sink(raw);
    os_print_to_sink(obj, &sink, escape_p != nil);
    return nil;
}

lisp_val_t cc_format_tab(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_t *raw = os_stream_from_lisp(cc_car(args));
    UINT32 target = (UINT32)os_fixnum_magnitude(cc_car(cc_cdr(args)));
    UINT32 current = raw->column;
    UINT32 spaces = (target > current) ? (target - current) : 1;
    for (UINT32 i = 0; i < spaces; i++) {
        os_stream_write_char(raw, ' ');
    }
    return nil;
}

lisp_val_t cc_format(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_t *raw = os_stream_from_lisp(cc_car(args));
    lisp_val_t fmt_str = cc_car(cc_cdr(args));
    lisp_val_t objs = cc_cdr(cc_cdr(args));

    os_char_sink_t sink = make_stream_sink(raw);

    const UINT8 *data;
    UINT32 len;
    format_string_bytes(fmt_str, &data, &len);

    UINT32 i = 0;
    while (i < len) {
        char c = (char)data[i];
        if (c != '~') {
            sink.write_char(sink.ctx, (UINT8)c);
            i++;
            continue;
        }
        i++; // '~'をスキップ
        if (i >= len) {
            break;
        }
        UINT32 num = 0;
        int has_num = 0;
        while (i < len && data[i] >= '0' && data[i] <= '9') {
            has_num = 1;
            num = num * 10 + (UINT32)(data[i] - '0');
            i++;
        }
        if (i >= len) {
            break;
        }
        char directive = (char)data[i];
        i++;

        switch (directive) {
            case 'A': case 'a': {
                lisp_val_t obj = cc_car(objs);
                objs = cc_cdr(objs);
                os_print_to_sink(obj, &sink, 0);
                break;
            }
            case 'S': case 's': {
                lisp_val_t obj = cc_car(objs);
                objs = cc_cdr(objs);
                os_print_to_sink(obj, &sink, 1);
                break;
            }
            case 'B': case 'b': {
                lisp_val_t obj = cc_car(objs);
                objs = cc_cdr(objs);
                format_integer_write(&sink, obj, 2);
                break;
            }
            case 'O': case 'o': {
                lisp_val_t obj = cc_car(objs);
                objs = cc_cdr(objs);
                format_integer_write(&sink, obj, 8);
                break;
            }
            case 'D': case 'd': {
                lisp_val_t obj = cc_car(objs);
                objs = cc_cdr(objs);
                format_integer_write(&sink, obj, 10);
                break;
            }
            case 'X': case 'x': {
                lisp_val_t obj = cc_car(objs);
                objs = cc_cdr(objs);
                format_integer_write(&sink, obj, 16);
                break;
            }
            case 'R': case 'r': {
                lisp_val_t obj = cc_car(objs);
                objs = cc_cdr(objs);
                format_integer_write(&sink, obj, has_num ? (int)num : 10);
                break;
            }
            case 'C': case 'c': {
                lisp_val_t obj = cc_car(objs);
                objs = cc_cdr(objs);
                sink.write_char(sink.ctx, (UINT8)(obj >> 3));
                break;
            }
            case 'G': case 'g': {
                lisp_val_t obj = cc_car(objs);
                objs = cc_cdr(objs);
                format_float_write(&sink, obj);
                break;
            }
            case 'T': case 't': {
                UINT32 target = has_num ? num : 0;
                UINT32 current = raw->column;
                UINT32 spaces = (target > current) ? (target - current) : 1;
                for (UINT32 s = 0; s < spaces; s++) {
                    os_stream_write_char(raw, ' ');
                }
                break;
            }
            case '%':
                os_stream_write_char(raw, '\n');
                break;
            case '&':
                if (raw->column != 0) {
                    os_stream_write_char(raw, '\n');
                }
                break;
            case '~':
                sink.write_char(sink.ctx, '~');
                break;
            default:
                // 仕様に無い未知の指示子は無視する(既存コードの簡略化方針に合わせる)
                break;
        }
    }
    return nil;
}

void os_register_format(void) {
    os_set_function(os_make_symbol("FORMAT"), os_make_native_function((lisp_addr_t)(void *)cc_format), global_environment);
    os_set_function(os_make_symbol("FORMAT-CHAR"), os_make_native_function((lisp_addr_t)(void *)cc_format_char), global_environment);
    os_set_function(os_make_symbol("FORMAT-FLOAT"), os_make_native_function((lisp_addr_t)(void *)cc_format_float), global_environment);
    os_set_function(os_make_symbol("FORMAT-FRESH-LINE"), os_make_native_function((lisp_addr_t)(void *)cc_format_fresh_line), global_environment);
    os_set_function(os_make_symbol("FORMAT-INTEGER"), os_make_native_function((lisp_addr_t)(void *)cc_format_integer), global_environment);
    os_set_function(os_make_symbol("FORMAT-OBJECT"), os_make_native_function((lisp_addr_t)(void *)cc_format_object), global_environment);
    os_set_function(os_make_symbol("FORMAT-TAB"), os_make_native_function((lisp_addr_t)(void *)cc_format_tab), global_environment);
}
