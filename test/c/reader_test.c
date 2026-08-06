#include <stdlib.h>
#include <string.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "lisp.h"
#include "process.h"
#include "reader.h"

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
// 積んでおいた「次の行」をこの場で注入することで、複数行にわたる入力を再現する。
// 積んでおいた行が無ければ何もしない(ready==0のままなので、未終端系のテストは従来通りエラーになる)。
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

void test_os_read_empty() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();

    lisp_val_t v = os_read(proc);
    assert(v == nil, "空入力ではnilが返る");
}

void test_os_read_whitespace_only() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "   \n");

    lisp_val_t v = os_read(proc);
    assert(v == nil, "空白のみの入力ではnilが返る");
    assert(proc->stdin_len == 0, "読み終えるとバッファがクリアされる");
    assert(proc->read_pos == 0, "読み終えると読取カーソルもクリアされる");
}

void test_os_read_fixnum() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "42");

    lisp_val_t v = os_read(proc);
    assert(v == os_make_fixnum(42), "\"42\"はfixnum 42として読める");
}

void test_os_read_symbol() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "foo");

    lisp_val_t v = os_read(proc);
    assert(v == os_make_symbol("foo"), "\"foo\"はsymbol fooとして読める");
}

void test_os_read_string() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "\"hello\"");

    lisp_val_t v = os_read(proc);
    assert((v & TAG_MASK) == TAG_STRING, "文字列リテラルはTAG_STRINGを持つ");

    lisp_addr_t addr = v & ~TAG_MASK;
    UINT64 *header = (UINT64 *)addr;
    const char *bytes = (const char *)(addr + 8);
    assert(header[0] == 5, "\"hello\"の文字列長は5");
    assert(strncmp(bytes, "hello", 5) == 0, "文字列の内容がhelloと一致する");
}

void test_os_read_empty_list() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "()");

    lisp_val_t v = os_read(proc);
    assert(v == nil, "\"()\"はnilとして読める");
}

void test_os_read_list() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "(1 2 3)");

    lisp_val_t v = os_read(proc);
    assert(cc_car(v) == os_make_fixnum(1), "リストの1番目は1");
    assert(cc_car(cc_cdr(v)) == os_make_fixnum(2), "リストの2番目は2");
    assert(cc_car(cc_cdr(cc_cdr(v))) == os_make_fixnum(3), "リストの3番目は3");
    assert(cc_cdr(cc_cdr(cc_cdr(v))) == nil, "リストの終端はnil");
}

void test_os_read_quote() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "'foo");

    lisp_val_t v = os_read(proc);
    assert(cc_car(v) == g_sym_quote, "'fooのcarはquote");
    assert(cc_car(cc_cdr(v)) == os_make_symbol("foo"), "'fooのcadrはsymbol foo");
    assert(cc_cdr(cc_cdr(v)) == nil, "'fooのcddrはnil");
}

void test_os_read_multiple_expr_per_line() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "(+ 1 2) (- 3 4)");

    lisp_val_t v1 = os_read(proc);
    assert(cc_car(v1) == os_make_symbol("+"), "1つ目のS式のcarはシンボル+");
    assert(cc_car(cc_cdr(v1)) == os_make_fixnum(1), "1つ目のS式の2番目は1");

    lisp_val_t v2 = os_read(proc);
    assert(cc_car(v2) == os_make_symbol("-"), "2つ目のS式のcarはシンボル-");
    assert(cc_car(cc_cdr(v2)) == os_make_fixnum(3), "2つ目のS式の2番目は3");

    lisp_val_t v3 = os_read(proc);
    assert(v3 == nil, "1行分読み切った後はnilが返る");
}

void test_os_read_stray_close_paren() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, ")");

    lisp_val_t v = os_read(proc);
    assert(v == g_sym_read_error, "先頭の余分な')'はread errorになる");
}

void test_os_read_unterminated_string() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "\"abc");

    lisp_val_t v = os_read(proc);
    assert(v == g_sym_read_error, "閉じクォートが無い文字列はread errorになる");
}

void test_os_read_unterminated_list() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "(1 2");

    lisp_val_t v = os_read(proc);
    assert(v == g_sym_read_error, "閉じ括弧が無いリストはread errorになる");
}

void test_os_read_multiline_list() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    clear_next_lines();
    push_string(proc, "(defun foo (x)\n");
    queue_next_line("  (+ x 1))\n");

    lisp_val_t v = os_read(proc);
    assert(cc_car(v) == os_make_symbol("defun"), "1行目のcarはシンボルdefun");
    assert(cc_car(cc_cdr(v)) == os_make_symbol("foo"), "2番目はシンボルfoo");
    assert(cc_car(cc_car(cc_cdr(cc_cdr(v)))) == os_make_symbol("x"), "3番目の引数リストの先頭はシンボルx");

    lisp_val_t body = cc_car(cc_cdr(cc_cdr(cc_cdr(v))));
    assert(cc_car(body) == os_make_symbol("+"), "2行目まで読んだ本体のcarはシンボル+");
    assert(cc_car(cc_cdr(body)) == os_make_symbol("x"), "本体の2番目はシンボルx");
    assert(cc_car(cc_cdr(cc_cdr(body))) == os_make_fixnum(1), "本体の3番目は1");

    assert(cc_cdr(cc_cdr(cc_cdr(cc_cdr(v)))) == nil, "全体の末尾はnil");
    assert(g_next_line_index == 1, "1回だけ次の行が注入される");
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    setup_heap();
    setup_buffers();

    test_os_read_empty();
    test_os_read_whitespace_only();
    test_os_read_fixnum();
    test_os_read_symbol();
    test_os_read_string();
    test_os_read_empty_list();
    test_os_read_list();
    test_os_read_quote();
    test_os_read_multiple_expr_per_line();
    test_os_read_stray_close_paren();
    test_os_read_unterminated_string();
    test_os_read_unterminated_list();
    test_os_read_multiline_list();

    return g_test_failed ? 1 : 0;
}
