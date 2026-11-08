#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "lisp.h"
#include "process.h"
#include "reader.h"
#include "eval.h"
#include "load.h"

// stream.c/reader.c/load.c は virtio9p.c/p9.c/drivers/*.c をリンクしない。
// cc_load/os_read_stream/os_stream_* が参照する外部シンボルだけダミー実装を置く。
// (process.cはeval.c/runtime.cのGC_PROTECTが参照するget_current_process/get_processのためリンクする)

#define FAKE_DATA_MAX 512
static UINT8 g_fake_data[FAKE_DATA_MAX];
static UINT32 g_fake_data_len = 0;
static UINT32 g_fake_chunk_size = 5; // 0以外なら1回のTreadで返す最大バイト数(不揃いな再充填を再現するため小さめ)
static int g_fake_open_fail = 0;

static void set_fake_data(const char *s) {
    UINT32 n = 0;
    while (s[n] != '\0' && n < FAKE_DATA_MAX) {
        g_fake_data[n] = (UINT8)s[n];
        n++;
    }
    g_fake_data_len = n;
}

static void reset_fake_state(void) {
    g_fake_open_fail = 0;
    g_fake_chunk_size = 5;
}

int os_virtio9p_open(const char *path, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path;
    (void)mode;
    if (g_fake_open_fail) {
        if (err_msg != 0 && err_msg_cap > 0) {
            snprintf(err_msg, err_msg_cap, "fake open failure");
        }
        return 0;
    }
    *out_fid = 1;
    return 1;
}

int os_virtio9p_create(const char *path, UINT32 perm, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path;
    (void)perm;
    (void)mode;
    (void)err_msg;
    (void)err_msg_cap;
    *out_fid = 1;
    return 1;
}

int os_virtio9p_write_chunk(UINT32 fid, UINT64 offset, const UINT8 *data, UINT32 count,
                             UINT32 *out_written, char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)offset;
    (void)data;
    (void)err_msg;
    (void)err_msg_cap;
    *out_written = count;
    return 1;
}

int os_virtio9p_read_chunk(UINT32 fid, UINT64 offset, UINT32 want,
                            const UINT8 **out_data, UINT32 *out_count,
                            char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)want;
    (void)err_msg;
    (void)err_msg_cap;

    UINT32 off = (UINT32)offset;
    UINT32 remaining = g_fake_data_len - off;
    UINT32 count = remaining;
    if (g_fake_chunk_size > 0 && g_fake_chunk_size < count) {
        count = g_fake_chunk_size;
    }
    *out_data = g_fake_data + off;
    *out_count = count;
    return 1;
}

int os_virtio9p_close(UINT32 fid, char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)err_msg;
    (void)err_msg_cap;
    return 1;
}

// runtime.c(os_alloc_bytesのOOM表示)が参照するダミー実装
static void dummy_write_string(struct _frame_buffer *self, const char *s) {
    (void)self;
    (void)s;
}

static frame_buffer g_active_frame_buffer = {
    .write_string = dummy_write_string,
};

