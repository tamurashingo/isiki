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
#include "mount.h"
#include "stream_lisp.h"

// stream.c/reader.c/load.c は virtio9p.c/p9.c/drivers/*.c をリンクしない。
// cc_load/os_read_stream/os_stream_* が参照する外部シンボルだけダミー実装を置く。
// (process.cはeval.c/runtime.cのGC_PROTECTが参照するget_current_process/get_processのためリンクする)

#define FAKE_DATA_MAX 512
static UINT8 g_fake_data[FAKE_DATA_MAX];
static UINT32 g_fake_data_len = 0;
static UINT32 g_fake_chunk_size = 5; // 0以外なら1回のTreadで返す最大バイト数(不揃いな再充填を再現するため小さめ)
static int g_fake_open_fail = 0;

// [M2-T1 / #193] **ホスト共有へ落ちていないことを数で見るための計器。**
// 「解決できない絶対パスはホスト共有へ落とさない」は、openが呼ばれなかったことで
// 固定する(signalの種類だけでは、9pのopenが失敗した場合と見分けが付かない)。
// g_fake_open_pathは9pへ渡された最終的なパス(= マウント接頭辞を剥がした相対パス)
static int g_fake_open_calls = 0;
#define FAKE_OPEN_PATH_MAX 256
static char g_fake_open_path[FAKE_OPEN_PATH_MAX];

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
    g_fake_open_calls = 0;
    g_fake_open_path[0] = '\0';
}

int os_virtio9p_open(const char *path, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)mode;
    g_fake_open_calls++;
    snprintf(g_fake_open_path, sizeof(g_fake_open_path), "%s", path);
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

// ---------------------------------------------------------------------------
// [#197] **直列線へ出た行を捕まえる。**
//
// load.cはos_diag_serial_writeを直接呼ばない(load.hのos_load_set_diag_writer
// の注記)。差せる形にしてあるので、**母艦が実際に読む文字列そのもの**を
// 単体試験から読める。#197の完了条件は「構文ごとに数えられるかを**実測した表**」で、
// 読みで埋めてはいけない。その実測をここで行う(QEMUを1周回さずに済む)。
// ---------------------------------------------------------------------------

#define DIAG_BUF_SIZE 2048
static char g_diag_buf[DIAG_BUF_SIZE];
static UINT32 g_diag_len = 0;

static void diag_capture(const char *s) {
    while (*s != '\0' && g_diag_len < DIAG_BUF_SIZE - 1) {
        g_diag_buf[g_diag_len++] = *s++;
    }
}

static void reset_diag(void) {
    g_diag_len = 0;
    g_diag_buf[0] = '\0';
}

static const char *diag_captured(void) {
    g_diag_buf[g_diag_len] = '\0';
    return g_diag_buf;
}

#define HEAP_SIZE (1024 * 1024)

