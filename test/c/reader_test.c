#include <stdlib.h>
#include <string.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "lisp.h"
#include "process.h"
#include "reader.h"

// reader.c は os_read_stream 経由でstream.cをリンクするため、stream.cが
// 参照するos_virtio9p_open/read_chunk/closeが未定義シンボルにならないよう
// ダミー実装を置く(このテストはos_read_streamを呼ばないため中身は使われない)
int os_virtio9p_open(const char *path, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path;
    (void)out_fid;
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

void test_os_read_negative_fixnum() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "-42");

    lisp_val_t v = os_read(proc);
    assert(v == os_make_fixnum_signed(1, 42), "\"-42\"は負のfixnum -42として読める");
}

void test_os_read_bare_minus_is_symbol() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "-");

    lisp_val_t v = os_read(proc);
    assert(v == os_make_symbol("-"), "\"-\"単体はsymbolとして読める");
}

void test_os_read_bignum_literal() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    // 2^60 = 1152921504606846976は60bitのFIXNUM_MAGNITUDE_MASKを超えるのでbignumになる
    push_string(proc, "1152921504606846976");

    lisp_val_t v = os_read(proc);
    assert((v & TAG_MASK) == TAG_INSTANCE, "60bitを超える整数リテラルはbignumとして読める");
    UINT64 *obj = (UINT64 *)(v & ~TAG_MASK);
    assert(obj[0] == MAGIC_BIGNUM, "word0はMAGIC_BIGNUM");
    assert(obj[1] == 0, "非負なのでsignは0");
}

void test_os_read_negative_bignum_literal() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "-1152921504606846976");

    lisp_val_t v = os_read(proc);
    assert((v & TAG_MASK) == TAG_INSTANCE, "60bitを超える負の整数リテラルはbignumとして読める");
    UINT64 *obj = (UINT64 *)(v & ~TAG_MASK);
    assert(obj[0] == MAGIC_BIGNUM, "word0はMAGIC_BIGNUM");
    assert(obj[1] == 1, "負なのでsignは1");
}

void test_os_read_small_literal_stays_fixnum() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "123456789012345");

    lisp_val_t v = os_read(proc);
    assert((v & TAG_MASK) == TAG_FIXNUM, "60bit以内の整数リテラルはFIXNUMのまま");
    assert(os_fixnum_magnitude(v) == 123456789012345ULL, "読み取ったマグニチュードが一致する");
}

void test_os_read_positive_fixnum_with_sign() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "+42");

    lisp_val_t v = os_read(proc);
    assert(v == os_make_fixnum(42), "明示的な'+'付きの\"+42\"はfixnum 42として読める");
}

void test_os_read_bare_plus_is_symbol() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "+");

    lisp_val_t v = os_read(proc);
    assert(v == os_make_symbol("+"), "\"+\"単体はsymbolとして読める");
}

void test_os_read_binary_literal() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#b1010");

    lisp_val_t v = os_read(proc);
    assert(v == os_make_fixnum(10), "\"#b1010\"は2進数として10で読める");
}

void test_os_read_binary_literal_uppercase_prefix() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#B1010");

    lisp_val_t v = os_read(proc);
    assert(v == os_make_fixnum(10), "\"#B1010\"(大文字B)も2進数として10で読める");
}

void test_os_read_octal_literal() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#o17");

    lisp_val_t v = os_read(proc);
    assert(v == os_make_fixnum(15), "\"#o17\"は8進数として15で読める");
}

void test_os_read_octal_literal_uppercase_prefix() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#O17");

    lisp_val_t v = os_read(proc);
    assert(v == os_make_fixnum(15), "\"#O17\"(大文字O)も8進数として15で読める");
}

void test_os_read_hex_literal_lowercase_digits() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#xff");

    lisp_val_t v = os_read(proc);
    assert(v == os_make_fixnum(255), "\"#xff\"(小文字桁)は16進数として255で読める");
}

void test_os_read_hex_literal_uppercase_prefix_and_digits() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#XFF");

    lisp_val_t v = os_read(proc);
    assert(v == os_make_fixnum(255), "\"#XFF\"(大文字X・大文字桁)も16進数として255で読める");
}

void test_os_read_radix_literal_negative() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#b-101");

    lisp_val_t v = os_read(proc);
    assert(v == os_make_fixnum_signed(1, 5), "\"#b-101\"は負の2進数として-5で読める");
}

void test_os_read_radix_literal_explicit_positive_sign() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#o+17");

    lisp_val_t v = os_read(proc);
    assert(v == os_make_fixnum(15), "\"#o+17\"は明示的な'+'付きでも8進数として15で読める");
}

void test_os_read_radix_literal_invalid_digit_is_read_error() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#b12");

    lisp_val_t v = os_read(proc);
    assert(v == g_sym_read_error, "2進数の基数で表現できない数字'2'を含む\"#b12\"はread errorになる");
}

void test_os_read_radix_literal_no_digits_is_read_error() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#b ");

    lisp_val_t v = os_read(proc);
    assert(v == g_sym_read_error, "数字が1つも無い\"#b\"はread errorになる");
}

