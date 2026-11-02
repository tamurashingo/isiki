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

// process.c(spawn)が参照するinterrupt.cのget_fpu_default_stateのダミー実装。
// FXSAVE領域の初期値はこのテストの対象外なので、ゼロ埋めの512byteバッファを返すだけにする
static UINT8 g_fake_fpu_default_state[512] __attribute__((aligned(16)));

const void *get_fpu_default_state(void) {
    return g_fake_fpu_default_state;
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

    assert(strcmp(captured(), "F1> ") == 0, "空行では入力待ちのプロンプト'環境名> 'が1回だけ表示される");
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

    assert(strcmp(captured(), "F1> \"a\nb\nc\"\n") == 0,
        "複数行にわたる文字列の入力では継続行に'環境名> 'が表示されない");
}

// ---------------------------------------------------------------------------
// [P2] トップレベルの打ち切り(%abort-top-level相当)の扱い
//
// init.lispをロードしないテストなので、errorやwith-environmentは使えない。
// 代わりに、%abort-top-levelが実際に出すのと同じ脱出
// ((return-from %top-level ...) = MAGIC_BLOCK_EXIT 宛先%TOP-LEVEL)を
// フォームとして直接書いて、C側の配線だけを検証する。
// 実際のconditionを使ったend-to-endの確認はQEMU側
// (test/lisp/toplevel_abort_test.lisp)で行う。
// ---------------------------------------------------------------------------

/** [P2] %REPORT-CONDITION-STRINGのフェイク。init.lispの代わりに、
    「呼ばれたら固定の文字列を返す」だけの実装をglobal_environmentへ登録する。
    repl.cがこの関数を実際に呼んで、その戻り値を表示に使っているかを見る */
static int g_fake_report_calls = 0;

static lisp_val_t fake_report_condition_string(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    g_fake_report_calls++;
    return os_make_string("p2 reported message");
}

/** フェイクの%REPORT-CONDITION-STRINGを登録する(登録しない版の挙動も見たいので関数にする) */
static void install_fake_report(void) {
    os_set_function(os_make_symbol("%REPORT-CONDITION-STRING"),
                     os_make_native_function((lisp_addr_t)(void *)fake_report_condition_string),
                     global_environment);
}

/** フェイクを外す(未定義に戻す)。os_set_functionでnilを入れるとos_get_functionがnilを返す */
static void uninstall_fake_report(void) {
    os_set_function(os_make_symbol("%REPORT-CONDITION-STRING"), nil, global_environment);
}

void test_os_repl_step_normal_form_does_not_use_report() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    install_fake_report();
    g_fake_report_calls = 0;
    reset_capture();
    push_string(proc, "(+ 1 2)\n");

    os_repl_step(proc);

    assert(strcmp(captured(), "3\n") == 0, "正常終了したフォームは従来どおりos_printで表示される");
    assert(g_fake_report_calls == 0, "正常終了したフォームではreport-conditionを呼ばない");
    uninstall_fake_report();
}

void test_os_repl_step_aborted_form_uses_report() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    install_fake_report();
    g_fake_report_calls = 0;
    reset_capture();
    // %abort-top-levelが出すのと同じ脱出。42が「abortに渡されたcondition」に当たる
    push_string(proc, "(return-from %top-level 42)\n");

    os_repl_step(proc);

    assert(g_fake_report_calls == 1, "打ち切られたフォームでは%REPORT-CONDITION-STRINGを1回呼ぶ");
    assert(strcmp(captured(), "p2 reported message\n") == 0,
        "打ち切りの表示はreport-conditionの文字列になる(生の値の印字ではない)");
    uninstall_fake_report();
}

void test_os_repl_step_aborted_form_falls_back_when_report_missing() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    uninstall_fake_report();
    reset_capture();
    push_string(proc, "(return-from %top-level 42)\n");

    os_repl_step(proc);

    assert(strcmp(captured(), "42\n") == 0,
        "%REPORT-CONDITION-STRINGが未定義なら従来どおりの印字へ落とす");
}

void test_os_repl_step_aborted_form_restores_environment() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    uninstall_fake_report();

    // まず1回評価してproc->envを遅延生成させる
    reset_capture();
    push_string(proc, "1\n");
    os_repl_step(proc);
    lisp_val_t original_env = proc->env;
    assert(original_env != 0, "envが遅延生成されている");

    // 打ち切られるフォームの**中で**環境を切り替える。
    // %%set-current-environmentはproc->envを恒久的に書き換えるプリミティブで、
    // switch-environment(init.lisp)の実体でもある
    reset_capture();
    push_string(proc,
        "(progn (%%set-current-environment (%%make-environment (quote P2ENV) (%%global-environment)))"
        " (return-from %top-level 7))\n");
    os_repl_step(proc);

    assert(proc->env == original_env,
        "打ち切られたフォームの中のswitch-environmentは巻き戻る");
}

void test_os_repl_step_normal_form_keeps_environment_switch() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    uninstall_fake_report();

    reset_capture();
    push_string(proc, "1\n");
    os_repl_step(proc);
    lisp_val_t original_env = proc->env;

    // 正常終了するフォームの中での切り替えは**残す**(switch-environmentの仕様)
    reset_capture();
    push_string(proc,
        "(%%set-current-environment (%%make-environment (quote P2ENV2) (%%global-environment)))\n");
    os_repl_step(proc);

    assert(proc->env != original_env,
        "正常終了したフォームのswitch-environmentは次のフォームにも効く");
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
    test_os_repl_step_normal_form_does_not_use_report();
    test_os_repl_step_aborted_form_uses_report();
    test_os_repl_step_aborted_form_falls_back_when_report_missing();
    test_os_repl_step_aborted_form_restores_environment();
    test_os_repl_step_normal_form_keeps_environment_switch();

    return g_test_failed ? 1 : 0;
}