frame_buffer* get_active_frame_buffer(void) {
    return &g_active_frame_buffer;
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

// process.c(spawn)が参照するinterrupt.cのget_fpu_default_stateのダミー実装。
// FXSAVE領域の初期値はこのテストの対象外なので、ゼロ埋めの512byteバッファを返すだけにする
static UINT8 g_fake_fpu_default_state[512] __attribute__((aligned(16)));

const void *get_fpu_default_state(void) {
    return g_fake_fpu_default_state;
}

void os_repl_step(process_t *proc) {
    (void)proc;
}

// reader.c(process用reader_source)が参照するダミー実装。
// load_testはos_read_stream経由のみを使うため実際には呼ばれない
void os_wait_for_more_input(process_t *proc) {
    (void)proc;
}

// load.c(cc_loadのエラー表示)が参照する現在プロセスのダミー実装。
// stdout_bufferへの書き込みは静的バッファへキャプチャする
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

static frame_buffer g_proc_frame_buffer = {
    .write_char = capture_write_char,
    .write_string = capture_write_string,
};

static void reset_capture(void) {
    g_capture_len = 0;
}

/** キャプチャした出力をNUL終端の文字列として取り出す */
static const char *captured(void) {
    g_capture_buf[g_capture_len] = '\0';
    return g_capture_buf;
}

#define HEAP_SIZE (1024 * 1024)

static void setup_heap(void) {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
    os_register_load();
}

static void setup_process(void) {
    process_t *proc = get_process(0);
    memset(proc, 0, sizeof(*proc));
    proc->stdout_buffer = &g_proc_frame_buffer;
}

static lisp_val_t call_load(const char *path) {
    lisp_val_t form = os_make_cons(os_make_symbol("LOAD"),
                                    os_make_cons(os_make_string(path), nil));
    return os_eval(form, global_environment);
}

static void test_load_evaluates_multiple_forms(void) {
    reset_fake_state();
    reset_capture();
    set_fake_data("(defun add1 (x) (+ x 1))\n(setq loaded-flag 42)\n");

    lisp_val_t result = call_load("fake/multi.lisp");

    assert(result == g_sym_t, "複数フォームを含むファイルのloadはg_sym_tを返す");

    lisp_val_t call_result = os_eval(
        os_make_cons(os_make_symbol("add1"), os_make_cons(os_make_fixnum(5), nil)),
        global_environment);
    assert(call_result == os_make_fixnum(6), "loadで定義された関数を後から呼び出せる");

    lisp_val_t flag = os_get_variable(os_make_symbol("loaded-flag"), global_environment);
    assert(flag == os_make_fixnum(42), "loadで設定された変数を後から参照できる");
}

static void test_load_syntax_error_returns_eval_error(void) {
    reset_fake_state();
    reset_capture();
    set_fake_data("(defun bad (x)\n");

    lisp_val_t result = call_load("fake/bad.lisp");

    assert(result == g_sym_eval_error, "構文エラーを含むファイルのloadはg_sym_eval_errorを返す");
}

static void test_load_empty_file_returns_t(void) {
    reset_fake_state();
    reset_capture();
    set_fake_data("");

    lisp_val_t result = call_load("fake/empty.lisp");

    assert(result == g_sym_t, "空ファイルのloadは何も評価せずg_sym_tを返す");
}

static void test_load_open_failure_returns_eval_error(void) {
    reset_fake_state();
    reset_capture();
    g_fake_open_fail = 1;

    lisp_val_t result = call_load("fake/missing.lisp");

    assert(result == g_sym_eval_error, "openに失敗した場合はg_sym_eval_errorを返す");
}

// ---------------------------------------------------------------------------
// [P2] ロード中にフォームが打ち切られたときの扱い
//
// init.lispをロードしないテストなので、errorやreport-conditionは使えない。
// %abort-top-levelが実際に出すのと同じ脱出((return-from %top-level ...))を
// フォームとして直接書き、%REPORT-CONDITION-STRINGはフェイクを登録して、
// C側の配線だけを検証する。
// ---------------------------------------------------------------------------

static int g_fake_report_calls = 0;

static lisp_val_t fake_report_condition_string(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    g_fake_report_calls++;
    return os_make_string("p2 load reported");
}

static void install_fake_report(void) {
    os_set_function(os_make_symbol("%REPORT-CONDITION-STRING"),
                     os_make_native_function((lisp_addr_t)(void *)fake_report_condition_string),
                     global_environment);
}

static void uninstall_fake_report(void) {
    os_set_function(os_make_symbol("%REPORT-CONDITION-STRING"), nil, global_environment);
}

static void test_load_aborted_form_reports_and_continues(void) {
    reset_fake_state();
    reset_capture();
    install_fake_report();
    g_fake_report_calls = 0;
    // 2番目のフォームが打ち切られる。**3番目は評価されなければならない**
    set_fake_data("(setq p2-a 1)\n(return-from %top-level 9)\n(setq p2-b 2)\n");

    lisp_val_t result = call_load("fake/abort.lisp");

    assert(result == g_sym_t, "打ち切られたフォームがあってもloadはg_sym_tを返す(ロードは続行する)");
    assert(g_fake_report_calls == 1, "打ち切られたフォーム1つにつき1回report-conditionを呼ぶ");
    assert(strcmp(captured(), "load: fake/abort.lisp: p2 load reported\n") == 0,
        "打ち切りはパス付きでreport-conditionのメッセージとして表示される");

    lisp_val_t a = os_get_variable(os_make_symbol("p2-a"), global_environment);
    assert(a == os_make_fixnum(1), "打ち切りより前のフォームは評価されている");
    lisp_val_t b = os_get_variable(os_make_symbol("p2-b"), global_environment);
    assert(b == os_make_fixnum(2), "打ち切られたフォームの**次**のフォームも評価される");
    uninstall_fake_report();
}

static void test_load_normal_form_does_not_report(void) {
    reset_fake_state();
    reset_capture();
    install_fake_report();
    g_fake_report_calls = 0;
    set_fake_data("(setq p2-c 3)\n");

    lisp_val_t result = call_load("fake/normal.lisp");

    assert(result == g_sym_t, "正常なファイルのloadはg_sym_tを返す");
    assert(g_fake_report_calls == 0, "打ち切りが無ければreport-conditionを呼ばない");
    assert(strcmp(captured(), "") == 0, "打ち切りが無ければ何も表示しない(従来どおり結果は捨てる)");
    uninstall_fake_report();
}

static void test_load_aborted_form_falls_back_when_report_missing(void) {
    reset_fake_state();
    reset_capture();
    uninstall_fake_report();
    set_fake_data("(return-from %top-level 9)\n");

    lisp_val_t result = call_load("fake/abort2.lisp");

    assert(result == g_sym_t, "report-conditionが無くてもloadは続行してg_sym_tを返す");
    assert(strcmp(captured(), "load: fake/abort2.lisp: 9\n") == 0,
        "%REPORT-CONDITION-STRINGが未定義なら生の値の印字へ落とす");
}

// ---------------------------------------------------------------------------
// [L1] os_load_from_stream: 「読んで評価する輪」をストリーム引数で呼ぶ経路。
//
// cc_loadは9Pのfakeに乗るが、この関数は入力元を問わないので**文字列入力
// ストリーム**で直接叩ける(カーネルへ同梱したソースを読む経路と同じ形)。
//
// ここで確かめたいのは**打ち切り件数が見えること**である。輪は打ち切られた
// フォームを飛ばして最後までg_sym_tを返すので、戻り値では「半分失敗した」が
// 分からない(docs/plans/local-load-and-core-image.md 設計判断L1)。
// 件数が0のままなら、同梱の読み込みを止める判断が実装できない。
// 規則8の陽性対照として、**打ち切りのある入力で1、無い入力で0**を両方見る
// ---------------------------------------------------------------------------

#define FROM_STREAM_BUF_MAX 512
static UINT8 g_from_stream_buf[FROM_STREAM_BUF_MAX];

static lisp_val_t call_load_from_stream(const char *src, const char *name, int *out_count) {
    UINT32 n = 0;
    while (src[n] != '\0' && n < FROM_STREAM_BUF_MAX) {
        g_from_stream_buf[n] = (UINT8)src[n];
        n++;
    }
    os_stream_t stream;
    os_stream_open_string_input(&stream, g_from_stream_buf, n);
    return os_load_from_stream(&stream, name, global_environment, out_count);
}

static void test_load_from_stream_evaluates_forms(void) {
    reset_capture();
    uninstall_fake_report();

    int aborted = 0;
    lisp_val_t result = call_load_from_stream(
        "(defun from-stream-add2 (x) (+ x 2))\n(setq from-stream-flag 7)\n",
        "<memory>", &aborted);

    assert(result == g_sym_t, "os_load_from_streamは最後まで読めればg_sym_tを返す");
    assert(aborted == 0, "打ち切りが無ければ件数は0のまま(陰性対照)");

    lisp_val_t call_result = os_eval(
        os_make_cons(os_make_symbol("from-stream-add2"), os_make_cons(os_make_fixnum(5), nil)),
        global_environment);
    assert(call_result == os_make_fixnum(7), "ストリーム経由で定義した関数を後から呼び出せる");
    lisp_val_t flag = os_get_variable(os_make_symbol("from-stream-flag"), global_environment);
    assert(flag == os_make_fixnum(7), "ストリーム経由で設定した変数を後から参照できる");
}

static void test_load_from_stream_counts_aborted_forms(void) {
    reset_capture();
    uninstall_fake_report();

    int aborted = 0;
    lisp_val_t result = call_load_from_stream(
        "(return-from %top-level 9)\n(setq from-stream-after 11)\n",
        "<memory>", &aborted);

    assert(result == g_sym_t, "打ち切りがあっても戻り値はg_sym_tのまま(従来の挙動)");
    assert(aborted == 1, "打ち切られたフォームの件数が呼び出し元へ返る(陽性対照)");
    assert(strcmp(captured(), "load: <memory>: 9\n") == 0,
        "打ち切りの表示にはpathではなく引数で渡した名前が出る");

    lisp_val_t after = os_get_variable(os_make_symbol("from-stream-after"), global_environment);
    assert(after == os_make_fixnum(11), "打ち切られたフォームの次のフォームは評価される");
}

static void test_load_from_stream_accepts_null_count(void) {
    reset_capture();
    uninstall_fake_report();

    // cc_loadが渡す形。件数を受け取らなくても落ちないこと
    lisp_val_t result = call_load_from_stream("(return-from %top-level 9)\n", "<memory>", 0);

    assert(result == g_sym_t, "件数の受け取り先が0でも打ち切りを飛ばして続行する");
}

static void test_load_from_stream_syntax_error_signals(void) {
    reset_capture();
    uninstall_fake_report();

    int aborted = 0;
    lisp_val_t result = call_load_from_stream("(defun from-stream-bad (x)\n", "<memory>", &aborted);

    assert(result == g_sym_eval_error, "構文エラーはcc_loadと同じくsignalへ回る");
    assert(aborted == 0, "構文エラーは打ち切り件数には数えない");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    setup_heap();
    setup_process();

    test_load_evaluates_multiple_forms();
    test_load_syntax_error_returns_eval_error();
    test_load_empty_file_returns_t();
    test_load_aborted_form_reports_and_continues();
    test_load_normal_form_does_not_report();
    test_load_aborted_form_falls_back_when_report_missing();
    test_load_open_failure_returns_eval_error();

    test_load_from_stream_evaluates_forms();
    test_load_from_stream_counts_aborted_forms();
    test_load_from_stream_accepts_null_count();
    test_load_from_stream_syntax_error_signals();

    return g_test_failed ? 1 : 0;
}