void test_os_read_hex_bignum_literal() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    // 16^15 = 2^60は60bitのFIXNUM_MAGNITUDE_MASKを超えるのでbignumになる
    push_string(proc, "#x1000000000000000");

    lisp_val_t v = os_read(proc);
    assert((v & TAG_MASK) == TAG_INSTANCE, "60bitを超える16進整数リテラルはbignumとして読める");
    UINT64 *obj = (UINT64 *)(v & ~TAG_MASK);
    assert(obj[0] == MAGIC_BIGNUM, "word0はMAGIC_BIGNUM");
    assert(obj[1] == 0, "非負なのでsignは0");
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

void test_os_read_quasiquote() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "`foo");

    lisp_val_t v = os_read(proc);
    assert(cc_car(v) == g_sym_quasiquote, "`fooのcarはquasiquote");
    assert(cc_car(cc_cdr(v)) == os_make_symbol("foo"), "`fooのcadrはsymbol foo");
    assert(cc_cdr(cc_cdr(v)) == nil, "`fooのcddrはnil");
}

void test_os_read_unquote() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, ",foo");

    lisp_val_t v = os_read(proc);
    assert(cc_car(v) == g_sym_unquote, ",fooのcarはunquote");
    assert(cc_car(cc_cdr(v)) == os_make_symbol("foo"), ",fooのcadrはsymbol foo");
    assert(cc_cdr(cc_cdr(v)) == nil, ",fooのcddrはnil");
}

void test_os_read_unquote_splicing() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, ",@foo");

    lisp_val_t v = os_read(proc);
    assert(cc_car(v) == g_sym_unquote_splicing, ",@fooのcarはunquote-splicing");
    assert(cc_car(cc_cdr(v)) == os_make_symbol("foo"), ",@fooのcadrはsymbol foo");
    assert(cc_cdr(cc_cdr(v)) == nil, ",@fooのcddrはnil");
}

void test_os_read_quasiquote_with_unquote_in_list() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "`(a ,b ,@c)");

    lisp_val_t v = os_read(proc);
    assert(cc_car(v) == g_sym_quasiquote, "`(...)のcarはquasiquote");

    lisp_val_t list = cc_car(cc_cdr(v));
    assert(cc_car(list) == os_make_symbol("a"), "リストの1番目はsymbol a");

    lisp_val_t unquote_form = cc_car(cc_cdr(list));
    assert(cc_car(unquote_form) == g_sym_unquote, "リストの2番目は(unquote b)");
    assert(cc_car(cc_cdr(unquote_form)) == os_make_symbol("b"), "(unquote b)のcadrはsymbol b");

    lisp_val_t splicing_form = cc_car(cc_cdr(cc_cdr(list)));
    assert(cc_car(splicing_form) == g_sym_unquote_splicing, "リストの3番目は(unquote-splicing c)");
    assert(cc_car(cc_cdr(splicing_form)) == os_make_symbol("c"), "(unquote-splicing c)のcadrはsymbol c");
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

void test_os_read_line_comment_only() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "; this is a comment\n");

    lisp_val_t v = os_read(proc);
    assert(v == nil, "コメントのみの行ではnilが返る");
}

void test_os_read_comment_after_expr() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "42 ; the answer\n");

    lisp_val_t v = os_read(proc);
    assert(v == os_make_fixnum(42), "式の後の';'以降はコメントとして無視され42が読める");
}

void test_os_read_comment_immediately_after_token() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "42;comment\n");

    lisp_val_t v = os_read(proc);
    assert(v == os_make_fixnum(42), "空白を挟まない';'もトークンの区切りとしてコメント扱いされる");
}

void test_os_read_multiline_list_with_comment_line() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    clear_next_lines();
    push_string(proc, "(defun foo (x)\n");
    queue_next_line("  ; コメント行\n");
    queue_next_line("  (+ x 1))\n");

    lisp_val_t v = os_read(proc);
    assert(cc_car(v) == os_make_symbol("defun"), "コメント行を挟んでも1行目のcarはシンボルdefun");

    lisp_val_t body = cc_car(cc_cdr(cc_cdr(cc_cdr(v))));
    assert(cc_car(body) == os_make_symbol("+"), "コメント行の次の行まで読んだ本体のcarはシンボル+");
    assert(cc_car(cc_cdr(cc_cdr(body))) == os_make_fixnum(1), "本体の3番目は1");
}

void test_os_read_function_sugar() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#'car");

    lisp_val_t v = os_read(proc);
    assert(cc_car(v) == os_make_symbol("function"), "#'carのcarはsymbol function");
    assert(cc_car(cc_cdr(v)) == os_make_symbol("car"), "#'carのcadrはsymbol car");
    assert(cc_cdr(cc_cdr(v)) == nil, "#'carのcddrはnil");
}

