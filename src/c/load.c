#include "load.h"
#include "runtime.h"
#include "lisp.h"
#include "stream.h"
#include "reader.h"
#include "eval.h"
#include "process.h"

/** LOADの引数として渡せるパスの最大長(NUL終端込み) */
#define LOAD_PATH_MAX 256
/** stream open失敗時などのエラーメッセージ用バッファサイズ */
#define LOAD_ERR_MSG_MAX 128

/** msgを現在実行中プロセスのstdout_bufferへ改行付きで表示する */
static void print_load_error(const char *msg) {
    process_t *proc = get_current_process();
    proc->stdout_buffer->write_string(proc->stdout_buffer, msg);
    proc->stdout_buffer->write_char(proc->stdout_buffer, '\n');
}

lisp_val_t cc_load(lisp_val_t args, lisp_val_t env) {
    char path[LOAD_PATH_MAX];
    os_string_to_cstr(cc_car(args), path, sizeof(path));

    char err_msg[LOAD_ERR_MSG_MAX];
    os_stream_t stream;
    if (!os_stream_open_9p_file(&stream, path, err_msg, sizeof(err_msg))) {
        print_load_error(err_msg);
        return g_sym_eval_error;
    }

    for (;;) {
        lisp_val_t form = os_read_stream(&stream);

        if (form == g_sym_read_error) {
            print_load_error("load: syntax error");
            os_stream_close(&stream);
            return g_sym_eval_error;
        }

        if (form == nil) {
            if (stream.error) {
                print_load_error("load: I/O error");
                os_stream_close(&stream);
                return g_sym_eval_error;
            }
            break;
        }

        os_eval(form, env);
    }

    os_stream_close(&stream);
    return g_sym_t;
}

void os_register_load(void) {
    os_set_function(os_make_symbol("LOAD"), os_make_native_function((lisp_addr_t)(void *)cc_load), global_environment);
}
