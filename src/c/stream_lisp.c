#include "stream_lisp.h"
#include "runtime.h"
#include "lisp.h"
#include "process.h"
#include "reader.h"

/** LOADのLOAD_PATH_MAXと同じ規約: OPEN-INPUT-STREAMに渡せるパスの最大長(NUL終端込み) */
#define STREAM_PATH_MAX 256

/** stream(TAG_INSTANCE, MAGIC_STREAM)のword1に埋め込んだ生ポインタを取り出す */
static os_stream_t *stream_raw(lisp_val_t stream) {
    lisp_addr_t addr = stream & ~TAG_MASK;
    UINT64 *obj = (UINT64 *)addr;
    return (os_stream_t *)(lisp_addr_t)obj[1];
}

/** kindが入力可能(READ-CHAR等が使える)なストリーム種別かどうかを判定する */
static int stream_kind_is_input(stream_kind_t kind) {
    return kind == STREAM_9P_FILE_READ || kind == STREAM_9P_FILE_IO || kind == STREAM_STRING_INPUT;
}

/** kindが出力可能(WRITE-CHAR等が使える)なストリーム種別かどうかを判定する */
static int stream_kind_is_output(stream_kind_t kind) {
    return kind == STREAM_9P_FILE_WRITE || kind == STREAM_9P_FILE_IO
        || kind == STREAM_OUTPUT_SCREEN || kind == STREAM_STRING_OUTPUT;
}

/** STRINGオブジェクトのレイアウト([len(8byte)][chars...])からデータ先頭とバイト数を取り出す */
static void string_bytes(lisp_val_t str, const UINT8 **out_data, UINT32 *out_len) {
    lisp_addr_t addr = str & ~TAG_MASK;
    UINT64 len = ((UINT64 *)addr)[0];
    *out_data = (const UINT8 *)(addr + 8);
    *out_len = (UINT32)len;
}

/** data(len バイト)をコピーしてSTRINGオブジェクトを作る。os_make_string(NUL終端cstr版)の生バイト版 */
static lisp_val_t make_string_from_bytes(const UINT8 *data, UINT32 len) {
    lisp_addr_t addr = os_alloc_raw(8 + len);
    lisp_val_t *header = (lisp_val_t *)addr;
    header[0] = len;
    UINT8 *bytes = (UINT8 *)(addr + 8);
    for (UINT32 i = 0; i < len; i++) {
        bytes[i] = data[i];
    }
    return (lisp_val_t)(addr | TAG_STRING);
}

lisp_val_t os_make_stream(os_stream_t *raw) {
    return os_make_instance(MAGIC_STREAM, (lisp_addr_t)(void *)raw, 0, 0);
}

os_stream_t *os_stream_from_lisp(lisp_val_t stream) {
    return stream_raw(stream);
}

lisp_val_t cc_open_input_stream(lisp_val_t args, lisp_val_t env) {
    char path[STREAM_PATH_MAX];
    os_string_to_cstr(cc_car(args), path, sizeof(path));

    os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
    char err_msg[128];
    if (!os_stream_open_9p_file(raw, path, err_msg, sizeof(err_msg))) {
        return g_sym_eval_error;
    }
    return os_make_stream(raw);
}

lisp_val_t cc_open_output_stream(lisp_val_t args, lisp_val_t env) {
    os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
    os_stream_open_screen_output(raw, get_current_process()->stdout_buffer);
    return os_make_stream(raw);
}

lisp_val_t cc_close(lisp_val_t args, lisp_val_t env) {
    os_stream_close(stream_raw(cc_car(args)));
    return nil;
}

lisp_val_t cc_read_char(lisp_val_t args, lisp_val_t env) {
    os_stream_t *raw = stream_raw(cc_car(args));
    char ch;
    if (!os_stream_read_char(raw, &ch)) {
        return nil;
    }
    return os_make_char(ch);
}

lisp_val_t cc_write_char(lisp_val_t args, lisp_val_t env) {
    lisp_val_t ch = cc_car(args);
    os_stream_t *raw = stream_raw(cc_car(cc_cdr(args)));
    os_stream_write_char(raw, (char)(ch >> 3));
    return ch;
}

lisp_val_t cc_read(lisp_val_t args, lisp_val_t env) {
    os_stream_t *raw = stream_raw(cc_car(args));
    return os_read_stream(raw);
}

lisp_val_t cc_open_stream_p(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) != TAG_INSTANCE) {
        return nil;
    }
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    if (obj[0] != MAGIC_STREAM) {
        return nil;
    }
    return stream_raw(val)->closed ? nil : g_sym_t;
}

lisp_val_t cc_input_stream_p(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) != TAG_INSTANCE) {
        return nil;
    }
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    if (obj[0] != MAGIC_STREAM) {
        return nil;
    }
    return stream_kind_is_input(stream_raw(val)->kind) ? g_sym_t : nil;
}