void test_os_read_vector_literal() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#(1 2 3)");

    lisp_val_t v = os_read(proc);
    assert((v & TAG_MASK) == TAG_INSTANCE, "#(1 2 3)はTAG_INSTANCEを持つ");
    assert(((UINT64 *)(v & ~TAG_MASK))[0] == MAGIC_VECTOR, "#(1 2 3)はMAGIC_VECTORを持つ");

    lisp_val_t *header = os_vector_header(v);
    assert(header[0] == 1, "#(1 2 3)のrankは1");
    assert(header[1] == 3, "#(1 2 3)の長さは3");
    lisp_val_t *data = (lisp_val_t *)((lisp_addr_t)header + 16);
    assert(data[0] == os_make_fixnum(1), "#(1 2 3)の1番目は1");
    assert(data[1] == os_make_fixnum(2), "#(1 2 3)の2番目は2");
    assert(data[2] == os_make_fixnum(3), "#(1 2 3)の3番目は3");
}

void test_os_read_empty_vector_literal() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#()");

    lisp_val_t v = os_read(proc);
    assert((v & TAG_MASK) == TAG_INSTANCE, "#()はTAG_INSTANCEを持つ");
    assert(((UINT64 *)(v & ~TAG_MASK))[0] == MAGIC_VECTOR, "#()はMAGIC_VECTORを持つ");
    lisp_val_t *header = os_vector_header(v);
    assert(header[0] == 1, "#()のrankは1");
    assert(header[1] == 0, "#()の長さは0");
}

void test_os_read_char_literal_simple() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#\\a");

    lisp_val_t v = os_read(proc);
    assert((v & TAG_MASK) == TAG_CHAR, "#\\aはTAG_CHARを持つ");
    assert((v >> 3) == 'a', "#\\aは大文字化されずに小文字'a'として読める");
}

void test_os_read_char_literal_paren() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#\\(");

    lisp_val_t v = os_read(proc);
    assert((v & TAG_MASK) == TAG_CHAR, "#\\(はTAG_CHARを持つ");
    assert((v >> 3) == '(', "#\\(は'('自身のCHARとして読める");
}

void test_os_read_char_literal_space() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#\\Space");

    lisp_val_t v = os_read(proc);
    assert((v & TAG_MASK) == TAG_CHAR, "#\\SpaceはTAG_CHARを持つ");
    assert((v >> 3) == ' ', "#\\Spaceはスペース文字として読める");
}

void test_os_read_char_literal_newline() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#\\Newline");

    lisp_val_t v = os_read(proc);
    assert((v & TAG_MASK) == TAG_CHAR, "#\\NewlineはTAG_CHARを持つ");
    assert((v >> 3) == '\n', "#\\Newlineは改行文字として読める");
}

void test_os_read_char_literal_unknown_name_is_read_error() {
    initialize_processes(g_buffers);
    process_t *proc = get_current_process();
    push_string(proc, "#\\Foo");

    lisp_val_t v = os_read(proc);
    assert(v == g_sym_read_error, "未知の複数文字名#\\Fooはread errorになる");
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    setup_heap();
    setup_buffers();

    test_os_read_empty();
    test_os_read_whitespace_only();
    test_os_read_fixnum();
    test_os_read_negative_fixnum();
    test_os_read_bare_minus_is_symbol();
    test_os_read_bignum_literal();
    test_os_read_negative_bignum_literal();
    test_os_read_small_literal_stays_fixnum();
    test_os_read_positive_fixnum_with_sign();
    test_os_read_bare_plus_is_symbol();
    test_os_read_binary_literal();
    test_os_read_binary_literal_uppercase_prefix();
    test_os_read_octal_literal();
    test_os_read_octal_literal_uppercase_prefix();
    test_os_read_hex_literal_lowercase_digits();
    test_os_read_hex_literal_uppercase_prefix_and_digits();
    test_os_read_radix_literal_negative();
    test_os_read_radix_literal_explicit_positive_sign();
    test_os_read_radix_literal_invalid_digit_is_read_error();
    test_os_read_radix_literal_no_digits_is_read_error();
    test_os_read_hex_bignum_literal();
    test_os_read_symbol();
    test_os_read_string();
    test_os_read_empty_list();
    test_os_read_list();
    test_os_read_quote();
    test_os_read_quasiquote();
    test_os_read_unquote();
    test_os_read_unquote_splicing();
    test_os_read_quasiquote_with_unquote_in_list();
    test_os_read_multiple_expr_per_line();
    test_os_read_stray_close_paren();
    test_os_read_unterminated_string();
    test_os_read_unterminated_list();
    test_os_read_multiline_list();
    test_os_read_line_comment_only();
    test_os_read_comment_after_expr();
    test_os_read_comment_immediately_after_token();
    test_os_read_multiline_list_with_comment_line();
    test_os_read_function_sugar();
    test_os_read_vector_literal();
    test_os_read_empty_vector_literal();
    test_os_read_char_literal_simple();
    test_os_read_char_literal_paren();
    test_os_read_char_literal_space();
    test_os_read_char_literal_newline();
    test_os_read_char_literal_unknown_name_is_read_error();

    return g_test_failed ? 1 : 0;
}
