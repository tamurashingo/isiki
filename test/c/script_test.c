#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "lisp.h"
#include "process.h"
#include "reader.h"
#include "eval.h"
#include "stream_lisp.h"
#include "format.h"

// reader.c は os_read_stream 経由でstream.cをリンクするため、stream.cが
// 参照するos_virtio9p_open/read_chunk/closeが未定義シンボルにならないよう
// ダミー実装を置く(このテストはos_read_streamを呼ばないため中身は使われない)
int os_virtio9p_open(const char *path, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path;
    (void)mode;
    (void)out_fid;
    (void)err_msg;
    (void)err_msg_cap;
    return 0;
}

int os_virtio9p_create(const char *path, UINT32 perm, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path;
    (void)perm;
    (void)mode;
    (void)out_fid;
    (void)err_msg;
    (void)err_msg_cap;
    return 0;
}

int os_virtio9p_write_chunk(UINT32 fid, UINT64 offset, const UINT8 *data, UINT32 count,
                             UINT32 *out_written, char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)offset;
    (void)data;
    (void)count;
    (void)out_written;
    (void)err_msg;
    (void)err_msg_cap;
    return 0;
}

int os_virtio9p_read_chunk(UINT32 fid, UINT64 offset, UINT32 want,
                            const UINT8 **out_data, UINT32 *out_count,
                            char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)offset;
    (void)want;
    (void)out_data;
    (void)out_count;
    (void)err_msg;
    (void)err_msg_cap;
    return 0;
}

int os_virtio9p_close(UINT32 fid, char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)err_msg;
    (void)err_msg_cap;
    return 0;
}

// runtime.c が参照する get_active_frame_buffer のダミー実装。
// テスト環境では実画面がないため、write_string は何もしない
static void dummy_write_string(struct _frame_buffer *self, const char *s) {
    (void)self;
    (void)s;
}

static frame_buffer g_frame_buffer = {
    .write_string = dummy_write_string,
};

frame_buffer* get_active_frame_buffer(void) {
    return &g_frame_buffer;
}

// process.c が参照する switch_active_frame_buffer のダミー実装。
// このテストでは process の切替えは行わないため、何もしない
void switch_active_frame_buffer(UINT32 index) {
    (void)index;
}

// process.c(process_scheduler_start/process_trampoline_c)が参照する
// interrupt.c/repl.cの関数のダミー実装。ハードウェア割り込みやREPLの実行に
// 依存する部分はこのテストの対象外なので、リンクを通すためだけに置く
void enable_timer_irq(void) {
}

void os_repl_step(process_t *proc) {
    (void)proc;
}

// reader.c の ensure_data が proc->stdout_buffer 経由でプロンプトを書き込むための
// 各プロセス用ダミーバッファ。内容の検証はしないため何もしない実装で良い
static void dummy_buf_write_char(struct _frame_buffer *self, UINT8 c) {
    (void)self;
    (void)c;
}

static void dummy_buf_write_string(struct _frame_buffer *self, const char *s) {
    (void)self;
    (void)s;
}

static frame_buffer g_buffers[PROCESS_COUNT];

static void setup_buffers() {
    for (UINT32 i = 0; i < PROCESS_COUNT; i++) {
        g_buffers[i].write_char = dummy_buf_write_char;
        g_buffers[i].write_string = dummy_buf_write_string;
    }
}

#define HEAP_SIZE (1024 * 1024 * 64)

static void setup_heap() {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "32MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
    os_register_eval_primitives();
}

static void push_string(process_t *proc, const char *s) {
    while (*s) {
        process_stdin_push(proc, (UINT8)*s);
        s++;
    }
}

// reader.c が参照する os_wait_for_more_input のダミー実装。
// テストでは実際のキー割り込みが発生しないため、run_lisp_file が開いた
// ファイルから「呼ばれた時点で」次の1行だけを読んで注入する。ファイル全体を
// 先読みしないので、ファイルサイズ・行数に上限がない(reader_test.cは
// 短い文字列を手書きで積むだけなので、この仕組みの対象外)。
#define SCRIPT_LINE_MAX 256
static FILE *g_script_fp = NULL;