lisp_val_t cc_output_stream_p(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) != TAG_INSTANCE) {
        return nil;
    }
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    if (obj[0] != MAGIC_STREAM) {
        return nil;
    }
    return stream_kind_is_output(stream_raw(val)->kind) ? g_sym_t : nil;
}

lisp_val_t cc_open_output_file(lisp_val_t args, lisp_val_t env) {
    (void)env;
    char path[STREAM_PATH_MAX];
    os_string_to_cstr(cc_car(args), path, sizeof(path));

    os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
    char err_msg[128];
    if (!os_stream_open_9p_file_write(raw, path, 1 /* create_if_missing */, err_msg, sizeof(err_msg))) {
        return g_sym_eval_error;
    }
    return os_make_stream(raw);
}

lisp_val_t cc_open_io_file(lisp_val_t args, lisp_val_t env) {
    (void)env;
    char path[STREAM_PATH_MAX];
    os_string_to_cstr(cc_car(args), path, sizeof(path));

    os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
    char err_msg[128];
    if (!os_stream_open_9p_file_io(raw, path, 1 /* create_if_missing */, err_msg, sizeof(err_msg))) {
        return g_sym_eval_error;
    }
    return os_make_stream(raw);
}

lisp_val_t cc_finish_output(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_finish_output(stream_raw(cc_car(args)));
    return nil;
}

lisp_val_t cc_create_string_input_stream(lisp_val_t args, lisp_val_t env) {
    (void)env;
    const UINT8 *data;
    UINT32 len;
    string_bytes(cc_car(args), &data, &len);

    os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
    os_stream_open_string_input(raw, (const char *)data, len);
    return os_make_stream(raw);
}

lisp_val_t cc_create_string_output_stream(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
    os_stream_open_string_output(raw);
    return os_make_stream(raw);
}

lisp_val_t cc_get_output_stream_string(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_t *raw = stream_raw(cc_car(args));
    lisp_val_t result = make_string_from_bytes(raw->str_buf, raw->str_len);
    raw->str_len = 0; // 呼び出すたびに「前回呼び出し以降に書き込まれた分」へリセットする(仕様通り)
    return result;
}

lisp_val_t cc_preview_char(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_t *raw = stream_raw(cc_car(args));
    char ch;
    if (!os_stream_preview_char(raw, &ch)) {
        return nil;
    }
    return os_make_char(ch);
}

lisp_val_t cc_read_line(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_t *raw = stream_raw(cc_car(args));

    #define READ_LINE_MAX 512
    char buf[READ_LINE_MAX];
    UINT32 n = 0;
    int got_any = 0;
    char ch;
    while (os_stream_read_char(raw, &ch)) {
        got_any = 1;
        if (ch == '\n') {
            break;
        }
        if (n < READ_LINE_MAX - 1) {
            buf[n++] = ch;
        }
    }
    if (!got_any) {
        return nil;
    }
    return make_string_from_bytes((const UINT8 *)buf, n);
    #undef READ_LINE_MAX
}

lisp_val_t cc_stream_ready_p(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    // 非同期I/Oが無く読み込みは常に同期的にブロックするため、常にtrueを返すスタブとする
    return g_sym_t;
}

lisp_val_t cc_read_byte(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_t *raw = stream_raw(cc_car(args));
    char ch;
    if (!os_stream_read_char(raw, &ch)) {
        return nil;
    }
    return os_make_fixnum((UINT64)(UINT8)ch);
}

lisp_val_t cc_write_byte(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t z = cc_car(args);
    os_stream_t *raw = stream_raw(cc_car(cc_cdr(args)));
    os_stream_write_char(raw, (char)(UINT8)os_fixnum_magnitude(z));
    return z;
}

lisp_val_t cc_probe_file(lisp_val_t args, lisp_val_t env) {
    (void)env;
    char path[STREAM_PATH_MAX];
    os_string_to_cstr(cc_car(args), path, sizeof(path));

    os_stream_t tmp;
    char err_msg[128];
    if (!os_stream_open_9p_file(&tmp, path, err_msg, sizeof(err_msg))) {
        return nil;
    }
    os_stream_close(&tmp);
    return g_sym_t;
}

lisp_val_t cc_file_position(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_t *raw = stream_raw(cc_car(args));
    if (raw->kind == STREAM_STRING_INPUT) {
        return os_make_fixnum(raw->str_pos);
    }
    if (raw->kind == STREAM_STRING_OUTPUT) {
        return os_make_fixnum(raw->str_len);
    }
    return os_make_fixnum(raw->next_offset);
}