static void setup_heap(void) {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
    os_register_load();
    /* [#197] 直列線の出し口を捕まえる側へ差す(実機ではos_diag_serial_write) */
    os_load_set_diag_writer(diag_capture);
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


// ===========================================================================
// [#197] 構文ごとに「打ち切りとして数えられるか」を**実測する**
//
// 予測は documents/load-abort-visibility.md §2-1 に**実測より前に**宣言してある
// (規則 11)。ここはその答え合わせであり、同時に将来の回帰を固定する検出器である。
//
// **なぜ表にするのか。** 調査(docs/investigation/detector-t4-passline.refs.md §C-2)は
// 「宛先が %TOP-LEVEL かつ MAGIC_BLOCK_EXIT のときだけ打ち切りになる」ことまでは
// 確かめたが、**go / tagbody 由来の脱出が同じ分類に入るかは読んでいない**と
// ★ を立てていた。読みで埋めてはいけないので、構文ごとに1件ずつ置いて数える。
//
// **数えてはいけない3つ(block / catch / tagbody が自分で捕まえる形)を
// 対照群として同じ表に入れてある。** 誤検出(正常な脱出を打ち切りと呼ぶ)は
// 「1周で誤って鳴る」形で効くので、陰性対照を同じ仕掛けで見る必要がある。
//
// **ここで測れないもの:** 条件クラスを1つも登録していない試験環境では
// signal が g_sym_eval_error で素通りする(runtime.c の os_signal_condition の注記)。
// したがって
//   - 宛先が生きていない return-from(<control-error> を signal する経路)
//   - 引数不足などの素のエラー
// の2つは**この試験では測れない**。実機(QEMU)側で測る。
// ===========================================================================

typedef struct {
    const char *label;        // 表の行の名前
    const char *src;          // 最上位に置く1つの式
    int expect_counted;       // 打ち切りとして数えられるべきか
    const char *expect_in_diag; // 直列線の理由に含まれるべき文字列(0なら見ない)
} abort_case_t;

static const abort_case_t g_abort_cases[] = {
    // 数えるべき側
    {"A  (return-from %top-level 9)",
     "(return-from %top-level 9)\n", 1, "[load] abort "},
    {"E  (throw 'tg 1) / catch 無し",
     "(throw 'tg 1)\n", 1, "uncaught catch-exit"},
    {"G  (go nowhere) / tagbody 無し",
     "(go nowhere)\n", 1, "uncaught go-exit"},
    {"H  (tagbody (go nowhere)) / 宛先のタグが無い",
     "(tagbody (go nowhere))\n", 1, "uncaught go-exit"},
    // 数えてはいけない側(陰性対照)
    {"B  (block b (return-from b 1))",
     "(block b (return-from b 1))\n", 0, 0},
    {"D  (catch 'tg (throw 'tg 1))",
     "(catch 'tg (throw 'tg 1))\n", 0, 0},
    {"F  (tagbody (go done) ... done)",
     "(tagbody (go done) (setq f197-x 1) done)\n", 0, 0},
};

static void test_load_counts_every_kind_of_escape(void) {
    reset_fake_state();
    uninstall_fake_report();

    printf("  [#197] 構文ごとの実測(1 = 数えられる / 0 = 数えられない):\n");
    for (UINT32 i = 0; i < sizeof(g_abort_cases) / sizeof(g_abort_cases[0]); i++) {
        const abort_case_t *c = &g_abort_cases[i];
        reset_capture();
        reset_diag();
        os_load_abort_stats_reset();

        /* **「直す前」の列を同じ試験の中で測る。**
           os_eval_top_level_ex は今回**変えていない**ので、この関数だけに
           同じ式を食わせれば「直す前は数えられたか」がそのまま出る。
           別の木を建てずに陽性対照が取れる(規則 8)。
           ここで出る 0 が、調査 §C-2 の「数えられていない脱出」そのものである */
        int eval_only = 0;
        {
            UINT8 buf[256];
            UINT32 n = 0;
            while (c->src[n] != '\0' && n < sizeof(buf)) {
                buf[n] = (UINT8)c->src[n];
                n++;
            }
            os_stream_t probe;
            os_stream_open_string_input(&probe, buf, n);
            lisp_val_t form = os_read_stream(&probe);
            GC_PROTECT(form);
            os_eval_top_level_ex(form, global_environment, &eval_only);
        }
        reset_capture();
        reset_diag();
        os_load_abort_stats_reset();

        int aborted = 0;
        lisp_val_t result = call_load_from_stream(c->src, "<memory>", &aborted);

        int counted = os_load_aborted_total();
        printf("    %-46s 直す前=%d 直した後=%d 第4引数=%d 直列線=%s",
               c->label, eval_only, counted, aborted, diag_captured());
        if (g_diag_len == 0) {
            printf("(無し)\n");
        }

        assert(result == g_sym_t,
            "どの構文でもloadは最後まで読んでg_sym_tを返す(契約は変えていない)");
        if (c->expect_counted) {
            assert(counted == 1, "この構文の脱出は打ち切りとして**数えられる**");
            if (c->expect_in_diag != 0 && strncmp(c->expect_in_diag, "uncaught", 8) == 0) {
                /* **穴そのものを固定する。** この3つは os_eval_top_level_ex だけでは
                   0 のまま = 直す前は数えられていなかった(調査 §C-2)。
                   ここが 1 に変わったら、脱出の分類が変わったということなので、
                   load の輪の側の判定を見直す合図である */
                assert(eval_only == 0,
                    "宛先の無い脱出は os_eval_top_level_ex 単独では数えられない(直す前の形)");
            }
            assert(aborted == 1, "第4引数(呼び元が渡す数え先)にも同じ1件が入る");
            assert(strstr(diag_captured(), c->expect_in_diag) != NULL,
                "直列線に理由の1行が出る(母艦が読むのはこの文字列である)");
        } else {
            assert(counted == 0, "自分で捕まえる脱出は打ち切りではない(陰性対照)");
            assert(g_diag_len == 0, "打ち切りでなければ直列線へ何も出さない(誤検出しない)");
        }
    }
}

// [#197] **0 を渡していた呼び元(cc_loadの3経路)が数えるようになったこと。**
// 以前は加算が `if (out_aborted_count != 0)` の中にあったため、
// 0 を渡す呼び元では**加算自体が起きていなかった**(= 捨てられていたのではなく
// 数えられていなかった)。ここが#197の真因である
static void test_cc_load_counts_and_names_the_file(void) {
    reset_fake_state();
    reset_capture();
    reset_diag();
    os_load_abort_stats_reset();
    install_fake_report();
    set_fake_data("(setq f197-a 1)\n(return-from %top-level 9)\n(setq f197-b 2)\n");

    lisp_val_t result = call_load("fake/half.lisp");

    assert(result == g_sym_t, "cc_loadの戻り値は従来どおりg_sym_t(契約は変えない)");
    assert(os_load_aborted_total() == 1,
        "**通常のloadでも総数が増える**(以前は0を渡していたので増えなかった)");
    assert(strstr(diag_captured(), "[load] file fake/half.lisp: aborted 1") != NULL,
        "**どのファイルで何件欠けたか**が直列線に出る(総数だけでは直せない)");
    assert(strstr(diag_captured(), "[load] abort fake/half.lisp: p2 load reported") != NULL,
        "理由も直列線に出る(画面だけに出す形では母艦から見えない)");
    uninstall_fake_report();
}

// [#197] **0件でも1行出す。** 出ないことを成功と読ませてはいけない
// (#204 / #208で学んだ形)。母艦側はこの行が無ければ落ちる
static void test_report_line_is_emitted_even_for_zero(void) {
    reset_diag();
    os_load_abort_stats_reset();

    os_load_report_aborts();

    assert(strcmp(diag_captured(), "[load] aborted top-level forms: 0\n") == 0,
        "打ち切りが0件でも総数の1行を必ず出す");

    // 陽性対照: 件数が入れば同じ行がその数で出る
    reset_diag();
    int aborted = 0;
    call_load_from_stream("(throw 'tg 1)\n(go nowhere)\n", "<memory>", &aborted);
    reset_diag();
    os_load_report_aborts();
    assert(strcmp(diag_captured(), "[load] aborted top-level forms: 2\n") == 0,
        "2件の脱出(catchの無いthrowとtagbodyの無いgo)が総数に積まれる");
    os_load_abort_stats_reset();
}

// ---------------------------------------------------------------------------
// [M2-T0] #196: 読み込みが記憶域の整理(GC)に耐えること
//
// docs/investigation/m2-load-from-disk.logic.md §6 / .refs.md §A-5。
// ディスク(FAT)経由の読み込み元はMAGIC_STREAMに包まないと中身ごと死ぬが、
// 包んだ以上**os_stream_t本体はGCのたびに別アドレスへ複製される**
// (gc_relocate_stream、runtime.c)。したがって「実体を指す生ポインタ」を
// 確保を跨いで持ち続けてはならない。
//
// **ここでの陽性対照(規則8)の作り方**: readerの中はLispを呼ばないので、
// 通常ビルドのユニットテストから「1つの式を読む途中でGCを起こす」ことはできない
// (os_alloc_bytesのGCは枯渇時のみ、%%DIAG-GC-STRESSはGC_DEBUGビルド限定)。
// そこでここでは**輪(os_load_from_stream)の側**を見る。輪はフォームを評価する
// ので、評価がGCを起こせばよい。test-force-gcというテスト専用の組み込みを
// フォームとして評価させることで、**実際にGCが走った状態**を作る。
// 式を読む途中(reader)の窓はGC_DEBUGビルドでしか強制できないので、
// test/lisp/load_gc_stress_test.lisp(%%DIAG-GC-STRESS 1)が見ている。
// ---------------------------------------------------------------------------

static int g_force_gc_calls = 0;

/** テスト専用の組み込み。評価されると実際にGCを1回走らせる */
static lisp_val_t fake_force_gc(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    g_force_gc_calls++;
    os_gc_collect();
    return g_sym_t;
}

/** テスト専用の組み込み。引数の個数だけconsを確保して**生きたまま**返す。
    GCのたびに複製される生存集合を大きくし、「古い複製が別オブジェクトの
    複製先として上書きされる」状況(stream_lisp.c:152-157の事故)を作るのに使う */
static lisp_val_t fake_grow_live(lisp_val_t args, lisp_val_t env) {
    (void)env;
    UINT64 n = os_fixnum_magnitude(cc_car(args));
    lisp_val_t acc = nil;
    GC_PROTECT(acc);
    for (UINT64 i = 0; i < n; i++) {
        acc = os_make_cons(os_make_fixnum((UINT64)i), acc);
    }
    return acc;
}

static void install_force_gc(void) {
    os_set_function(os_make_symbol("test-force-gc"),
                     os_make_native_function((lisp_addr_t)(void *)fake_force_gc),
                     global_environment);
    os_set_function(os_make_symbol("test-grow-live"),
                     os_make_native_function((lisp_addr_t)(void *)fake_grow_live),
                     global_environment);
}

/**
 * MAGIC_STREAMに包んだ文字列入力ストリームを作る。
 * バッファ・rawをos_alloc_rawで取り、**包む直前まで確保を挟まない**
 * ([ファイルI/O]#49のGC安全性の作法。stream_lisp.c)。
 * 包むのは本物のos_make_stream(stream_lisp.c)である
 * (M2-T1 / #193 でload.cがstream_lisp.cを要するようになり、
 *  このテストもstream_lisp.cをリンクするようになった。以前は手で写していた)。
 * **FAT経由のopen-input-streamが作るストリームと同じ種別・同じ包み方である。**
 */
static lisp_val_t make_wrapped_string_input(const char *src) {
    UINT32 n = 0;
    while (src[n] != '\0') {
        n++;
    }
    UINT8 *buf = (UINT8 *)(lisp_addr_t)os_alloc_raw(n);
    for (UINT32 i = 0; i < n; i++) {
        buf[i] = (UINT8)src[i];
    }
    os_stream_t *raw = (os_stream_t *)(lisp_addr_t)os_alloc_raw(sizeof(os_stream_t));
    os_stream_open_string_input(raw, buf, n);
    return os_make_stream(raw);
}

/** MAGIC_STREAMハンドルから生ポインタを取り直す */
static os_stream_t *wrapped_raw(lisp_val_t handle) {
    return os_stream_from_handle(handle);
}

/**
 * [M2-T0 / 陽性対照] 包んだストリームを輪へ渡し、**輪の中でGCが2回走る**入力を
 * 読み込ませる。輪が生ポインタを使い回していると、1回目のGCでos_stream_tは
 * 別アドレスへ移り、2回目のGCでその古い複製が別オブジェクトの複製先として
 * 上書きされる。以後の読み取りは化けた中身から進むので、
 * **後ろのフォームが評価されない / 二度評価される / 構文エラーになる。**
 * どれも「読み込みは成功を返す」ので、戻り値では見分けられない(静かな破壊)。
 */
static void test_load_from_wrapped_stream_survives_gc(void) {
    reset_capture();
    uninstall_fake_report();
    install_force_gc();
    g_force_gc_calls = 0;

    lisp_val_t handle = make_wrapped_string_input(
        "(setq m2t0-a 1)\n"
        "(test-force-gc)\n"
        "(setq m2t0-junk (test-grow-live 2000))\n"
        "(setq m2t0-b 2)\n"
        "(test-force-gc)\n"
        "(setq m2t0-c 3)\n"
        "(setq m2t0-d (+ m2t0-a m2t0-b m2t0-c))\n");
    GC_PROTECT(handle);

    int aborted = 0;
    lisp_val_t result = os_load_from_stream(wrapped_raw(handle), "<wrapped>",
                                            global_environment, &aborted);

    assert(g_force_gc_calls == 2, "輪の中でGCが2回走った(陽性対照。走っていなければ何も確かめていない)");
    assert(result == g_sym_t, "包んだストリームでも最後まで読めればg_sym_tを返す");
    assert(aborted == 0, "GCを跨いでも打ち切りは起きない");

    assert(os_get_variable(os_make_symbol("m2t0-a"), global_environment) == os_make_fixnum(1),
        "GCより前のフォームが評価されている");
    assert(os_get_variable(os_make_symbol("m2t0-b"), global_environment) == os_make_fixnum(2),
        "1回目のGCより後のフォームが評価されている");
    assert(os_get_variable(os_make_symbol("m2t0-c"), global_environment) == os_make_fixnum(3),
        "2回目のGCより後のフォームが評価されている");
    assert(os_get_variable(os_make_symbol("m2t0-d"), global_environment) == os_make_fixnum(6),
        "最後のフォームまで1回ずつ評価されている(二度評価も取りこぼしも無い)");
}

/**
 * [M2-T0 / 陽性対照] cc_loadはenvをGC_PROTECTしていなかった。
 * いまは9pのopenが確保しないので安全だが、マウント経由(os_mount_resolveは
 * os_make_symbolを呼び、FAT経路はLispを呼び戻す)にした瞬間に守りが要る。
 * **漏らすと以後の全フォームが再配置前の古い環境へ書き込まれる。**
 *
 * ここではその保護が効いていることを、**openの前にGCを起こす**ことで見る。
 * envはglobal_environmentではなく自前に作った環境で、GCで必ず動く位置にある。
 */
static void test_load_protects_env_across_open(void) {
    reset_fake_state();
    reset_capture();
    uninstall_fake_report();
    install_force_gc();

    // 呼び出し元の環境を1つ作る。global_environmentを親に持つ普通の環境
    lisp_val_t env = os_make_environment(os_make_symbol("m2t0-env"), global_environment);
    GC_PROTECT(env);

    set_fake_data("(setq m2t0-env-probe 55)\n");

    // openの前にGCが走った状態を作る: ここで1回走らせておくと、
    // 以後envは移動済みのアドレスを指す(GC_PROTECTが効いていれば追随する)
    os_gc_collect();

    lisp_val_t form = os_make_cons(os_make_symbol("LOAD"),
                                    os_make_cons(os_make_string("fake/env.lisp"), nil));
    GC_PROTECT(form);
    lisp_val_t result = cc_load(cc_cdr(form), env);

    assert(result == g_sym_t, "自前の環境を渡したloadも最後まで読めればg_sym_tを返す");
    assert(os_get_variable(os_make_symbol("m2t0-env-probe"), env) == os_make_fixnum(55),
        "読んだフォームは**呼び出し元の(再配置後の)環境**で評価されている");
}


// ---------------------------------------------------------------------------
// [M2-T1] #193: パスの契約 — **先頭の区切りで経路が分かれる**
//
//   先頭が'/'でない -> ホスト共有(9p)。マウント表は引かない
//   先頭が'/'       -> マウント表で解決。"/9p"配下は組み込みで常に解決できる
//   解決できない    -> **signalする。ホスト共有へ落とさない**
//
// docs/investigation/m2-load-from-disk.logic.md §8、.refs.md §B-2。
// **ここをゲスト無しで固められるのは mount.c が TEST_SRC_LOAD に入っているから**
// (*MOUNTS*はLisp側の動的変数なので、mount_test.cと同じ形で直接積める)。
// ---------------------------------------------------------------------------

/** *MOUNTS*へ(path device fs-type)のエントリを1件追加する(test/c/mount_test.cと同じ形) */
static void add_mount(const char *path, const char *device_name, const char *fs_type_name) {
    lisp_val_t path_val = os_make_string(path);
    GC_PROTECT(path_val);
    lisp_val_t device = os_make_symbol(device_name);
    GC_PROTECT(device);
    lisp_val_t fs_type = os_make_symbol(fs_type_name);
    GC_PROTECT(fs_type);
    lisp_val_t entry = os_make_cons(path_val, os_make_cons(device, fs_type));
    GC_PROTECT(entry);
    lisp_val_t sym = os_make_symbol("*MOUNTS*");
    GC_PROTECT(sym);
    lisp_val_t existing = os_get_dynamic(sym);
    GC_PROTECT(existing);
    os_set_dynamic(sym, os_make_cons(entry, existing));
}

static void reset_mounts(void) {
    os_set_dynamic(os_make_symbol("*MOUNTS*"), nil);
}

/**
 * [陰性対照] **先頭に区切りが無い名前は、根("/")を割り当てても9pのまま。**
 * ここが崩れると、根を割り当てる試験(test/lisp/partition_test.lisp)の後続のloadが
 * ディスクへ流れて落ちる(.refs.md §E-3)。「相対に'/'を補う」案を採らない理由そのもの。
 */
static void test_load_relative_path_stays_on_9p_even_with_root_mount(void) {
    reset_fake_state();
    reset_capture();
    uninstall_fake_report();
    reset_mounts();
    add_mount("/", "BLK0", ":FAT32");
    set_fake_data("(setq m2t1-rel 1)\n");

    lisp_val_t result = call_load("test/lisp/x.lisp");

    assert(result == g_sym_t, "先頭に区切りが無い名前のloadは根の割り当てがあっても成功する");
    assert(g_fake_open_calls == 1, "ホスト共有のopenが1回だけ呼ばれた(FATへ流れていない)");
    assert(strcmp(g_fake_open_path, "test/lisp/x.lisp") == 0,
        "**渡した名前がそのまま9pへ行く**(マウント表を引かないので接頭辞の剥がしも無い)");
    assert(os_get_variable(os_make_symbol("m2t1-rel"), global_environment) == os_make_fixnum(1),
        "相対指定のloadは中身まで評価される");
    reset_mounts();
}

/**
 * **先頭が'/'なら、マウント表を引いて"/9p"を剥がしてから9pへ渡す。**
 * 剥がさないと「9pのルート直下に9pという入口が無い」ことが理由で落ちる
 * (いままで test/lisp/io_signal_test.lisp がそうなっていた)。
 */
static void test_load_absolute_9p_path_strips_prefix(void) {
    reset_fake_state();
    reset_capture();
    uninstall_fake_report();
    reset_mounts();
    set_fake_data("(setq m2t1-abs 2)\n");

    lisp_val_t result = call_load("/9p/test/lisp/x.lisp");

    assert(result == g_sym_t, "'/9p'配下の絶対パスは*mounts*が空でもloadできる");
    assert(g_fake_open_calls == 1, "ホスト共有のopenが1回だけ呼ばれた");
    assert(strcmp(g_fake_open_path, "/test/lisp/x.lisp") == 0,
        "9pへ渡るのは'/9p'を剥がした相対パス");
    assert(os_get_variable(os_make_symbol("m2t1-abs"), global_environment) == os_make_fixnum(2),
        "'/9p'配下のloadは中身まで評価される");
}

/**
 * [#193 の決定を固定する] **解決できない絶対パスはホスト共有へ落とさない。**
 * 落ちていないことは「9pのopenが一度も呼ばれていない」で見る
 * (戻り値だけでは9pのopen失敗と区別が付かない。.refs.md §B-5 / logic.md §9-5)。
 */
static void test_load_unresolvable_absolute_path_does_not_fall_back_to_9p(void) {
    reset_fake_state();
    reset_capture();
    uninstall_fake_report();
    reset_mounts();
    add_mount("/mnt", "BLK0", ":FAT16");
    // 9pが開けることを**わざと**有効にしておく。落ちてしまえばopenが呼ばれる
    set_fake_data("(setq m2t1-leak 3)\n");

    lisp_val_t result = call_load("/mnt2/typo.lisp");

    assert(result == g_sym_eval_error, "どのマウントにも解決できない絶対パスのloadはsignalへ回る");
    assert(g_fake_open_calls == 0,
        "**ホスト共有のopenは1度も呼ばれない**(綴り間違いが9pの文面で失敗しない)");
    assert(os_get_variable(os_make_symbol("m2t1-leak"), global_environment) != os_make_fixnum(3),
        "解決できなかったのでファイルの中身は評価されていない(未束縛のまま)");
    reset_mounts();
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

    // [#197] 構文ごとの実測と、母艦へ出る行
    test_load_counts_every_kind_of_escape();
    test_cc_load_counts_and_names_the_file();
    test_report_line_is_emitted_even_for_zero();

    test_load_from_wrapped_stream_survives_gc();
    test_load_protects_env_across_open();

    test_load_relative_path_stays_on_9p_even_with_root_mount();
    test_load_absolute_9p_path_strips_prefix();
    test_load_unresolvable_absolute_path_does_not_fall_back_to_9p();

    return g_test_failed ? 1 : 0;
}
