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

lisp_val_t os_make_stream(os_stream_t *raw) {
    return os_make_instance(MAGIC_STREAM, (lisp_addr_t)(void *)raw, 0, 0);
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

void os_register_streams(void) {
    os_set_function(os_make_symbol("OPEN-INPUT-STREAM"), os_make_native_function((lisp_addr_t)(void *)cc_open_input_stream), global_environment);
    os_set_function(os_make_symbol("OPEN-OUTPUT-STREAM"), os_make_native_function((lisp_addr_t)(void *)cc_open_output_stream), global_environment);
    os_set_function(os_make_symbol("CLOSE"), os_make_native_function((lisp_addr_t)(void *)cc_close), global_environment);
    os_set_function(os_make_symbol("READ-CHAR"), os_make_native_function((lisp_addr_t)(void *)cc_read_char), global_environment);
    os_set_function(os_make_symbol("WRITE-CHAR"), os_make_native_function((lisp_addr_t)(void *)cc_write_char), global_environment);
    os_set_function(os_make_symbol("READ"), os_make_native_function((lisp_addr_t)(void *)cc_read), global_environment);
}
