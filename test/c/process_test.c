#include <stdlib.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "lisp.h"
#include "process.h"

// process.c が参照する switch_active_frame_buffer のダミー実装。
// テスト環境では実画面がないため、呼ばれたindexを記録するだけにする
static UINT32 g_last_switched_index = 0xFFFFFFFF;

void switch_active_frame_buffer(UINT32 index) {
    g_last_switched_index = index;
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

// process.c(process_scheduler_start/process_trampoline_c)が参照する
// interrupt.c/repl.cの関数のダミー実装。ハードウェア割り込みやREPLの実行に
// 依存する部分はユニットテストの対象外なので、リンクを通すためだけに置く
void enable_timer_irq(void) {
}

void os_repl_step(process_t *proc) {
    (void)proc;
}

#define HEAP_SIZE (1024 * 1024)

static void setup_heap() {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
}

static frame_buffer g_buffers[PROCESS_COUNT];

void test_initialize_processes() {
    initialize_processes(g_buffers);

    process_t *current = get_current_process();
    assert(current->id == 0, "初期状態ではプロセス0がカレントになる");
    assert(current->stdout_buffer == &g_buffers[0], "プロセス0の標準出力はg_buffers[0]");
    assert(current->stdin_len == 0, "初期状態では標準入力は空");
}

void test_switch_active_process() {
    initialize_processes(g_buffers);

    switch_active_process(2);
    process_t *current = get_current_process();
    assert(current->id == 2, "switch_active_process(2)後はプロセス2がカレントになる");
    assert(current->stdout_buffer == &g_buffers[2], "プロセス2の標準出力はg_buffers[2]");
    assert(g_last_switched_index == 2, "switch_active_frame_bufferにも2が渡される");

    // 範囲外のindexは無視される
    switch_active_process(PROCESS_COUNT);
    assert(get_current_process()->id == 2, "範囲外のindexへの切替えは無視される");

    // 同じindexへの切替えではswitch_active_frame_bufferが呼ばれない(重複呼び出し防止)
    g_last_switched_index = 0xFFFFFFFF;
    switch_active_process(2);
    assert(g_last_switched_index == 0xFFFFFFFF, "同じindexへの切替えではswitch_active_frame_bufferを呼ばない");
}

void test_process_stdin_push() {
    initialize_processes(g_buffers);
    process_t *current = get_current_process();

    process_stdin_push(current, 'a');
    process_stdin_push(current, 'b');
    assert(current->stdin_len == 2, "2文字積んだ後はstdin_len == 2");
    assert(current->stdin_buf[0] == 'a', "1文字目はa");
    assert(current->stdin_buf[1] == 'b', "2文字目はb");

    // バッファ境界を超えないことを確認する
    for (UINT32 i = 0; i < PROCESS_STDIN_BUF_SIZE + 10; i++) {
        process_stdin_push(current, 'x');
    }
    assert(current->stdin_len == PROCESS_STDIN_BUF_SIZE - 1, "stdin_lenはPROCESS_STDIN_BUF_SIZE-1を超えない");
}

void test_process_stdin_push_sets_ready_on_newline() {
    initialize_processes(g_buffers);
    process_t *current = get_current_process();

    process_stdin_push(current, 'a');
    assert(current->ready == 0, "改行以外の文字を積んでもreadyは立たない");

    process_stdin_push(current, '\n');
    assert(current->ready == 1, "改行を積むとreadyが立つ");
}

void test_os_process_saved_rsp_get_set() {
    lisp_val_t pcb = os_make_instance(MAGIC_PROCESS, os_make_fixnum(0), 0, g_sym_process_ready);
    assert(os_process_get_saved_rsp(pcb) == 0, "初期状態のsaved_rspは0");

    os_process_set_saved_rsp(pcb, 0x1234);
    assert(os_process_get_saved_rsp(pcb) == 0x1234, "set_saved_rspで書き込んだ値をget_saved_rspで読み直せる");
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    setup_heap();

    test_initialize_processes();
    test_switch_active_process();
    test_process_stdin_push();
    test_process_stdin_push_sets_ready_on_newline();
    test_os_process_saved_rsp_get_set();

    return g_test_failed ? 1 : 0;
}
