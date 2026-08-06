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

#define HEAP_SIZE (1024 * 1024)

static void setup_heap() {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
}

static void push_string(process_t *proc, const char *s) {
    while (*s) {
        process_stdin_push(proc, (UINT8)*s);
        s++;
    }
}

// reader.c が参照する os_wait_for_more_input のダミー実装。
// テストでは実際のキー割り込みが発生しないため、あらかじめ queue_next_line で
// 積んでおいた「次の行」をこの場で注入することで、ファイル全体(256byte超)を
// 1行ずつ読ませる(reader_test.cと同じ仕組み)
#define NEXT_LINES_MAX 64
static const char *g_next_lines[NEXT_LINES_MAX];
static UINT32 g_next_line_count = 0;
static UINT32 g_next_line_index = 0;

static void queue_next_line(const char *line) {
    g_next_lines[g_next_line_count++] = line;
}

static void clear_next_lines() {
    g_next_line_count = 0;
    g_next_line_index = 0;
}

void os_wait_for_more_input(process_t *proc) {
    if (g_next_line_index < g_next_line_count) {
        push_string(proc, g_next_lines[g_next_line_index]);
        g_next_line_index++;
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

#define SCRIPT_BUF_SIZE 4096
static char g_script_buf[SCRIPT_BUF_SIZE];

// path の内容を改行で分割し、空行を除いて1行目はpush_string、残りはqueue_next_lineに積む。
// 空行を注入するとos_wait_for_more_inputが何も追加できず、os_readが誤って
// 「入力なし」と判定してしまうため空行はスキップする
static void queue_script_lines(process_t *proc, char *buf) {
    char *line_start = buf;
    int pushed_first = 0;
    for (char *p = buf; ; p++) {
        if (*p == '\n' || *p == '\0') {
            int end = (*p == '\0');
            *p = '\0';
            if (line_start[0] != '\0') {
                if (!pushed_first) {
                    push_string(proc, line_start);
                    pushed_first = 1;
                } else {
                    queue_next_line(line_start);
                }
            }
            if (end) {
                break;
            }
            line_start = p + 1;
        }
    }
}

// path のLispソースを先頭から順にos_read/os_evalし、envを育てながら実行する。
// 構文エラー(read error)が出た場合はテスト失敗として即座に打ち切る
static void run_lisp_file(process_t *proc, lisp_val_t env, const char *path) {
    clear_next_lines();

    FILE *fp = fopen(path, "r");
    assert(fp != NULL, "スクリプトファイルを開ける");
    if (fp == NULL) {
        return;
    }

    size_t len = fread(g_script_buf, 1, SCRIPT_BUF_SIZE - 1, fp);
    fclose(fp);
    g_script_buf[len] = '\0';

    queue_script_lines(proc, g_script_buf);

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

    lisp_val_t env = os_make_environment(os_make_symbol("SCRIPT-TEST-ENV"), global_environment);

    run_lisp_file(proc, env, "test/lisp/square_test.lisp");

    return g_test_failed ? 1 : 0;
}