lisp_val_t cc_set_file_position(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_t *raw = stream_raw(cc_car(args));
    lisp_val_t z = cc_car(cc_cdr(args));
    UINT64 newpos = os_fixnum_magnitude(z);

    raw->has_lookahead = 0;
    raw->eof = 0;
    if (raw->kind == STREAM_STRING_INPUT) {
        raw->str_pos = (UINT32)newpos;
    } else if (raw->kind == STREAM_STRING_OUTPUT) {
        raw->str_len = (UINT32)newpos;
    } else {
        raw->next_offset = newpos;
        raw->buf_pos = 0;
        raw->buf_count = 0;
    }
    return z;
}

lisp_val_t cc_file_length(lisp_val_t args, lisp_val_t env) {
    (void)env;
    char path[STREAM_PATH_MAX];
    os_string_to_cstr(cc_car(args), path, sizeof(path));

    os_stream_t tmp;
    char err_msg[128];
    if (!os_stream_open_9p_file(&tmp, path, err_msg, sizeof(err_msg))) {
        return g_sym_eval_error;
    }
    UINT64 count = 0;
    char ch;
    while (os_stream_read_char(&tmp, &ch)) {
        count++;
    }
    os_stream_close(&tmp);
    return os_make_fixnum(count);
}

void os_register_streams(void) {
    os_set_function(os_make_symbol("OPEN-INPUT-STREAM"), os_make_native_function((lisp_addr_t)(void *)cc_open_input_stream), global_environment);
    os_set_function(os_make_symbol("OPEN-OUTPUT-STREAM"), os_make_native_function((lisp_addr_t)(void *)cc_open_output_stream), global_environment);
    os_set_function(os_make_symbol("CLOSE"), os_make_native_function((lisp_addr_t)(void *)cc_close), global_environment);
    os_set_function(os_make_symbol("READ-CHAR"), os_make_native_function((lisp_addr_t)(void *)cc_read_char), global_environment);
    os_set_function(os_make_symbol("WRITE-CHAR"), os_make_native_function((lisp_addr_t)(void *)cc_write_char), global_environment);
    os_set_function(os_make_symbol("READ"), os_make_native_function((lisp_addr_t)(void *)cc_read), global_environment);
    os_set_function(os_make_symbol("OPEN-STREAM-P"), os_make_native_function((lisp_addr_t)(void *)cc_open_stream_p), global_environment);
    os_set_function(os_make_symbol("INPUT-STREAM-P"), os_make_native_function((lisp_addr_t)(void *)cc_input_stream_p), global_environment);
    os_set_function(os_make_symbol("OUTPUT-STREAM-P"), os_make_native_function((lisp_addr_t)(void *)cc_output_stream_p), global_environment);
    os_set_function(os_make_symbol("OPEN-OUTPUT-FILE"), os_make_native_function((lisp_addr_t)(void *)cc_open_output_file), global_environment);
    os_set_function(os_make_symbol("OPEN-IO-FILE"), os_make_native_function((lisp_addr_t)(void *)cc_open_io_file), global_environment);
    os_set_function(os_make_symbol("FINISH-OUTPUT"), os_make_native_function((lisp_addr_t)(void *)cc_finish_output), global_environment);
    os_set_function(os_make_symbol("CREATE-STRING-INPUT-STREAM"), os_make_native_function((lisp_addr_t)(void *)cc_create_string_input_stream), global_environment);
    os_set_function(os_make_symbol("CREATE-STRING-OUTPUT-STREAM"), os_make_native_function((lisp_addr_t)(void *)cc_create_string_output_stream), global_environment);
    os_set_function(os_make_symbol("GET-OUTPUT-STREAM-STRING"), os_make_native_function((lisp_addr_t)(void *)cc_get_output_stream_string), global_environment);
    os_set_function(os_make_symbol("PREVIEW-CHAR"), os_make_native_function((lisp_addr_t)(void *)cc_preview_char), global_environment);
    os_set_function(os_make_symbol("READ-LINE"), os_make_native_function((lisp_addr_t)(void *)cc_read_line), global_environment);
    os_set_function(os_make_symbol("STREAM-READY-P"), os_make_native_function((lisp_addr_t)(void *)cc_stream_ready_p), global_environment);
    os_set_function(os_make_symbol("READ-BYTE"), os_make_native_function((lisp_addr_t)(void *)cc_read_byte), global_environment);
    os_set_function(os_make_symbol("WRITE-BYTE"), os_make_native_function((lisp_addr_t)(void *)cc_write_byte), global_environment);
    os_set_function(os_make_symbol("PROBE-FILE"), os_make_native_function((lisp_addr_t)(void *)cc_probe_file), global_environment);
    os_set_function(os_make_symbol("FILE-POSITION"), os_make_native_function((lisp_addr_t)(void *)cc_file_position), global_environment);
    os_set_function(os_make_symbol("SET-FILE-POSITION"), os_make_native_function((lisp_addr_t)(void *)cc_set_file_position), global_environment);
    os_set_function(os_make_symbol("FILE-LENGTH"), os_make_native_function((lisp_addr_t)(void *)cc_file_length), global_environment);
}