// g_script_fp から空行を読み飛ばして次の1行を buf に格納する。
// 空行を注入するとos_wait_for_more_inputが何も追加できず、os_readが誤って
// 「入力なし」と判定してしまうため空行はスキップする。
// 各行には末尾の'\n'を必ず付け直す: reader.c の';'コメント読み飛ばしは
// 行末を'\n'で判定するため、'\n'を落とすとコメント行が次の行を
// 読み込むまで終端せず、後続の行がコメントとして無言で読み飛ばされてしまう。
static int fetch_next_nonblank_line(char *buf, size_t bufsize) {
    while (g_script_fp != NULL && fgets(buf, (int)bufsize, g_script_fp) != NULL) {
        size_t len = strlen(buf);
        if (len == 0 || buf[len - 1] != '\n') {
            if (len < bufsize - 1) {
                buf[len] = '\n';
                buf[len + 1] = '\0';
                len++;
            }
        }
        if (len > 1 || (len == 1 && buf[0] != '\n')) {
            return 1;
        }
        // 空行だったので読み直す
    }
    return 0;
}

void os_wait_for_more_input(process_t *proc) {
    char line[SCRIPT_LINE_MAX];
    if (fetch_next_nonblank_line(line, sizeof(line))) {
        push_string(proc, line);
    } else if (g_script_fp != NULL) {
        fclose(g_script_fp);
        g_script_fp = NULL;
    }
}

// assert-equal(expected actual): Lispテストコードから呼べるnative function。
// 評価済みの2引数をタグ付き値のまま比較し、test_assert.hのassert()マクロに
// 結果を渡して既存のC側テストと同じOK/NG出力・g_test_failed集計に乗せる
static lisp_val_t primitive_assert_equal(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t expected = cc_car(args);
    lisp_val_t actual = cc_car(cc_cdr(args));
    char msg[128];
    snprintf(msg, sizeof(msg), "assert-equal: expected=%lld actual=%lld",
             (long long)(expected >> 3), (long long)(actual >> 3));
    assert(expected == actual, msg);
    return actual;
}

// path のLispソースを先頭から順にos_read/os_evalし、envを育てながら実行する。
// ファイルは一度に読み込まず、os_wait_for_more_inputから1行ずつ消費される。
// 構文エラー(read error)が出た場合はテスト失敗として即座に打ち切る
static void run_lisp_file(process_t *proc, lisp_val_t env, const char *path) {
    g_script_fp = fopen(path, "r");
    assert(g_script_fp != NULL, "スクリプトファイルを開ける");
    if (g_script_fp == NULL) {
        return;
    }

    char first_line[SCRIPT_LINE_MAX];
    if (!fetch_next_nonblank_line(first_line, sizeof(first_line))) {
        fclose(g_script_fp);
        g_script_fp = NULL;
        return; // 空ファイル: 何も評価しない
    }
    push_string(proc, first_line);

    for (;;) {
        lisp_val_t form = os_read(proc);
        if (form == nil) {
            break;
        }
        if (form == g_sym_read_error) {
            assert(0, "スクリプトにread errorが無いこと");
            break;
        }
        os_eval(form, env);
    }

    if (g_script_fp != NULL) {
        fclose(g_script_fp);
        g_script_fp = NULL;
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    setup_heap();
    setup_buffers();
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();

    os_set_function(os_make_symbol("assert-equal"),
                     os_make_native_function((lisp_addr_t)(void *)primitive_assert_equal),
                     global_environment);
    os_register_streams();
    os_register_format();

    lisp_val_t env = os_make_environment(os_make_symbol("SCRIPT-TEST-ENV"), global_environment);

    run_lisp_file(proc, env, "test/lisp/square_test.lisp");
    run_lisp_file(proc, env, "test/lisp/rest_test.lisp");
    run_lisp_file(proc, env, "test/lisp/defmacro_test.lisp");

    lisp_val_t init_env = os_make_environment(os_make_symbol("INIT-TEST-ENV"), global_environment);

    run_lisp_file(proc, init_env, "src/lisp/init.lisp");
    run_lisp_file(proc, init_env, "test/lisp/init_test.lisp");

    return g_test_failed ? 1 : 0;
}
