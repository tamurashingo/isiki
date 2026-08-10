#include <stdlib.h>
#include <string.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "lisp.h"
#include "process.h"
#include "repl.h"

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

// os_print が書き込んだ内容を検証できるよう、write_char/write_string を
// 実画面ではなく静的バッファへキャプチャするダミー実装にする
#define CAPTURE_BUF_SIZE 256
static char g_capture_buf[CAPTURE_BUF_SIZE];
static UINT32 g_capture_len = 0;

static void capture_write_char(struct _frame_buffer *self, UINT8 c) {
    (void)self;
    if (g_capture_len < CAPTURE_BUF_SIZE - 1) {
        g_capture_buf[g_capture_len++] = (char)c;
    }
}

static void capture_write_string(struct _frame_buffer *self, const char *s) {
    while (*s) {
        capture_write_char(self, (UINT8)*s);
        s++;
    }
}

static frame_buffer g_frame_buffer = {
    .write_char = capture_write_char,
    .write_string = capture_write_string,
};

frame_buffer* get_active_frame_buffer(void) {
    return &g_frame_buffer;
}

static void reset_capture() {
    g_capture_len = 0;
}

static const char *captured() {
    g_capture_buf[g_capture_len] = '\0';
    return g_capture_buf;
}

// process.c が参照する switch_active_frame_buffer のダミー実装。
// このテストでは process の切替えは行わないため、何もしない
void switch_active_frame_buffer(UINT32 index) {
    (void)index;
}

// process.c(process_scheduler_start)が参照するinterrupt.cの関数のダミー実装。
// ハードウェア割り込みに依存する部分はこのテストの対象外なので、
// リンクを通すためだけに置く
void enable_timer_irq(void) {
}

// os_print/reader.c のプロンプトは proc->stdout_buffer 経由で書かれるようになったため、
// 各プロセスのバッファも capture_write_char/capture_write_string を使うようにし、
// 従来通り captured() で検証できるようにする
static frame_buffer g_buffers[PROCESS_COUNT];

static void setup_buffers() {
    for (UINT32 i = 0; i < PROCESS_COUNT; i++) {
        g_buffers[i].write_char = capture_write_char;
        g_buffers[i].write_string = capture_write_string;
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
// あらかじめ queue_next_line で積んでおいた「次の行」をこの場で注入することで、
// 複数行にわたる入力(文字列やリストの途中改行)を再現する
#define NEXT_LINES_MAX 4
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

void test_os_repl_step_evaluates_and_prints() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    reset_capture();
    push_string(proc, "(+ 1 2)\n");

    os_repl_step(proc);

    assert(strcmp(captured(), "3\n") == 0, "(+ 1 2)を読み評価した結果3が表示され、改行される");
}

void test_os_repl_step_empty_line_shows_prompt() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    reset_capture();
    push_string(proc, "\n");

    os_repl_step(proc);

    assert(strcmp(captured(), "> ") == 0, "空行では入力待ちのプロンプト'> 'が1回だけ表示される");
}

void test_os_repl_step_lazily_initializes_env() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    assert(proc->env == 0, "初期状態ではenvは未初期化(0)");

    reset_capture();
    push_string(proc, "1\n");
    os_repl_step(proc);

    assert(proc->env != 0, "os_repl_stepを1回呼ぶとenvが遅延生成される");
}

void test_os_repl_step_reuses_env_across_calls() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    reset_capture();
    push_string(proc, "1\n");
    os_repl_step(proc);

    os_set_variable(os_make_symbol("x"), os_make_fixnum(10), proc->env);

    reset_capture();
    push_string(proc, "x\n");
    os_repl_step(proc);

    assert(strcmp(captured(), "10\n") == 0, "生成されたenvはプロセスをまたいで保持される");
}

void test_os_repl_step_multiline_string_shows_prompt_once() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    clear_next_lines();
    queue_next_line("b\n");
    queue_next_line("c\"\n");
    reset_capture();
    push_string(proc, "\"a\n");

    os_repl_step(proc);

    assert(strcmp(captured(), "> \"a\nb\nc\"\n") == 0,
        "複数行にわたる文字列の入力では継続行に'> 'が表示されない");
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    setup_heap();
    setup_buffers();

    test_os_repl_step_evaluates_and_prints();
    test_os_repl_step_empty_line_shows_prompt();
    test_os_repl_step_lazily_initializes_env();
    test_os_repl_step_reuses_env_across_calls();
    test_os_repl_step_multiline_string_shows_prompt_once();

    return g_test_failed ? 1 : 0;
}
