#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
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

// runtime.c が参照する g_frame_buffer のダミー実装。
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

// process.c(spawn)が参照するinterrupt.cのget_fpu_default_stateのダミー実装。
// FXSAVE領域の初期値はこのテストの対象外なので、ゼロ埋めの512byteバッファを返すだけにする
static UINT8 g_fake_fpu_default_state[512] __attribute__((aligned(16)));

const void *get_fpu_default_state(void) {
    return g_fake_fpu_default_state;
}

void os_repl_step(process_t *proc) {
    (void)proc;
}

// reader.c の os_read が参照するが、runtime_test.c では実際の割り込みが発生しないため
// 何もしないダミー実装を用意する(load_test.c等の既存テストと同じパターン)
void os_wait_for_more_input(process_t *proc) {
    (void)proc;
}

#define HEAP_SIZE (1024 * 1024)

// os_make_cons/symbol/char/string はヒープ確保とnilの初期化が前提なので、
// 各テスト実行前に heap_init と boot を済ませておく
static void setup_heap() {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
}

void test_os_make_fixnum() {
    lisp_val_t f1 = os_make_fixnum(42);
    assert(f1 >> 3 == 42, "os_make_fixnum(42)は3bit右シフトで42に戻る");
}

void test_os_make_cons() {
    lisp_val_t car = os_make_fixnum(1);
    lisp_val_t cdr = os_make_fixnum(2);
    lisp_val_t cons = os_make_cons(car, cdr);

    assert((cons & TAG_MASK) == TAG_CONS, "os_make_consの戻り値はTAG_CONSを持つ");
    lisp_val_t *cell = (lisp_val_t *)(cons & ~TAG_MASK);
    assert(cell[0] == car, "cons cellのword0はcarと一致する");
    assert(cell[1] == cdr, "cons cellのword1はcdrと一致する");


    lisp_val_t str = os_make_string("hello world");
    lisp_val_t cons2 = os_make_cons(str, cdr);
    lisp_val_t *cell2 = (lisp_val_t *)(cons2 & ~TAG_MASK);
    assert(cell2[0] == str, "cons cellのword0はstrと一致する");
    assert(cell2[1] == cdr, "cons cellのword1はcdrと一致する");

    assert((str & TAG_MASK) == TAG_STRING, "word0はTAG_STRINGを持つ");
    lisp_val_t *str_cell = (lisp_val_t *)(str & ~TAG_MASK);
    UINT64 str_len = str_cell[0];
    assert(str_len == 11, "word0(長さ)は11(\"hello world\"の文字数)である");
    const char *s = (const char *)(str_cell + 1);
    assert(strncmp(s, "hello world", str_len) == 0, "word1以降のバイト列は\"hello world\"である");
}

void test_os_make_char() {
    lisp_val_t c = os_make_char('A');
    assert((c & TAG_MASK) == TAG_CHAR, "os_make_charの戻り値はTAG_CHARを持つ");
    assert((c >> 3) == 'A', "os_make_char('A')は3bit右シフトで'A'に戻る");
}

void test_os_make_string() {
    lisp_val_t s = os_make_string("hi");
    assert((s & TAG_MASK) == TAG_STRING, "os_make_stringの戻り値はTAG_STRINGを持つ");

    lisp_addr_t addr = s & ~TAG_MASK;
    UINT64 *header = (UINT64 *)addr;
    assert(header[0] == 2, "文字列ヘッダのword0は文字列長と一致する");

    UINT8 *bytes = (UINT8 *)(addr + 8);
    assert(bytes[0] == 'h', "文字列本体の1文字目は'h'と一致する");
    assert(bytes[1] == 'i', "文字列本体の2文字目は'i'と一致する");
}

void test_os_make_symbol() {
    lisp_val_t sym = os_make_symbol("foo");
    assert((sym & TAG_MASK) == TAG_SYMBOL, "os_make_symbolの戻り値はTAG_SYMBOLを持つ");

    lisp_addr_t sym_addr = sym & ~TAG_MASK;
    lisp_val_t *slots = (lisp_val_t *)sym_addr;
    lisp_val_t name_str = slots[0];

    assert((name_str & TAG_MASK) == TAG_STRING, "symbolのword0(名前)はTAG_STRINGを持つ");
    lisp_addr_t str_addr = name_str & ~TAG_MASK;
    UINT64 *header = (UINT64 *)str_addr;
    assert(header[0] == 3, "symbol名の文字列長は3と一致する");

    UINT8 *bytes = (UINT8 *)(str_addr + 8);
    assert(bytes[0] == 'F', "symbol名の1文字目は'F'と一致する");
    assert(bytes[1] == 'O', "symbol名の2文字目は'O'と一致する");
    assert(bytes[2] == 'O', "symbol名の3文字目は'O'と一致する");

    lisp_val_t sym2 = os_make_symbol("Foo");
    assert(sym == sym2, "os_make_symbol(\"foo\")とos_make_symbol(\"Foo\")は同じアドレスを指す");

    lisp_val_t sym3 = os_make_symbol("symbol");
    lisp_val_t sym4 = os_make_symbol("SYMBOL");

    assert(sym3 == sym4, "symbol と SYMBOL は同じ symbol");
    assert(sym != sym3, "foo と symbol は違う symbol");
    assert(sym2 != sym3, "Foo と symbol は違う symbol");
    assert(sym != sym4, "foo と SYMBOL は違う symbol");
    assert(sym2 != sym4, "Foo と SYMBOL は違う symbol");
}

void test_os_make_symbol_prefix_is_not_confused() {
    // 先にinternした短い名前が、後からinternする長い名前のprefixになっている場合でも
    // 別のsymbolとして扱われることを確認する(UNQUOTE / UNQUOTE-SPLICINGで実際に踏んだ回帰)
    lisp_val_t shorter = os_make_symbol("UNQUOTE");
    lisp_val_t longer = os_make_symbol("UNQUOTE-SPLICING");

    assert(shorter != longer, "UNQUOTEとUNQUOTE-SPLICINGは違うsymbolになる");

    lisp_val_t longer_again = os_make_symbol("UNQUOTE-SPLICING");
    assert(longer == longer_again, "UNQUOTE-SPLICINGを2回internすると同じsymbolが返る");
}


void test_os_get_variable() {
    lisp_val_t base_env = os_make_environment(os_make_symbol("BASE-ENV"), nil);
    lisp_val_t current_env = os_make_environment(os_make_symbol("CURRENT-ENV"), base_env);

    os_set_variable(os_make_symbol("sym1"), os_make_fixnum(1), base_env);
    os_set_variable(os_make_symbol("sym2"), os_make_string("hello world"), current_env);

    // base_env にしかない変数も current_env から(親を辿って)取得できること
    lisp_val_t v1 = os_get_variable(os_make_symbol("sym1"), current_env);
    assert((v1 & TAG_MASK) == TAG_FIXNUM, "fixnumが返ること");
    assert(v1 >> 3 == 1, "1であること");

    // current_env 自身の変数が取得できること。シンボル名は大文字小文字を区別しない
    lisp_val_t v2 = os_get_variable(os_make_symbol("SYM2"), current_env);
    assert((v2 & TAG_MASK) == TAG_STRING, "stringが返ること");
    const char *s2 = (const char *)((v2 & ~TAG_MASK) + 8);
    assert(strncmp(s2, "hello world", 11) == 0, "\"hello world\"であること");


    // current_env で sym1 をセットしても base_env の値は書き換わらないこと
    os_set_variable(os_make_symbol("SYM1"), os_make_fixnum(42), current_env);

    lisp_val_t v3 = os_get_variable(os_make_symbol("sym1"), current_env);
    assert((v3 & TAG_MASK) == TAG_FIXNUM, "fixnumが返ること");
    assert(v3 >> 3 == 42, "current_envにセットした42が優先して返る");

    lisp_val_t v4 = os_get_variable(os_make_symbol("sym1"), base_env);
    assert(v4 >> 3 == 1, "base_env自身の値は書き換えられていないこと");

    // 未定義のシンボルはnilが返ること
    lisp_val_t v5 = os_get_variable(os_make_symbol("undefined"), current_env);
    assert(v5 == nil, "未定義のシンボルはnilが返る");
}
void test_os_get_function() {
    lisp_val_t base_env = os_make_environment(os_make_symbol("BASE-ENV"), nil);
    lisp_val_t current_env = os_make_environment(os_make_symbol("CURRENT-ENV"), base_env);

    lisp_val_t fn1 = os_make_native_function((lisp_addr_t)(void *)primitive_car);
    lisp_val_t fn2 = os_make_native_function((lisp_addr_t)(void *)primitive_cdr);

    os_set_function(os_make_symbol("fn1"), fn1, base_env);
    os_set_function(os_make_symbol("fn2"), fn2, current_env);

    // base_env にしかない関数も current_env から(親を辿って)取得できること
    lisp_val_t v1 = os_get_function(os_make_symbol("fn1"), current_env);
    assert(v1 == fn1, "base_envの関数がcurrent_envから取得できる");

    // current_env 自身の関数が取得できること。シンボル名は大文字小文字を区別しない
    lisp_val_t v2 = os_get_function(os_make_symbol("FN2"), current_env);
    assert(v2 == fn2, "current_env自身の関数が取得できる");

    // 未定義の関数はnilが返ること
    lisp_val_t v3 = os_get_function(os_make_symbol("undefined"), current_env);
    assert(v3 == nil, "未定義の関数はnilが返る");
}

// primitive_global_environmentが、呼び出し元のenv引数(与えた別環境)や引数の内容に
// 関わらず常にglobal_environment(os_bootstrap()が生成したroot environment)そのものを
// 返すことを確認する。primitive_current_environmentが返すproc->env(プロセスごとの
// 子env)とは別物であることが本質のため、あえてglobal_environmentとは無関係な
// 別環境をenv引数に渡して呼び出す。
void test_primitive_global_environment_returns_global_environment_regardless_of_caller_env() {
    lisp_val_t unrelated_env = os_make_environment(os_make_symbol("UNRELATED-ENV"), nil);

    lisp_val_t v1 = primitive_global_environment(nil, unrelated_env);
    assert(v1 == global_environment, "呼び出し元envと無関係にglobal_environmentが返る");

    lisp_val_t v2 = primitive_global_environment(nil, global_environment);
    assert(v2 == global_environment, "global_environmentを渡しても同じ値が返る(冗等)");
}

// primitive_set_current_environmentの戻り値がt/nilになったこと(旧実装は切り替え先の
// 環境そのものを返していたが、REPLで直接呼んだ際に環境の内部構造がそのまま表示されて
// 出力が大量になるため変更した)、および環境として妥当でない値(nil、cons以外)を渡した
// 場合は切り替えを行わずnilを返すことを確認する。
void test_primitive_set_current_environment_returns_t_or_nil_and_rejects_invalid_env() {
    lisp_val_t env1 = os_make_environment(os_make_symbol("SET-CUR-ENV-1"), nil);
    lisp_val_t env2 = os_make_environment(os_make_symbol("SET-CUR-ENV-2"), nil);

    lisp_val_t args1 = os_make_cons(env1, nil);
    lisp_val_t r1 = primitive_set_current_environment(args1, nil);
    assert(r1 == g_sym_t, "妥当な環境への切り替えはtが返る");
    assert(get_current_process()->env == env1, "実際にproc->envがenv1へ切り替わっている");

    lisp_val_t args_nil = os_make_cons(nil, nil);
    lisp_val_t r2 = primitive_set_current_environment(args_nil, nil);
    assert(r2 == nil, "nilは環境として妥当でないためnilが返る");
    assert(get_current_process()->env == env1, "失敗時はproc->envが変更されず前の環境のまま");

    lisp_val_t args_fixnum = os_make_cons(os_make_fixnum(42), nil);
    lisp_val_t r3 = primitive_set_current_environment(args_fixnum, nil);
    assert(r3 == nil, "consでない値も環境として妥当でないためnilが返る");
    assert(get_current_process()->env == env1, "失敗時はproc->envが変更されず前の環境のまま");

    lisp_val_t args2 = os_make_cons(env2, nil);
    lisp_val_t r4 = primitive_set_current_environment(args2, nil);
    assert(r4 == g_sym_t, "2回目の妥当な切り替えもtが返る");
    assert(get_current_process()->env == env2, "実際にproc->envがenv2へ切り替わっている");
}

// Phase2動作確認: Function Cellのアドレスは再defunしても不変であること(呼び出し側は
// cellアドレスだけ握っていればよく、cellの中身がどこを指しているかを意識しなくてよい)、
// および同じcellアドレス経由での呼び出しがcellの中身の書き換えに応じて動的に切り替わる
// ことを確認する。
void test_os_function_cell() {
    lisp_val_t env = os_make_environment(os_make_symbol("CELL-TEST-ENV"), nil);
    lisp_val_t sym = os_make_symbol("cell-test-fn");

    lisp_val_t fn_car = os_make_native_function((lisp_addr_t)(void *)primitive_car);
    os_set_function(sym, fn_car, env);
    lisp_val_t cell = os_get_function_cell(sym, env);
    assert(cell != nil, "定義済み関数のFunction Cellはnilではない");

    lisp_val_t pair = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
    lisp_val_t args = os_make_cons(pair, nil);
    lisp_val_t r1 = os_apply_via_cell(cell, args, env);
    assert(r1 == os_make_fixnum(1), "carとして定義した直後はcell経由の呼び出しがcarとして動く");

    // 再defun。呼び出し側が握っているcellアドレス自体は不変のまま、内容だけがcdrへ切り替わる
    lisp_val_t fn_cdr = os_make_native_function((lisp_addr_t)(void *)primitive_cdr);
    os_set_function(sym, fn_cdr, env);
    lisp_val_t cell_after = os_get_function_cell(sym, env);
    assert(cell_after == cell, "再defunしてもFunction Cellのアドレス自体は不変");

    lisp_val_t r2 = os_apply_via_cell(cell, args, env);
    assert(r2 == os_make_fixnum(2), "同じcellアドレス経由の呼び出しが再defun後はcdrとして動く(動的切り替え)");
}

static lisp_val_t make_arg_list(int argc, ...) {
    lisp_val_t vals[8];
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc; i++) {
        vals[i] = os_make_fixnum(va_arg(ap, UINT64));
    }
    va_end(ap);

    lisp_val_t list = nil;
    for (int i = argc - 1; i >= 0; i--) {
        list = os_make_cons(vals[i], list);
    }
    return list;
}

// make_arg_listは非負値専用(os_make_fixnum経由)なので、負数やbignumを引数に
// 渡したいテストではあらかじめ組み立てたlisp_val_tの配列からリストを作る
static lisp_val_t make_arg_list_vals(int argc, lisp_val_t *vals) {
    lisp_val_t list = nil;
    for (int i = argc - 1; i >= 0; i--) {
        list = os_make_cons(vals[i], list);
    }
    return list;
}

// is_floatはruntime.c内のstatic関数のため、テストからはMAGIC_FLOATを直接見て判定する
static int test_is_float(lisp_val_t val) {
    return (val & TAG_MASK) == TAG_INSTANCE && ((UINT64 *)(val & ~TAG_MASK))[0] == MAGIC_FLOAT;
}

void test_os_make_fixnum_signed() {
    lisp_val_t neg5 = os_make_fixnum_signed(1, 5);
    assert((neg5 & TAG_MASK) == TAG_FIXNUM, "os_make_fixnum_signed(1,5)はTAG_FIXNUM");
    assert(os_fixnum_is_negative(neg5), "os_make_fixnum_signed(1,5)は負");
    assert(os_fixnum_magnitude(neg5) == 5, "os_make_fixnum_signed(1,5)のマグニチュードは5");

    lisp_val_t zero_neg = os_make_fixnum_signed(1, 0);
    assert(!os_fixnum_is_negative(zero_neg), "os_make_fixnum_signed(1,0)は0に正規化され符号は負にならない");
    assert(zero_neg == os_make_fixnum(0), "os_make_fixnum_signed(1,0)はos_make_fixnum(0)と同じ表現になる");

    lisp_val_t max_fixnum = os_make_fixnum(FIXNUM_MAGNITUDE_MASK);
    assert((max_fixnum & TAG_MASK) == TAG_FIXNUM, "60bit境界値(FIXNUM_MAGNITUDE_MASK)はまだFIXNUM");
    assert(os_fixnum_magnitude(max_fixnum) == FIXNUM_MAGNITUDE_MASK, "60bit境界値のマグニチュードが一致する");
}

void test_os_make_integer_promotes_to_bignum() {
    // 2^60(FIXNUM_MAGNITUDE_MASK+1)は60bitに収まらないのでbignumになる
    UINT64 limbs_over[2] = {0, 0x10000000ULL};
    lisp_val_t over = os_make_integer(0, limbs_over, 2);
    assert((over & TAG_MASK) == TAG_INSTANCE, "60bitを超えるマグニチュードはTAG_INSTANCEになる");
    UINT64 *obj = (UINT64 *)(over & ~TAG_MASK);
    assert(obj[0] == MAGIC_BIGNUM, "word0はMAGIC_BIGNUM");
    assert(obj[1] == 0, "非負なのでword1(sign)は0");
    assert(obj[2] == 2, "limb countは2");
    UINT64 *dst = (UINT64 *)obj[3];
    assert(dst[0] == 0 && dst[1] == 0x10000000ULL, "limb配列の内容がコピーされている");

    // 60bit以内に収まる値は符号があってもFIXNUMに降格される
    UINT64 limbs_small[1] = {42};
    lisp_val_t neg_small = os_make_integer(1, limbs_small, 1);
    assert((neg_small & TAG_MASK) == TAG_FIXNUM, "60bit以内のbignum構築要求はFIXNUMに降格される");
    assert(os_fixnum_is_negative(neg_small) && os_fixnum_magnitude(neg_small) == 42,
           "降格されたFIXNUMは符号とマグニチュードを保持する");

    // 値0はsign指定に関わらず常にsign=0に正規化される
    UINT64 limbs_zero[1] = {0};
    lisp_val_t zero = os_make_integer(1, limbs_zero, 1);
    assert(zero == os_make_fixnum(0), "0はsign指定に関わらずos_make_fixnum(0)に正規化される");
}

void test_primitive_add_signed_and_bignum() {
    lisp_val_t vals1[2] = {os_make_fixnum_signed(1, 5), os_make_fixnum(3)};
    lisp_val_t r1 = primitive_add(make_arg_list_vals(2, vals1), nil);
    assert(os_fixnum_is_negative(r1) && os_fixnum_magnitude(r1) == 2, "(+ -5 3) は-2");

    lisp_val_t vals2[2] = {os_make_fixnum_signed(1, 3), os_make_fixnum_signed(1, 4)};
    lisp_val_t r2 = primitive_add(make_arg_list_vals(2, vals2), nil);
    assert(os_fixnum_is_negative(r2) && os_fixnum_magnitude(r2) == 7, "(+ -3 -4) は-7");

    // 60bit境界+1でbignumに昇格する
    lisp_val_t max_fixnum = os_make_fixnum(FIXNUM_MAGNITUDE_MASK);
    lisp_val_t vals3[2] = {max_fixnum, os_make_fixnum(1)};
    lisp_val_t r3 = primitive_add(make_arg_list_vals(2, vals3), nil);
    assert((r3 & TAG_MASK) == TAG_INSTANCE, "60bit境界を超える加算はbignumになる");
    UINT64 *obj3 = (UINT64 *)(r3 & ~TAG_MASK);
    assert(obj3[0] == MAGIC_BIGNUM, "word0はMAGIC_BIGNUM");
    UINT64 *limbs3 = (UINT64 *)obj3[3];
    UINT64 magnitude3 = limbs3[0] | (obj3[2] > 1 ? limbs3[1] << 32 : 0);
    assert(magnitude3 == FIXNUM_MAGNITUDE_MASK + 1, "(+ FIXNUM_MAGNITUDE_MASK 1)のマグニチュードは2^60");

    // bignumから引いて60bit以内に戻れば再びFIXNUMに降格する
    lisp_val_t vals4[2] = {r3, os_make_fixnum_signed(1, 1)};
    lisp_val_t r4 = primitive_add(make_arg_list_vals(2, vals4), nil);
    assert((r4 & TAG_MASK) == TAG_FIXNUM, "bignumから1減らして60bit以内に戻ればFIXNUMに降格する");
    assert(!os_fixnum_is_negative(r4) && os_fixnum_magnitude(r4) == FIXNUM_MAGNITUDE_MASK,
           "降格後の値はFIXNUM_MAGNITUDE_MASKと一致する");
}

void test_primitive_subtract_unary_and_signed() {
    lisp_val_t vals1[1] = {os_make_fixnum(5)};
    lisp_val_t r1 = primitive_subtract(make_arg_list_vals(1, vals1), nil);
    assert(os_fixnum_is_negative(r1) && os_fixnum_magnitude(r1) == 5, "(- 5) は単項マイナスで-5");

    lisp_val_t vals2[1] = {os_make_fixnum_signed(1, 5)};
    lisp_val_t r2 = primitive_subtract(make_arg_list_vals(1, vals2), nil);
    assert(!os_fixnum_is_negative(r2) && os_fixnum_magnitude(r2) == 5, "(- -5) は5");

    lisp_val_t vals3[2] = {os_make_fixnum(3), os_make_fixnum(5)};
    lisp_val_t r3 = primitive_subtract(make_arg_list_vals(2, vals3), nil);
    assert(os_fixnum_is_negative(r3) && os_fixnum_magnitude(r3) == 2, "(- 3 5) は-2");

    lisp_val_t vals4[3] = {os_make_fixnum(5), os_make_fixnum(3), os_make_fixnum(2)};
    lisp_val_t r4 = primitive_subtract(make_arg_list_vals(3, vals4), nil);
    assert(!os_fixnum_is_negative(r4) && os_fixnum_magnitude(r4) == 0, "(- 5 3 2) は0");
}

void test_primitive_multiply_signed_and_bignum() {
    lisp_val_t vals1[2] = {os_make_fixnum_signed(1, 2), os_make_fixnum(3)};
    lisp_val_t r1 = primitive_multiply(make_arg_list_vals(2, vals1), nil);
    assert(os_fixnum_is_negative(r1) && os_fixnum_magnitude(r1) == 6, "(* -2 3) は-6");

    lisp_val_t vals2[2] = {os_make_fixnum_signed(1, 2), os_make_fixnum_signed(1, 3)};
    lisp_val_t r2 = primitive_multiply(make_arg_list_vals(2, vals2), nil);
    assert(!os_fixnum_is_negative(r2) && os_fixnum_magnitude(r2) == 6, "(* -2 -3) は6");

    // 2^30 * 2^31 = 2^61 > FIXNUM_MAGNITUDE_MASKなのでbignumになる
    lisp_val_t vals3[2] = {os_make_fixnum(1ULL << 30), os_make_fixnum(1ULL << 31)};
    lisp_val_t r3 = primitive_multiply(make_arg_list_vals(2, vals3), nil);
    assert((r3 & TAG_MASK) == TAG_INSTANCE, "2^30*2^31は60bitを超えるのでbignumになる");
    UINT64 *obj3 = (UINT64 *)(r3 & ~TAG_MASK);
    assert(obj3[0] == MAGIC_BIGNUM, "word0はMAGIC_BIGNUM");
}

void test_primitive_divide_signed() {
    lisp_val_t vals1[2] = {os_make_fixnum_signed(1, 12), os_make_fixnum(3)};
    lisp_val_t r1 = primitive_divide(make_arg_list_vals(2, vals1), nil);
    assert(os_fixnum_is_negative(r1) && os_fixnum_magnitude(r1) == 4, "(/ -12 3) は-4");

    lisp_val_t vals2[2] = {os_make_fixnum(12), os_make_fixnum_signed(1, 3)};
    lisp_val_t r2 = primitive_divide(make_arg_list_vals(2, vals2), nil);
    assert(os_fixnum_is_negative(r2) && os_fixnum_magnitude(r2) == 4, "(/ 12 -3) は-4");

    lisp_val_t vals3[2] = {os_make_fixnum_signed(1, 12), os_make_fixnum_signed(1, 3)};
    lisp_val_t r3 = primitive_divide(make_arg_list_vals(2, vals3), nil);
    assert(!os_fixnum_is_negative(r3) && os_fixnum_magnitude(r3) == 4, "(/ -12 -3) は4");
}

void test_primitive_arithmetic_with_float() {
    lisp_val_t vals1[2] = {os_make_fixnum(1), os_make_float(2.5)};
    lisp_val_t r1 = primitive_add(make_arg_list_vals(2, vals1), nil);
    assert(test_is_float(r1) && os_float_value(r1) == 3.5, "(+ 1 2.5) はfloatの3.5");

    lisp_val_t vals2[2] = {os_make_float(5.5), os_make_fixnum(2)};
    lisp_val_t r2 = primitive_subtract(make_arg_list_vals(2, vals2), nil);
    assert(test_is_float(r2) && os_float_value(r2) == 3.5, "(- 5.5 2) はfloatの3.5");

    lisp_val_t vals3[2] = {os_make_fixnum(2), os_make_float(1.5)};
    lisp_val_t r3 = primitive_multiply(make_arg_list_vals(2, vals3), nil);
    assert(test_is_float(r3) && os_float_value(r3) == 3.0, "(* 2 1.5) はfloatの3.0");

    lisp_val_t vals4[2] = {os_make_float(5.0), os_make_fixnum(2)};
    lisp_val_t r4 = primitive_divide(make_arg_list_vals(2, vals4), nil);
    assert(test_is_float(r4) && os_float_value(r4) == 2.5, "(/ 5.0 2) はfloatの2.5");

    lisp_val_t vals5[2] = {os_make_fixnum(1), os_make_float(1.5)};
    assert(primitive_less_than(make_arg_list_vals(2, vals5), nil) == g_sym_t, "(< 1 1.5) はT");

    lisp_val_t vals6[2] = {os_make_fixnum(2), os_make_float(2.0)};
    assert(primitive_num_equal(make_arg_list_vals(2, vals6), nil) == g_sym_t, "(= 2 2.0) はT");
}

void test_primitive_multiply() {
    lisp_val_t r = primitive_multiply(make_arg_list(3, 2, 3, 4), nil);
    assert(r >> 3 == 24, "(* 2 3 4) は24");

    lisp_val_t r2 = primitive_multiply(make_arg_list(1, 5), nil);
    assert(r2 >> 3 == 5, "(* 5) は5");
}

void test_primitive_divide() {
    lisp_val_t r = primitive_divide(make_arg_list(2, 12, 3), nil);
    assert(r >> 3 == 4, "(/ 12 3) は4");

    lisp_val_t r2 = primitive_divide(make_arg_list(3, 24, 4, 2), nil);
    assert(r2 >> 3 == 3, "(/ 24 4 2) は3");

    lisp_val_t r3 = primitive_divide(make_arg_list(2, 5, 0), nil);
    assert(r3 == g_sym_eval_error, "(/ 5 0) はg_sym_eval_error");
}

void test_primitive_less_than() {
    assert(primitive_less_than(make_arg_list(3, 1, 2, 3), nil) == g_sym_t, "(< 1 2 3) はT");
    assert(primitive_less_than(make_arg_list(3, 1, 3, 2), nil) == nil, "(< 1 3 2) はnil");
    assert(primitive_less_than(make_arg_list(2, 1, 1), nil) == nil, "(< 1 1) はnil");
}

void test_primitive_greater_than() {
    assert(primitive_greater_than(make_arg_list(3, 3, 2, 1), nil) == g_sym_t, "(> 3 2 1) はT");
    assert(primitive_greater_than(make_arg_list(3, 3, 1, 2), nil) == nil, "(> 3 1 2) はnil");
}

void test_primitive_num_equal() {
    assert(primitive_num_equal(make_arg_list(3, 2, 2, 2), nil) == g_sym_t, "(= 2 2 2) はT");
    assert(primitive_num_equal(make_arg_list(2, 2, 3), nil) == nil, "(= 2 3) はnil");
}

void test_primitive_num_not_equal_ge_le() {
    assert(primitive_num_not_equal(make_arg_list(3, 1, 2, 3), nil) == g_sym_t, "(/= 1 2 3) はT");
    assert(primitive_num_not_equal(make_arg_list(3, 1, 1, 2), nil) == nil, "(/= 1 1 2) はnil");

    assert(primitive_greater_equal(make_arg_list(3, 3, 3, 2), nil) == g_sym_t, "(>= 3 3 2) はT");
    assert(primitive_greater_equal(make_arg_list(2, 2, 3), nil) == nil, "(>= 2 3) はnil");

    assert(primitive_less_equal(make_arg_list(3, 1, 1, 2), nil) == g_sym_t, "(<= 1 1 2) はT");
    assert(primitive_less_equal(make_arg_list(2, 3, 2), nil) == nil, "(<= 3 2) はnil");
}

void test_primitive_max_min_abs() {
    assert(primitive_max(make_arg_list(3, 1, 5, 3), nil) >> 3 == 5, "(max 1 5 3) は5");
    assert(primitive_min(make_arg_list(3, 5, 1, 3), nil) >> 3 == 1, "(min 5 1 3) は1");

    lisp_val_t vals1[2] = {os_make_fixnum_signed(1, 5), os_make_fixnum(3)};
    assert(primitive_max(make_arg_list_vals(2, vals1), nil) == os_make_fixnum(3), "(max -5 3) は3");
    assert(primitive_min(make_arg_list_vals(2, vals1), nil) == vals1[0], "(min -5 3) は-5");

    lisp_val_t neg = os_make_fixnum_signed(1, 7);
    lisp_val_t abs_neg = primitive_abs(os_make_cons(neg, nil), nil);
    assert(!os_fixnum_is_negative(abs_neg) && os_fixnum_magnitude(abs_neg) == 7, "(abs -7) は7");

    lisp_val_t pos = os_make_fixnum(7);
    assert(primitive_abs(os_make_cons(pos, nil), nil) == pos, "(abs 7) は7自身をそのまま返す(ヒープ確保なし)");
}

void test_primitive_div_mod() {
    // 仕様例(§19.4)の8符号パターンをすべて確認する
    struct { UINT64 z1_mag; int z1_neg; UINT64 z2_mag; int z2_neg; int div_mag; int div_neg; int mod_mag; int mod_neg; } cases[] = {
        {12, 0, 3, 0, 4, 0, 0, 0},
        {12, 0, 3, 1, 4, 1, 0, 0},
        {12, 1, 3, 0, 4, 1, 0, 0},
        {12, 1, 3, 1, 4, 0, 0, 0},
        {14, 0, 3, 0, 4, 0, 2, 0},
        {14, 0, 3, 1, 5, 1, 1, 1},
        {14, 1, 3, 0, 5, 1, 1, 0},
        {14, 1, 3, 1, 4, 0, 2, 1},
    };
    for (int i = 0; i < 8; i++) {
        lisp_val_t vals[2] = {os_make_fixnum_signed(cases[i].z1_neg, cases[i].z1_mag),
                               os_make_fixnum_signed(cases[i].z2_neg, cases[i].z2_mag)};
        lisp_val_t d = primitive_div(make_arg_list_vals(2, vals), nil);
        lisp_val_t m = primitive_mod(make_arg_list_vals(2, vals), nil);
        assert(os_fixnum_is_negative(d) == (cases[i].div_mag != 0 && cases[i].div_neg) && os_fixnum_magnitude(d) == (UINT64)cases[i].div_mag,
               "divの符号パターンテスト");
        assert(os_fixnum_is_negative(m) == (cases[i].mod_mag != 0 && cases[i].mod_neg) && os_fixnum_magnitude(m) == (UINT64)cases[i].mod_mag,
               "modの符号パターンテスト");
    }

    lisp_val_t vals_zero[2] = {os_make_fixnum(5), os_make_fixnum(0)};
    assert(primitive_div(make_arg_list_vals(2, vals_zero), nil) == g_sym_eval_error, "(div 5 0) はg_sym_eval_error");
    assert(primitive_mod(make_arg_list_vals(2, vals_zero), nil) == g_sym_eval_error, "(mod 5 0) はg_sym_eval_error");
}

void test_primitive_gcd_lcm() {
    lisp_val_t vals1[2] = {os_make_fixnum(0), os_make_fixnum_signed(1, 4)};
    assert(primitive_gcd(make_arg_list_vals(2, vals1), nil) == os_make_fixnum(4), "(gcd 0 -4) は4");

    assert(primitive_gcd(make_arg_list(2, 12, 8), nil) == os_make_fixnum(4), "(gcd 12 8) は4");

    lisp_val_t vals2[2] = {os_make_fixnum(0), os_make_fixnum(0)};
    assert(primitive_gcd(make_arg_list_vals(2, vals2), nil) == os_make_fixnum(0), "(gcd 0 0) は0");

    assert(primitive_lcm(make_arg_list(2, 4, 6), nil) == os_make_fixnum(12), "(lcm 4 6) は12");

    lisp_val_t vals3[2] = {os_make_fixnum(0), os_make_fixnum(5)};
    assert(primitive_lcm(make_arg_list_vals(2, vals3), nil) == os_make_fixnum(0), "(lcm 0 5) は0");
}

void test_primitive_isqrt() {
    assert(primitive_isqrt(make_arg_list(1, 49), nil) == os_make_fixnum(7), "(isqrt 49) は7");
    assert(primitive_isqrt(make_arg_list(1, 63), nil) == os_make_fixnum(7), "(isqrt 63) は7");
    assert(primitive_isqrt(make_arg_list(1, 0), nil) == os_make_fixnum(0), "(isqrt 0) は0");
    assert(primitive_isqrt(make_arg_list(1, 1), nil) == os_make_fixnum(1), "(isqrt 1) は1");
    assert(primitive_isqrt(make_arg_list(1, 2), nil) == os_make_fixnum(1), "(isqrt 2) は1");

    lisp_val_t neg1[1] = {os_make_fixnum_signed(1, 1)};
    assert(primitive_isqrt(make_arg_list_vals(1, neg1), nil) == g_sym_eval_error, "(isqrt -1) はg_sym_eval_error");

    // bignum境界: k = FIXNUM_MAGNITUDE_MASKとしてk*k(bignum)のisqrtがkに戻ることを確認する
    lisp_val_t k = os_make_fixnum(FIXNUM_MAGNITUDE_MASK);
    lisp_val_t ksq_vals[2] = {k, k};
    lisp_val_t ksq = primitive_multiply(make_arg_list_vals(2, ksq_vals), nil);
    assert((ksq & TAG_MASK) == TAG_INSTANCE, "k*kはbignumになる");
    assert(primitive_isqrt(os_make_cons(ksq, nil), nil) == k, "(isqrt (k*k)) はk(完全平方数)");

    lisp_val_t ksq_plus1_vals[2] = {ksq, os_make_fixnum(1)};
    lisp_val_t ksq_plus1 = primitive_add(make_arg_list_vals(2, ksq_plus1_vals), nil);
    assert(primitive_isqrt(os_make_cons(ksq_plus1, nil), nil) == k, "(isqrt (k*k+1)) もk(floor)");
}

void test_primitive_numberp_and_fixnump() {
    assert(primitive_numberp(make_arg_list(1, 42), nil) == g_sym_t, "(numberp 42) はT");
    assert(primitive_numberp(os_make_cons(os_make_symbol("foo"), nil), nil) == nil, "(numberp 'foo) はnil");
    assert(primitive_fixnump(make_arg_list(1, 42), nil) == g_sym_t, "(fixnump 42) はT");
}

void test_primitive_comparisons_signed_and_bignum() {
    lisp_val_t vals1[2] = {os_make_fixnum_signed(1, 5), os_make_fixnum_signed(1, 3)};
    assert(primitive_less_than(make_arg_list_vals(2, vals1), nil) == g_sym_t, "(< -5 -3) はT");

    lisp_val_t vals2[2] = {os_make_fixnum_signed(1, 3), os_make_fixnum_signed(1, 5)};
    assert(primitive_less_than(make_arg_list_vals(2, vals2), nil) == nil, "(< -3 -5) はnil");

    lisp_val_t vals3[2] = {os_make_fixnum_signed(1, 1), os_make_fixnum(0)};
    assert(primitive_less_than(make_arg_list_vals(2, vals3), nil) == g_sym_t, "(< -1 0) はT");

    // FIXNUM境界値とbignum(2^60)との比較
    UINT64 limbs[2] = {0, 0x10000000ULL};
    lisp_val_t bignum_val = os_make_integer(0, limbs, 2);
    lisp_val_t max_fixnum = os_make_fixnum(FIXNUM_MAGNITUDE_MASK);
    lisp_val_t vals4[2] = {max_fixnum, bignum_val};
    assert(primitive_less_than(make_arg_list_vals(2, vals4), nil) == g_sym_t,
           "(< FIXNUM_MAGNITUDE_MASK 2^60) はT");
    lisp_val_t vals5[2] = {bignum_val, max_fixnum};
    assert(primitive_greater_than(make_arg_list_vals(2, vals5), nil) == g_sym_t,
           "(> 2^60 FIXNUM_MAGNITUDE_MASK) はT");

    lisp_val_t neg_bignum = os_make_integer(1, limbs, 2);
    lisp_val_t vals6[2] = {neg_bignum, max_fixnum};
    assert(primitive_less_than(make_arg_list_vals(2, vals6), nil) == g_sym_t,
           "(< -2^60 FIXNUM_MAGNITUDE_MASK) はT");

    lisp_val_t vals7[2] = {neg_bignum, neg_bignum};
    assert(primitive_num_equal(make_arg_list_vals(2, vals7), nil) == g_sym_t, "(= -2^60 -2^60) はT");
}

void test_primitive_bignump() {
    UINT64 limbs[2] = {0, 0x10000000ULL};
    lisp_val_t bignum_val = os_make_integer(0, limbs, 2);
    assert(primitive_bignump(os_make_cons(bignum_val, nil), nil) == g_sym_t, "(bignump 2^60) はT");
    assert(primitive_bignump(os_make_cons(os_make_fixnum(1), nil), nil) == nil, "(bignump 1) はnil");
    assert(primitive_fixnump(os_make_cons(bignum_val, nil), nil) == nil, "(fixnump 2^60) はnil");
    assert(primitive_numberp(os_make_cons(bignum_val, nil), nil) == g_sym_t, "(numberp 2^60) はT");
}

void test_primitive_floatp_and_float() {
    assert(primitive_floatp(os_make_cons(os_make_float(1.5), nil), nil) == g_sym_t, "(floatp 1.5) はT");
    assert(primitive_floatp(os_make_cons(os_make_fixnum(1), nil), nil) == nil, "(floatp 1) はnil");

    lisp_val_t r1 = primitive_float(os_make_cons(os_make_fixnum(3), nil), nil);
    assert(test_is_float(r1) && os_float_value(r1) == 3.0, "(float 3) はfloatの3.0");

    lisp_val_t r2 = primitive_float(os_make_cons(os_make_fixnum_signed(1, 3), nil), nil);
    assert(test_is_float(r2) && os_float_value(r2) == -3.0, "(float -3) はfloatの-3.0");

    lisp_val_t already_float = os_make_float(1.5);
    lisp_val_t r3 = primitive_float(os_make_cons(already_float, nil), nil);
    assert(r3 == already_float, "既にfloatならfloatはそのまま同じ値を返す");

    UINT64 limbs[2] = {0, 0x10000000ULL}; // 2^60
    lisp_val_t bignum_val = os_make_integer(0, limbs, 2);
    lisp_val_t r4 = primitive_float(os_make_cons(bignum_val, nil), nil);
    assert(test_is_float(r4) && os_float_value(r4) == 1152921504606846976.0, "(float 2^60) はbignumをdoubleへ変換したfloat");
}

static int approx_equal(double a, double b, double eps) {
    return fabs(a - b) < eps;
}

void test_primitive_sqrt() {
    assert(primitive_sqrt(make_arg_list(1, 4), nil) == os_make_fixnum(2), "(sqrt 4) は完全平方数なので整数の2");
    assert(primitive_sqrt(make_arg_list(1, 0), nil) == os_make_fixnum(0), "(sqrt 0) は0");

    lisp_val_t r1 = primitive_sqrt(make_arg_list(1, 2), nil);
    assert(test_is_float(r1) && approx_equal(os_float_value(r1), 1.4142135623730951, 1e-12), "(sqrt 2) は非完全平方数なのでfloat");

    // bignum完全平方数: k = FIXNUM_MAGNITUDE_MASKとしてk*k(bignum)のsqrtがkに戻る
    lisp_val_t k = os_make_fixnum(FIXNUM_MAGNITUDE_MASK);
    lisp_val_t ksq_vals[2] = {k, k};
    lisp_val_t ksq = primitive_multiply(make_arg_list_vals(2, ksq_vals), nil);
    assert(primitive_sqrt(os_make_cons(ksq, nil), nil) == k, "(sqrt (k*k)) はk(bignumの完全平方数)");

    lisp_val_t r2 = primitive_sqrt(os_make_cons(os_make_float(9.0), nil), nil);
    assert(test_is_float(r2) && os_float_value(r2) == 3.0, "(sqrt 9.0) はfloat入力でもfloatの3.0");

    // 型は数値で合っているが値が負(fixnum)。init.lisp未ロードなのでg_sym_eval_errorへフォールバック
    lisp_val_t neg1[1] = {os_make_fixnum_signed(1, 1)};
    assert(primitive_sqrt(make_arg_list_vals(1, neg1), nil) == g_sym_eval_error, "(sqrt -1) はdomain-error相当、init.lisp未ロード時はg_sym_eval_error");

    // 型は数値で合っているが値が負(float)
    lisp_val_t r3 = primitive_sqrt(os_make_cons(os_make_float(-4.0), nil), nil);
    assert(r3 == g_sym_eval_error, "(sqrt -4.0) もg_sym_eval_error");
}

void test_primitive_log() {
    lisp_val_t r1 = primitive_log(os_make_cons(os_make_float(2.718281828459045), nil), nil);
    assert(test_is_float(r1) && approx_equal(os_float_value(r1), 1.0, 1e-9), "(log e) は~1.0");

    lisp_val_t r2 = primitive_log(make_arg_list(1, 10), nil);
    assert(test_is_float(r2) && approx_equal(os_float_value(r2), 2.302585092994046, 1e-9), "(log 10) は~2.302585092994046");

    // 型は数値で合っているが値が0以下。init.lisp未ロードなのでg_sym_eval_errorへフォールバック
    assert(primitive_log(make_arg_list(1, 0), nil) == g_sym_eval_error, "(log 0) はg_sym_eval_error");

    lisp_val_t neg5[1] = {os_make_fixnum_signed(1, 5)};
    assert(primitive_log(make_arg_list_vals(1, neg5), nil) == g_sym_eval_error, "(log -5) もg_sym_eval_error");
}

void test_primitive_exp_sin_cos_atan2() {
    lisp_val_t r1 = primitive_exp(make_arg_list(1, 0), nil);
    assert(test_is_float(r1) && os_float_value(r1) == 1.0, "(exp 0) は1.0");

    lisp_val_t r2 = primitive_exp(make_arg_list(1, 1), nil);
    assert(test_is_float(r2) && approx_equal(os_float_value(r2), 2.718281828459045, 1e-9), "(exp 1) は~e");

    lisp_val_t r3 = primitive_sin(make_arg_list(1, 0), nil);
    assert(test_is_float(r3) && os_float_value(r3) == 0.0, "(sin 0) は0.0");

    lisp_val_t r4 = primitive_cos(make_arg_list(1, 0), nil);
    assert(test_is_float(r4) && os_float_value(r4) == 1.0, "(cos 0) は1.0");

    lisp_val_t r5 = primitive_atan2(make_arg_list(2, 0, 1), nil);
    assert(test_is_float(r5) && os_float_value(r5) == 0.0, "(atan2 0 1) は0.0");

    lisp_val_t r6 = primitive_atan2(make_arg_list(2, 1, 1), nil);
    assert(test_is_float(r6) && approx_equal(os_float_value(r6), 0.7853981633974483, 1e-9), "(atan2 1 1) は~pi/4");
}

void test_primitive_floor_ceiling_truncate_round() {
    lisp_val_t f34 = os_make_float(3.4);
    lisp_val_t fneg34 = os_make_float(-3.4);
    lisp_val_t f35 = os_make_float(3.5);
    lisp_val_t f25 = os_make_float(2.5);
    lisp_val_t fneg35 = os_make_float(-3.5);

    assert(primitive_floor(os_make_cons(f34, nil), nil) == os_make_fixnum(3), "(floor 3.4) は3");
    assert(primitive_floor(os_make_cons(fneg34, nil), nil) == os_make_fixnum_signed(1, 4), "(floor -3.4) は-4");
    assert(primitive_floor(make_arg_list(1, 5), nil) == os_make_fixnum(5), "(floor 5) はfixnumのまま高速パス");

    assert(primitive_ceiling(os_make_cons(f34, nil), nil) == os_make_fixnum(4), "(ceiling 3.4) は4");
    assert(primitive_ceiling(os_make_cons(fneg34, nil), nil) == os_make_fixnum_signed(1, 3), "(ceiling -3.4) は-3");

    assert(primitive_truncate(os_make_cons(f34, nil), nil) == os_make_fixnum(3), "(truncate 3.4) は3");
    assert(primitive_truncate(os_make_cons(fneg34, nil), nil) == os_make_fixnum_signed(1, 3), "(truncate -3.4) は-3");

    assert(primitive_round(os_make_cons(f35, nil), nil) == os_make_fixnum(4), "(round 3.5) はties-to-evenで4");
    assert(primitive_round(os_make_cons(f25, nil), nil) == os_make_fixnum(2), "(round 2.5) はties-to-evenで2");
    assert(primitive_round(os_make_cons(fneg35, nil), nil) == os_make_fixnum_signed(1, 4), "(round -3.5) はties-to-evenで-4");
}

void test_primitive_parse_number() {
    lisp_val_t r1 = primitive_parse_number(os_make_cons(os_make_string("123.34"), nil), nil);
    assert(test_is_float(r1) && approx_equal(os_float_value(r1), 123.34, 1e-9), "(parse-number \"123.34\") は123.34");

    lisp_val_t r2 = primitive_parse_number(os_make_cons(os_make_string("#XFACE"), nil), nil);
    assert(r2 == os_make_fixnum(64206), "(parse-number \"#XFACE\") は64206");

    lisp_val_t r3 = primitive_parse_number(os_make_cons(os_make_string("42"), nil), nil);
    assert(r3 == os_make_fixnum(42), "(parse-number \"42\") は42");

    // 数値として読めない文字列。init.lisp未ロードなのでg_sym_eval_errorへフォールバック
    lisp_val_t r4 = primitive_parse_number(os_make_cons(os_make_string("abc"), nil), nil);
    assert(r4 == g_sym_eval_error, "(parse-number \"abc\") はg_sym_eval_error");

    // 末尾に余分な文字が残る場合も数値として読めなかったものとして扱う
    lisp_val_t r5 = primitive_parse_number(os_make_cons(os_make_string("123abc"), nil), nil);
    assert(r5 == g_sym_eval_error, "(parse-number \"123abc\") もg_sym_eval_error");
}

void test_primitive_symbolp() {
    lisp_val_t sym = os_make_symbol("foo");
    assert(primitive_symbolp(os_make_cons(sym, nil), nil) == g_sym_t, "(symbolp 'foo) はT");
    assert(primitive_symbolp(os_make_cons(nil, nil), nil) == g_sym_t, "(symbolp nil) はT(nilはISLisp上symbol)");
    assert(primitive_symbolp(os_make_cons(os_make_fixnum(1), nil), nil) == nil, "(symbolp 1) はnil");
}

void test_primitive_consp() {
    lisp_val_t cons = os_make_cons(os_make_fixnum(1), nil);
    assert(primitive_consp(os_make_cons(cons, nil), nil) == g_sym_t, "(consp '(1)) はT");
    assert(primitive_consp(os_make_cons(nil, nil), nil) == nil, "(consp nil) はnil(nilはconsではない)");
    assert(primitive_consp(os_make_cons(os_make_fixnum(1), nil), nil) == nil, "(consp 1) はnil");
}

void test_primitive_eql() {
    lisp_val_t sym = os_make_symbol("foo");
    assert(primitive_eql(os_make_cons(os_make_fixnum(42), os_make_cons(os_make_fixnum(42), nil)), nil) == g_sym_t,
           "(eql 42 42) はT");
    assert(primitive_eql(os_make_cons(sym, os_make_cons(sym, nil)), nil) == g_sym_t, "(eql 'foo 'foo) はT");
    assert(primitive_eql(os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(2), nil)), nil) == nil,
           "(eql 1 2) はnil");
}

void test_primitive_eql_float() {
    assert(primitive_eql(os_make_cons(os_make_float(1.5), os_make_cons(os_make_float(1.5), nil)), nil) == g_sym_t,
           "(eql 1.5 1.5) はT");
    assert(primitive_eql(os_make_cons(os_make_float(1.5), os_make_cons(os_make_float(2.5), nil)), nil) == nil,
           "(eql 1.5 2.5) はnil");
    assert(primitive_eql(os_make_cons(os_make_float(1.0), os_make_cons(os_make_fixnum(1), nil)), nil) == nil,
           "(eql 1.0 1) はnil(型が異なるので不一致)");
}

void test_primitive_equal() {
    lisp_val_t list1 = os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(2), nil));
    lisp_val_t list2 = os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(2), nil));
    lisp_val_t list3 = os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(3), nil));
    assert(primitive_equal(os_make_cons(list1, os_make_cons(list2, nil)), nil) == g_sym_t,
           "(equal '(1 2) '(1 2)) は構造が同じならT(同一オブジェクトでなくても)");
    assert(primitive_equal(os_make_cons(list1, os_make_cons(list3, nil)), nil) == nil,
           "(equal '(1 2) '(1 3)) は内容が違うのでnil");

    lisp_val_t str1 = os_make_string("abc");
    lisp_val_t str2 = os_make_string("abc");
    lisp_val_t str3 = os_make_string("abd");
    assert(primitive_equal(os_make_cons(str1, os_make_cons(str2, nil)), nil) == g_sym_t,
           "(equal \"abc\" \"abc\") は別オブジェクトでも内容が同じならT");
    assert(primitive_equal(os_make_cons(str1, os_make_cons(str3, nil)), nil) == nil,
           "(equal \"abc\" \"abd\") は内容が違うのでnil");

    lisp_val_t vec1 = primitive_make_array(os_make_cons(os_make_fixnum(2), nil), nil);
    lisp_val_t vec2 = primitive_make_array(os_make_cons(os_make_fixnum(2), nil), nil);
    assert(primitive_equal(os_make_cons(vec1, os_make_cons(vec2, nil)), nil) == g_sym_t,
           "(equal #(nil nil) #(nil nil)) は要素がすべて同じならT");

    assert(primitive_equal(os_make_cons(os_make_fixnum(1), os_make_cons(os_make_symbol("a"), nil)), nil) == nil,
           "(equal 1 'a) はタグが異なるのでnil");
}

void test_primitive_eql_and_equal_bignum() {
    UINT64 limbs_a[2] = {0, 0x10000000ULL};
    UINT64 limbs_b[2] = {0, 0x10000000ULL};
    lisp_val_t bignum_a = os_make_integer(0, limbs_a, 2);
    lisp_val_t bignum_b = os_make_integer(0, limbs_b, 2);
    assert(bignum_a != bignum_b, "別々に構築したbignumは異なるヒープオブジェクトになる");
    assert(primitive_eql(os_make_cons(bignum_a, os_make_cons(bignum_b, nil)), nil) == g_sym_t,
           "(eql 2^60 2^60) は内容が同じならT(異なるオブジェクトでも)");

    UINT64 limbs_c[2] = {1, 0x10000000ULL};
    lisp_val_t bignum_c = os_make_integer(0, limbs_c, 2);
    assert(primitive_eql(os_make_cons(bignum_a, os_make_cons(bignum_c, nil)), nil) == nil,
           "(eql 2^60 (+ 2^60 1)) はnil");

    lisp_val_t list_a = os_make_cons(bignum_a, nil);
    lisp_val_t list_b = os_make_cons(bignum_b, nil);
    assert(primitive_equal(os_make_cons(list_a, os_make_cons(list_b, nil)), nil) == g_sym_t,
           "(equal (list 2^60) (list 2^60)) は内容が同じならT");
}

void test_primitive_listp() {
    lisp_val_t cons = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
    assert(primitive_listp(os_make_cons(cons, nil), nil) == g_sym_t, "(listp '(1 . 2)) はT(ドットリストも含む)");
    assert(primitive_listp(os_make_cons(nil, nil), nil) == g_sym_t, "(listp nil) はT");
    assert(primitive_listp(os_make_cons(os_make_fixnum(1), nil), nil) == nil, "(listp 1) はnil");
}

void test_primitive_characterp() {
    assert(primitive_characterp(os_make_cons(os_make_char('a'), nil), nil) == g_sym_t, "(characterp #\\a) はT");
    assert(primitive_characterp(os_make_cons(os_make_fixnum(1), nil), nil) == nil, "(characterp 1) はnil");
}

void test_primitive_char_comparisons() {
    lisp_val_t aa[2] = {os_make_char('a'), os_make_char('a')};
    lisp_val_t ab[2] = {os_make_char('a'), os_make_char('b')};
    lisp_val_t ba[2] = {os_make_char('b'), os_make_char('a')};
    lisp_val_t a_upper_a[2] = {os_make_char('a'), os_make_char('A')};

    assert(primitive_char_equal(make_arg_list_vals(2, aa), nil) == g_sym_t, "(char= #\\a #\\a) はT");
    assert(primitive_char_equal(make_arg_list_vals(2, ab), nil) == nil, "(char= #\\a #\\b) はnil");
    assert(primitive_char_equal(make_arg_list_vals(2, a_upper_a), nil) == nil, "(char= #\\a #\\A) はnil(大文字小文字を区別する)");

    assert(primitive_char_not_equal(make_arg_list_vals(2, aa), nil) == nil, "(char/= #\\a #\\a) はnil");
    assert(primitive_char_not_equal(make_arg_list_vals(2, ab), nil) == g_sym_t, "(char/= #\\a #\\b) はT");

    assert(primitive_char_less_than(make_arg_list_vals(2, aa), nil) == nil, "(char< #\\a #\\a) はnil");
    assert(primitive_char_less_than(make_arg_list_vals(2, ab), nil) == g_sym_t, "(char< #\\a #\\b) はT");
    assert(primitive_char_less_than(make_arg_list_vals(2, ba), nil) == nil, "(char< #\\b #\\a) はnil");

    assert(primitive_char_greater_than(make_arg_list_vals(2, ba), nil) == g_sym_t, "(char> #\\b #\\a) はT");

    assert(primitive_char_less_equal(make_arg_list_vals(2, aa), nil) == g_sym_t, "(char<= #\\a #\\a) はT");
    assert(primitive_char_greater_equal(make_arg_list_vals(2, ba), nil) == g_sym_t, "(char>= #\\b #\\a) はT");
    assert(primitive_char_greater_equal(make_arg_list_vals(2, aa), nil) == g_sym_t, "(char>= #\\a #\\a) はT");

    // 3引数以上の隣接ペア連鎖
    lisp_val_t abc[3] = {os_make_char('a'), os_make_char('b'), os_make_char('c')};
    assert(primitive_char_less_than(make_arg_list_vals(3, abc), nil) == g_sym_t, "(char< #\\a #\\b #\\c) はT");
    lisp_val_t acb[3] = {os_make_char('a'), os_make_char('c'), os_make_char('b')};
    assert(primitive_char_less_than(make_arg_list_vals(3, acb), nil) == nil, "(char< #\\a #\\c #\\b) はnil");
}

void test_primitive_stringp() {
    assert(primitive_stringp(os_make_cons(os_make_string("abc"), nil), nil) == g_sym_t, "(stringp \"abc\") はT");
    assert(primitive_stringp(os_make_cons(os_make_fixnum(1), nil), nil) == nil, "(stringp 1) はnil");
}

void test_primitive_functionp() {
    lisp_val_t native_fn = os_make_instance(MAGIC_FUNCTION_NATIVE, 0, 0, 0);
    lisp_val_t interp_fn = os_make_instance(MAGIC_FUNCTION_INTERPRETED, 0, 0, 0);
    lisp_val_t macro = os_make_instance(MAGIC_MACRO, 0, 0, 0);
    assert(primitive_functionp(os_make_cons(native_fn, nil), nil) == g_sym_t, "ネイティブ関数はfunctionp=T");
    assert(primitive_functionp(os_make_cons(interp_fn, nil), nil) == g_sym_t, "インタプリタ関数はfunctionp=T");
    assert(primitive_functionp(os_make_cons(macro, nil), nil) == nil, "macroはfunctionp=nil");
    assert(primitive_functionp(os_make_cons(os_make_fixnum(1), nil), nil) == nil, "(functionp 1) はnil");
}

void test_primitive_generic_function_p() {
    assert(primitive_generic_function_p(os_make_cons(os_make_fixnum(1), nil), nil) == nil,
           "defgeneric/defmethodが無いため常にnil");
}

void test_primitive_array_and_vector_predicates() {
    lisp_val_t vec1d = primitive_make_array(os_make_cons(os_make_fixnum(3), nil), nil);
    lisp_val_t dims2d = os_make_cons(os_make_fixnum(2), os_make_cons(os_make_fixnum(3), nil));
    lisp_val_t vec2d = primitive_make_array(os_make_cons(dims2d, nil), nil);
    lisp_val_t str = os_make_string("abc");

    assert(primitive_basic_array_p(os_make_cons(vec1d, nil), nil) == g_sym_t, "1次元配列はbasic-array-p=T");
    assert(primitive_basic_array_p(os_make_cons(vec2d, nil), nil) == g_sym_t, "2次元配列はbasic-array-p=T");
    assert(primitive_basic_array_p(os_make_cons(str, nil), nil) == g_sym_t, "stringはbasic-array-p=T");
    assert(primitive_basic_array_p(os_make_cons(os_make_fixnum(1), nil), nil) == nil, "(basic-array-p 1) はnil");

    assert(primitive_array_star_p(os_make_cons(vec1d, nil), nil) == nil, "rank1の配列はarray*-p=nil");
    assert(primitive_array_star_p(os_make_cons(vec2d, nil), nil) == g_sym_t, "rank2の配列はarray*-p=T");
    assert(primitive_array_star_p(os_make_cons(str, nil), nil) == nil, "stringはarray*-p=nil");

    assert(primitive_basic_vector_p(os_make_cons(vec1d, nil), nil) == g_sym_t, "rank1の配列はbasic-vector-p=T");
    assert(primitive_basic_vector_p(os_make_cons(vec2d, nil), nil) == nil, "rank2の配列はbasic-vector-p=nil");
    assert(primitive_basic_vector_p(os_make_cons(str, nil), nil) == g_sym_t, "stringはbasic-vector-p=T");

    assert(primitive_general_vector_p(os_make_cons(vec1d, nil), nil) == g_sym_t, "rank1の配列はgeneral-vector-p=T");
    assert(primitive_general_vector_p(os_make_cons(vec2d, nil), nil) == nil, "rank2の配列はgeneral-vector-p=nil");
    assert(primitive_general_vector_p(os_make_cons(str, nil), nil) == nil,
           "stringはbasic-vectorだがgeneral-vector-p=nil");
}

void test_primitive_streamp() {
    lisp_val_t stream = os_make_instance(MAGIC_STREAM, 0, 0, 0);
    assert(primitive_streamp(os_make_cons(stream, nil), nil) == g_sym_t, "streamはstreamp=T");
    assert(primitive_streamp(os_make_cons(os_make_fixnum(1), nil), nil) == nil, "(streamp 1) はnil");
}

void test_primitive_symbol_name() {
    lisp_val_t sym = os_make_symbol("foo");
    lisp_val_t name = primitive_symbol_name(os_make_cons(sym, nil), nil);
    assert((name & TAG_MASK) == TAG_STRING, "(symbol-name 'foo)はSTRINGを返す");

    char buf[16];
    os_string_to_cstr(name, buf, sizeof(buf));
    assert(strncmp(buf, "FOO", 3) == 0, "(symbol-name 'foo)は大文字化された\"FOO\"を返す");
}

void test_primitive_string_to_symbol() {
    lisp_val_t str = os_make_string("bar");
    lisp_val_t sym = primitive_string_to_symbol(os_make_cons(str, nil), nil);
    assert((sym & TAG_MASK) == TAG_SYMBOL, "(string-to-symbol \"bar\")はSYMBOLを返す");
    assert(sym == os_make_symbol("bar"), "(string-to-symbol \"bar\")はos_make_symbol(\"bar\")と同じsymbolになる(interning)");
}

void test_primitive_gensym() {
    lisp_val_t g1 = primitive_gensym(nil, nil);
    lisp_val_t g2 = primitive_gensym(nil, nil);
    assert((g1 & TAG_MASK) == TAG_SYMBOL, "(gensym)はSYMBOLを返す");
    assert(g1 != g2, "(gensym)を2回呼ぶと異なるsymbolが返る");
    assert(os_symbol_is_gensym(g1) != 0, "(gensym)が作るsymbolはgensymフラグが立っている");

    lisp_val_t normal_sym = os_make_symbol("SOME-NORMAL-SYMBOL-FOR-GENSYM-TEST");
    assert(os_symbol_is_gensym(normal_sym) == 0, "os_make_symbolが作る通常のsymbolはgensymフラグが立っていない");

    int count_before = os_symbol_table_count();
    primitive_gensym(nil, nil);
    primitive_gensym(nil, nil);
    int count_after = os_symbol_table_count();
    assert(count_before == count_after, "(gensym)を呼んでもg_symbol_tableの登録数は増えない");
}

void test_primitive_make_array_1d() {
    lisp_val_t array = primitive_make_array(os_make_cons(os_make_fixnum(3), nil), nil);
    assert((array & TAG_MASK) == TAG_INSTANCE, "(make-array 3)の戻り値はTAG_INSTANCEを持つ");
    assert(((UINT64 *)(array & ~TAG_MASK))[0] == MAGIC_VECTOR, "(make-array 3)の戻り値はMAGIC_VECTORを持つ");

    lisp_val_t *header = os_vector_header(array);
    assert(header[0] == 1, "1次元配列のrankは1");
    assert(header[1] == 3, "次元のサイズは3");

    lisp_val_t *data = (lisp_val_t *)((lisp_addr_t)header + 8 * (1 + 1));
    for (int i = 0; i < 3; i++) {
        assert(data[i] == nil, "要素はすべてnilで初期化される");
    }
}

void test_primitive_make_array_multi_dim() {
    lisp_val_t dims = os_make_cons(os_make_fixnum(2), os_make_cons(os_make_fixnum(3), nil));
    lisp_val_t array = primitive_make_array(os_make_cons(dims, nil), nil);
    assert((array & TAG_MASK) == TAG_INSTANCE, "(make-array '(2 3))の戻り値はTAG_INSTANCEを持つ");
    assert(((UINT64 *)(array & ~TAG_MASK))[0] == MAGIC_VECTOR, "(make-array '(2 3))の戻り値はMAGIC_VECTORを持つ");

    lisp_val_t *header = os_vector_header(array);
    assert(header[0] == 2, "2次元配列のrankは2");
    assert(header[1] == 2, "1次元目のサイズは2");
    assert(header[2] == 3, "2次元目のサイズは3");

    lisp_val_t *data = (lisp_val_t *)((lisp_addr_t)header + 8 * (1 + 2));
    for (int i = 0; i < 6; i++) {
        assert(data[i] == nil, "要素はすべてnilで初期化される");
    }
}

void test_primitive_array_dimensions() {
    lisp_val_t dims = os_make_cons(os_make_fixnum(2), os_make_cons(os_make_fixnum(3), nil));
    lisp_val_t array = primitive_make_array(os_make_cons(dims, nil), nil);

    lisp_val_t result = primitive_array_dimensions(os_make_cons(array, nil), nil);
    assert(cc_car(result) >> 3 == 2, "(array-dimensions a)の1番目は2");
    assert(cc_car(cc_cdr(result)) >> 3 == 3, "(array-dimensions a)の2番目は3");
    assert(cc_cdr(cc_cdr(result)) == nil, "(array-dimensions a)の終端はnil");
}

void test_primitive_array_dimensions_on_string() {
    lisp_val_t str = os_make_string("foo");

    lisp_val_t result = primitive_array_dimensions(os_make_cons(str, nil), nil);
    assert(cc_car(result) >> 3 == 3, "(array-dimensions \"foo\")の1番目は3");
    assert(cc_cdr(result) == nil, "(array-dimensions \"foo\")の終端はnil");
}

void test_primitive_aref_reads_back_value() {
    lisp_val_t dims = os_make_cons(os_make_fixnum(2), os_make_cons(os_make_fixnum(3), nil));
    lisp_val_t array = primitive_make_array(os_make_cons(dims, nil), nil);

    // 位置(1,2)は行優先オフセットで1*3+2=5番目
    lisp_val_t set_args = os_make_cons(array,
        os_make_cons(os_make_fixnum(1),
        os_make_cons(os_make_fixnum(2),
        os_make_cons(os_make_fixnum(99), nil))));
    lisp_val_t set_result = primitive_set_aref(set_args, nil);
    assert(set_result >> 3 == 99, "(set-aref a 1 2 99)は書き込んだ99を返す");

    lisp_val_t aref_args = os_make_cons(array,
        os_make_cons(os_make_fixnum(1),
        os_make_cons(os_make_fixnum(2), nil)));
    assert(primitive_aref(aref_args, nil) >> 3 == 99, "(aref a 1 2)は書き込んだ99を返す");

    lisp_val_t other_args = os_make_cons(array,
        os_make_cons(os_make_fixnum(0),
        os_make_cons(os_make_fixnum(0), nil)));
    assert(primitive_aref(other_args, nil) == nil, "書き込んでいない要素はnilのまま");
}

void test_primitive_aref_out_of_bounds() {
    lisp_val_t array = primitive_make_array(os_make_cons(os_make_fixnum(3), nil), nil);
    lisp_val_t args = os_make_cons(array, os_make_cons(os_make_fixnum(3), nil));
    assert(primitive_aref(args, nil) == g_sym_eval_error, "(aref a 3)は範囲外なのでg_sym_eval_error");
}

void test_primitive_vector() {
    lisp_val_t args = os_make_cons(os_make_symbol("a"),
        os_make_cons(os_make_symbol("b"), os_make_cons(os_make_symbol("c"), nil)));
    lisp_val_t vec = primitive_vector(args, nil);
    assert((vec & TAG_MASK) == TAG_INSTANCE, "(vector 'a 'b 'c)の戻り値はTAG_INSTANCEを持つ");
    assert(((UINT64 *)(vec & ~TAG_MASK))[0] == MAGIC_VECTOR, "(vector 'a 'b 'c)の戻り値はMAGIC_VECTORを持つ");

    lisp_val_t *header = os_vector_header(vec);
    assert(header[0] == 1, "(vector 'a 'b 'c)のrankは1");
    assert(header[1] == 3, "(vector 'a 'b 'c)の長さは3");

    lisp_val_t aref_args = os_make_cons(vec, os_make_cons(os_make_fixnum(1), nil));
    assert(primitive_aref(aref_args, nil) == os_make_symbol("b"), "(aref (vector 'a 'b 'c) 1)は'b");

    lisp_val_t empty = primitive_vector(nil, nil);
    assert(primitive_length(os_make_cons(empty, nil), nil) >> 3 == 0, "(vector)は空のvectorを返す");
}

void test_primitive_create_vector() {
    lisp_val_t vec = primitive_create_vector(os_make_cons(os_make_fixnum(3), nil), nil);
    assert(primitive_length(os_make_cons(vec, nil), nil) >> 3 == 3, "(create-vector 3)の長さは3");
    lisp_val_t aref_args = os_make_cons(vec, os_make_cons(os_make_fixnum(0), nil));
    assert(primitive_aref(aref_args, nil) == nil, "初期値省略時は各要素がnil");

    lisp_val_t vec_filled = primitive_create_vector(
        os_make_cons(os_make_fixnum(2), os_make_cons(os_make_fixnum(99), nil)), nil);
    lisp_val_t aref_args2 = os_make_cons(vec_filled, os_make_cons(os_make_fixnum(1), nil));
    assert(primitive_aref(aref_args2, nil) >> 3 == 99, "初期値指定時は各要素がその値で初期化される");
}

void test_primitive_garef_set_garef() {
    lisp_val_t vec = primitive_create_vector(os_make_cons(os_make_fixnum(3), nil), nil);
    lisp_val_t set_args = os_make_cons(vec,
        os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(42), nil)));
    assert(primitive_set_aref(set_args, nil) >> 3 == 42, "set-garefはset-arefと同じ実体で書き込める");

    lisp_val_t aref_args = os_make_cons(vec, os_make_cons(os_make_fixnum(1), nil));
    assert(primitive_aref(aref_args, nil) >> 3 == 42, "garefはarefと同じ実体で読み込める");
}

void test_primitive_set_car() {
    lisp_val_t c = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
    lisp_val_t args = os_make_cons(c, os_make_cons(os_make_fixnum(99), nil));

    lisp_val_t r = primitive_set_car(args, nil);
    assert(r >> 3 == 99, "(set-car c 99)は書き込んだ99を返す");
    assert(cc_car(c) >> 3 == 99, "carが99に書き換えられている");
    assert(cc_cdr(c) >> 3 == 2, "cdrは書き換えられていない");
}

void test_primitive_set_cdr() {
    lisp_val_t c = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
    lisp_val_t args = os_make_cons(c, os_make_cons(os_make_fixnum(99), nil));

    lisp_val_t r = primitive_set_cdr(args, nil);
    assert(r >> 3 == 99, "(set-cdr c 99)は書き込んだ99を返す");
    assert(cc_cdr(c) >> 3 == 99, "cdrが99に書き換えられている");
    assert(cc_car(c) >> 3 == 1, "carは書き換えられていない");
}

void test_primitive_set_aref_out_of_bounds() {
    lisp_val_t array = primitive_make_array(os_make_cons(os_make_fixnum(3), nil), nil);
    lisp_val_t args = os_make_cons(array,
        os_make_cons(os_make_fixnum(3),
        os_make_cons(os_make_fixnum(1), nil)));
    assert(primitive_set_aref(args, nil) == g_sym_eval_error, "(set-aref a 3 1)は範囲外なのでg_sym_eval_error");
}

void test_primitive_create_string_default_fill() {
    lisp_val_t str = primitive_create_string(os_make_cons(os_make_fixnum(3), nil), nil);
    assert((str & TAG_MASK) == TAG_STRING, "(create-string 3)の戻り値はTAG_STRINGを持つ");

    lisp_addr_t addr = str & ~TAG_MASK;
    lisp_val_t *header = (lisp_val_t *)addr;
    assert(header[0] == 3, "文字列長は3である");

    UINT8 *bytes = (UINT8 *)(addr + 8);
    assert(bytes[0] == ' ' && bytes[1] == ' ' && bytes[2] == ' ', "省略時は空白で初期化される");
}

void test_primitive_create_string_with_char() {
    lisp_val_t args = os_make_cons(os_make_fixnum(3), os_make_cons(os_make_char('A'), nil));
    lisp_val_t str = primitive_create_string(args, nil);

    lisp_addr_t addr = str & ~TAG_MASK;
    UINT8 *bytes = (UINT8 *)(addr + 8);
    assert(bytes[0] == 'A' && bytes[1] == 'A' && bytes[2] == 'A', "第二引数を指定すればその文字で初期化される");
}

void test_primitive_string_elt() {
    lisp_val_t str = os_make_string("hello");
    lisp_val_t r = primitive_string_elt(os_make_cons(str, os_make_cons(os_make_fixnum(1), nil)), nil);
    assert((r & TAG_MASK) == TAG_CHAR, "(string-elt \"hello\" 1)の戻り値はTAG_CHARを持つ");
    assert((r >> 3) == 'e', "(string-elt \"hello\" 1)は'e'を返す");
}

void test_primitive_string_elt_out_of_bounds() {
    lisp_val_t str = os_make_string("hi");
    lisp_val_t r = primitive_string_elt(os_make_cons(str, os_make_cons(os_make_fixnum(2), nil)), nil);
    assert(r == g_sym_eval_error, "(string-elt \"hi\" 2)は範囲外なのでg_sym_eval_error");
}

void test_primitive_string_comparisons() {
    lisp_val_t abcd_abcd[2] = {os_make_string("abcd"), os_make_string("abcd")};
    lisp_val_t abcd_wxyz[2] = {os_make_string("abcd"), os_make_string("wxyz")};
    lisp_val_t abcd_abcde[2] = {os_make_string("abcd"), os_make_string("abcde")};
    lisp_val_t abcde_abcd[2] = {os_make_string("abcde"), os_make_string("abcd")};

    assert(primitive_string_equal(make_arg_list_vals(2, abcd_abcd), nil) == g_sym_t, "(string= \"abcd\" \"abcd\") はT");
    assert(primitive_string_equal(make_arg_list_vals(2, abcd_wxyz), nil) == nil, "(string= \"abcd\" \"wxyz\") はnil");
    assert(primitive_string_equal(make_arg_list_vals(2, abcd_abcde), nil) == nil, "(string= \"abcd\" \"abcde\") はnil");
    assert(primitive_string_equal(make_arg_list_vals(2, abcde_abcd), nil) == nil, "(string= \"abcde\" \"abcd\") はnil");

    assert(primitive_string_not_equal(make_arg_list_vals(2, abcd_wxyz), nil) == g_sym_t, "(string/= \"abcd\" \"wxyz\") はT");

    assert(primitive_string_less_than(make_arg_list_vals(2, abcd_abcd), nil) == nil, "(string< \"abcd\" \"abcd\") はnil");
    assert(primitive_string_less_than(make_arg_list_vals(2, abcd_wxyz), nil) == g_sym_t, "(string< \"abcd\" \"wxyz\") はT");
    assert(primitive_string_less_than(make_arg_list_vals(2, abcd_abcde), nil) == g_sym_t, "(string< \"abcd\" \"abcde\") はT(短い方が接頭辞なら小さい)");
    assert(primitive_string_less_than(make_arg_list_vals(2, abcde_abcd), nil) == nil, "(string< \"abcde\" \"abcd\") はnil");

    assert(primitive_string_less_equal(make_arg_list_vals(2, abcd_abcd), nil) == g_sym_t, "(string<= \"abcd\" \"abcd\") はT");
    assert(primitive_string_less_equal(make_arg_list_vals(2, abcd_wxyz), nil) == g_sym_t, "(string<= \"abcd\" \"wxyz\") はT");
    assert(primitive_string_less_equal(make_arg_list_vals(2, abcd_abcde), nil) == g_sym_t, "(string<= \"abcd\" \"abcde\") はT");
    assert(primitive_string_less_equal(make_arg_list_vals(2, abcde_abcd), nil) == nil, "(string<= \"abcde\" \"abcd\") はnil");

    assert(primitive_string_greater_than(make_arg_list_vals(2, abcd_wxyz), nil) == nil, "(string> \"abcd\" \"wxyz\") はnil");
    assert(primitive_string_greater_equal(make_arg_list_vals(2, abcd_abcd), nil) == g_sym_t, "(string>= \"abcd\" \"abcd\") はT");

    // 3引数以上の隣接ペア連鎖
    lisp_val_t abc[3] = {os_make_string("a"), os_make_string("b"), os_make_string("c")};
    assert(primitive_string_less_than(make_arg_list_vals(3, abc), nil) == g_sym_t, "(string< \"a\" \"b\" \"c\") はT");
    lisp_val_t acb[3] = {os_make_string("a"), os_make_string("c"), os_make_string("b")};
    assert(primitive_string_less_than(make_arg_list_vals(3, acb), nil) == nil, "(string< \"a\" \"c\" \"b\") はnil");
}

void test_primitive_char_index() {
    lisp_val_t str = os_make_string("abcab");
    assert(primitive_char_index(os_make_cons(os_make_char('b'), os_make_cons(str, nil)), nil) >> 3 == 1,
           "(char-index #\\b \"abcab\") は1");
    assert(primitive_char_index(os_make_cons(os_make_char('B'), os_make_cons(str, nil)), nil) == nil,
           "(char-index #\\B \"abcab\") はnil(大文字小文字を区別する)");
    lisp_val_t args_with_start = os_make_cons(os_make_char('b'), os_make_cons(str, os_make_cons(os_make_fixnum(2), nil)));
    assert(primitive_char_index(args_with_start, nil) >> 3 == 4, "(char-index #\\b \"abcab\" 2) は4");
    assert(primitive_char_index(os_make_cons(os_make_char('d'), os_make_cons(str, nil)), nil) == nil,
           "(char-index #\\d \"abcab\") はnil");
    lisp_val_t args_a_from_4 = os_make_cons(os_make_char('a'), os_make_cons(str, os_make_cons(os_make_fixnum(4), nil)));
    assert(primitive_char_index(args_a_from_4, nil) == nil, "(char-index #\\a \"abcab\" 4) はnil");
}

void test_primitive_string_index() {
    lisp_val_t foobar = os_make_string("foobar");
    assert(primitive_string_index(os_make_cons(os_make_string("foo"), os_make_cons(foobar, nil)), nil) >> 3 == 0,
           "(string-index \"foo\" \"foobar\") は0");
    assert(primitive_string_index(os_make_cons(os_make_string("bar"), os_make_cons(foobar, nil)), nil) >> 3 == 3,
           "(string-index \"bar\" \"foobar\") は3");
    assert(primitive_string_index(os_make_cons(os_make_string("FOO"), os_make_cons(foobar, nil)), nil) == nil,
           "(string-index \"FOO\" \"foobar\") はnil(大文字小文字を区別する)");
    lisp_val_t foo_from_1 = os_make_cons(os_make_string("foo"), os_make_cons(foobar, os_make_cons(os_make_fixnum(1), nil)));
    assert(primitive_string_index(foo_from_1, nil) == nil, "(string-index \"foo\" \"foobar\" 1) はnil");
    lisp_val_t bar_from_1 = os_make_cons(os_make_string("bar"), os_make_cons(foobar, os_make_cons(os_make_fixnum(1), nil)));
    assert(primitive_string_index(bar_from_1, nil) >> 3 == 3, "(string-index \"bar\" \"foobar\" 1) は3");
    assert(primitive_string_index(os_make_cons(os_make_string("foo"), os_make_cons(os_make_string(""), nil)), nil) == nil,
           "(string-index \"foo\" \"\") はnil");
    assert(primitive_string_index(os_make_cons(os_make_string(""), os_make_cons(os_make_string("foo"), nil)), nil) >> 3 == 0,
           "(string-index \"\" \"foo\") は0(空文字列は即マッチ)");
}

void test_primitive_string_append() {
    lisp_val_t r1 = primitive_string_append(os_make_cons(os_make_string("abc"), os_make_cons(os_make_string("def"), nil)), nil);
    char buf[16];
    os_string_to_cstr(r1, buf, sizeof(buf));
    assert(strcmp(buf, "abcdef") == 0, "(string-append \"abc\" \"def\") は\"abcdef\"");

    lisp_val_t r2 = primitive_string_append(
        os_make_cons(os_make_string("abc"), os_make_cons(os_make_string(""), os_make_cons(os_make_string("def"), nil))), nil);
    os_string_to_cstr(r2, buf, sizeof(buf));
    assert(strcmp(buf, "abcdef") == 0, "(string-append \"abc\" \"\" \"def\") は\"abcdef\"");

    lisp_val_t r3 = primitive_string_append(nil, nil);
    assert((r3 & TAG_MASK) == TAG_STRING, "(string-append)の戻り値はTAG_STRINGを持つ");
    lisp_val_t *header3 = (lisp_val_t *)(r3 & ~TAG_MASK);
    assert(header3[0] == 0, "(string-append)は空文字列を返す");
}

void test_primitive_length() {
    lisp_val_t list = os_make_cons(os_make_fixnum(1),
                        os_make_cons(os_make_fixnum(2),
                          os_make_cons(os_make_fixnum(3), nil)));
    assert(primitive_length(os_make_cons(list, nil), nil) >> 3 == 3, "(length '(1 2 3))は3");
    assert(primitive_length(os_make_cons(nil, nil), nil) >> 3 == 0, "(length nil)は0");

    lisp_val_t str = os_make_string("hello");
    assert(primitive_length(os_make_cons(str, nil), nil) >> 3 == 5, "(length \"hello\")は5");

    lisp_val_t dims = os_make_cons(os_make_fixnum(2), os_make_cons(os_make_fixnum(3), nil));
    lisp_val_t array = primitive_make_array(os_make_cons(dims, nil), nil);
    assert(primitive_length(os_make_cons(array, nil), nil) >> 3 == 6, "(length (make-array '(2 3)))は次元に関わらず全要素数の6");
}

void test_primitive_elt() {
    lisp_val_t list = os_make_cons(os_make_symbol("A"),
                        os_make_cons(os_make_symbol("B"),
                          os_make_cons(os_make_symbol("C"), nil)));
    assert(primitive_elt(os_make_cons(list, os_make_cons(os_make_fixnum(2), nil)), nil) == os_make_symbol("C"),
           "(elt '(a b c) 2) はc");

    lisp_val_t vec_args = os_make_cons(os_make_symbol("A"),
                            os_make_cons(os_make_symbol("B"),
                              os_make_cons(os_make_symbol("C"), nil)));
    lisp_val_t vec = primitive_vector(vec_args, nil);
    assert(primitive_elt(os_make_cons(vec, os_make_cons(os_make_fixnum(1), nil)), nil) == os_make_symbol("B"),
           "(elt (vector 'a 'b 'c) 1) はb");

    lisp_val_t str = os_make_string("abc");
    assert(primitive_elt(os_make_cons(str, os_make_cons(os_make_fixnum(0), nil)), nil) == os_make_char('a'),
           "(elt \"abc\" 0) は#\\a");

    assert(primitive_elt(os_make_cons(list, os_make_cons(os_make_fixnum(5), nil)), nil) == g_sym_eval_error,
           "(elt '(a b c) 5) は範囲外なのでg_sym_eval_error");
    assert(primitive_elt(os_make_cons(str, os_make_cons(os_make_fixnum(5), nil)), nil) == g_sym_eval_error,
           "(elt \"abc\" 5) は範囲外なのでg_sym_eval_error");
    assert(primitive_elt(os_make_cons(vec, os_make_cons(os_make_fixnum(5), nil)), nil) == g_sym_eval_error,
           "(elt (vector 'a 'b 'c) 5) は範囲外なのでg_sym_eval_error");
}

void test_primitive_set_elt() {
    lisp_val_t str = primitive_create_string(os_make_cons(os_make_fixnum(5), os_make_cons(os_make_char('x'), nil)), nil);
    lisp_val_t set_str_args = os_make_cons(os_make_char('O'), os_make_cons(str, os_make_cons(os_make_fixnum(2), nil)));
    assert(primitive_set_elt(set_str_args, nil) == os_make_char('O'), "(set-elt #\\O string 2) は書き込んだ#\\Oを返す");
    char buf[8];
    os_string_to_cstr(str, buf, sizeof(buf));
    assert(strcmp(buf, "xxOxx") == 0, "set-eltで書き換えた後のstringは\"xxOxx\"");

    lisp_val_t list = os_make_cons(os_make_fixnum(1),
                        os_make_cons(os_make_fixnum(2),
                          os_make_cons(os_make_fixnum(3), nil)));
    lisp_val_t set_list_args = os_make_cons(os_make_fixnum(99), os_make_cons(list, os_make_cons(os_make_fixnum(1), nil)));
    assert(primitive_set_elt(set_list_args, nil) >> 3 == 99, "(set-elt 99 list 1) は書き込んだ99を返す");
    assert(primitive_elt(os_make_cons(list, os_make_cons(os_make_fixnum(1), nil)), nil) >> 3 == 99,
           "set-eltで書き換えた後のlistの1番目は99");

    lisp_val_t vec = primitive_create_vector(os_make_cons(os_make_fixnum(3), nil), nil);
    lisp_val_t set_vec_args = os_make_cons(os_make_fixnum(42), os_make_cons(vec, os_make_cons(os_make_fixnum(1), nil)));
    assert(primitive_set_elt(set_vec_args, nil) >> 3 == 42, "(set-elt 42 vec 1) は書き込んだ42を返す");
    assert(primitive_elt(os_make_cons(vec, os_make_cons(os_make_fixnum(1), nil)), nil) >> 3 == 42,
           "set-eltで書き換えた後のvecの1番目は42");

    lisp_val_t out_of_range_args = os_make_cons(os_make_fixnum(1), os_make_cons(list, os_make_cons(os_make_fixnum(5), nil)));
    assert(primitive_set_elt(out_of_range_args, nil) == g_sym_eval_error, "(set-elt 1 list 5) は範囲外なのでg_sym_eval_error");
}

void test_primitive_subseq() {
    lisp_val_t str_result = primitive_subseq(
        os_make_cons(os_make_string("abcdef"), os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(4), nil))), nil);
    char buf[8];
    os_string_to_cstr(str_result, buf, sizeof(buf));
    assert(strcmp(buf, "bcd") == 0, "(subseq \"abcdef\" 1 4) は\"bcd\"");

    lisp_val_t list = os_make_cons(os_make_symbol("A"),
                        os_make_cons(os_make_symbol("B"),
                          os_make_cons(os_make_symbol("C"),
                            os_make_cons(os_make_symbol("D"),
                              os_make_cons(os_make_symbol("E"),
                                os_make_cons(os_make_symbol("F"), nil))))));
    lisp_val_t list_result = primitive_subseq(
        os_make_cons(list, os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(4), nil))), nil);
    assert(primitive_elt(os_make_cons(list_result, os_make_cons(os_make_fixnum(0), nil)), nil) == os_make_symbol("B"),
           "(subseq '(a b c d e f) 1 4) の0番目はb");
    assert(primitive_elt(os_make_cons(list_result, os_make_cons(os_make_fixnum(1), nil)), nil) == os_make_symbol("C"),
           "(subseq '(a b c d e f) 1 4) の1番目はc");
    assert(primitive_elt(os_make_cons(list_result, os_make_cons(os_make_fixnum(2), nil)), nil) == os_make_symbol("D"),
           "(subseq '(a b c d e f) 1 4) の2番目はd");
    assert(primitive_length(os_make_cons(list_result, nil), nil) >> 3 == 3, "(subseq '(a b c d e f) 1 4) の長さは3");

    lisp_val_t vec = primitive_vector(list, nil);
    lisp_val_t vec_result = primitive_subseq(
        os_make_cons(vec, os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(4), nil))), nil);
    assert((vec_result & TAG_MASK) == TAG_INSTANCE, "(subseq (vector ...) 1 4) の戻り値はTAG_INSTANCEを持つ(VECTOR)");
    assert(primitive_length(os_make_cons(vec_result, nil), nil) >> 3 == 3, "(subseq (vector 'a 'b 'c 'd 'e 'f) 1 4) の長さは3");
    assert(primitive_elt(os_make_cons(vec_result, os_make_cons(os_make_fixnum(0), nil)), nil) == os_make_symbol("B"),
           "(subseq (vector 'a 'b 'c 'd 'e 'f) 1 4) の0番目はb");
    assert(primitive_elt(os_make_cons(vec_result, os_make_cons(os_make_fixnum(2), nil)), nil) == os_make_symbol("D"),
           "(subseq (vector 'a 'b 'c 'd 'e 'f) 1 4) の2番目はd");
}

void test_primitive_make_class_raw_and_accessors() {
    lisp_val_t name = os_make_symbol("POINT");
    lisp_val_t supers = nil;
    lisp_val_t slots = os_make_cons(os_make_symbol("X"), nil);

    lisp_val_t class = primitive_make_class_raw(
        os_make_cons(name, os_make_cons(supers, os_make_cons(slots, nil))), nil);
    assert((class & TAG_MASK) == TAG_INSTANCE, "(%%make-class-raw ...)の戻り値はTAG_INSTANCEを持つ");

    UINT64 *obj = (UINT64 *)(class & ~TAG_MASK);
    assert(obj[0] == MAGIC_STANDARD_CLASS, "word0はMAGIC_STANDARD_CLASS");

    assert(primitive_class_name(os_make_cons(class, nil), nil) == name, "(%%class-name c)は渡したnameをそのまま返す");
    assert(primitive_class_supers(os_make_cons(class, nil), nil) == supers, "(%%class-supers c)は渡したsupersをそのまま返す");
    assert(primitive_class_slots(os_make_cons(class, nil), nil) == slots, "(%%class-slots c)は渡したslotsをそのまま返す");

    assert(primitive_classp(os_make_cons(class, nil), nil) == g_sym_t, "(%%classp c)はtを返す");
    assert(primitive_classp(os_make_cons(os_make_fixnum(1), nil), nil) == nil, "(%%classp 1)はnilを返す");

    assert(primitive_standard_classp(os_make_cons(class, nil), nil) == g_sym_t, "(%%standard-classp c)はtを返す");
    assert(primitive_builtin_classp(os_make_cons(class, nil), nil) == nil, "(%%builtin-classp c)はnilを返す(標準クラスなので)");
}

void test_primitive_make_builtin_class_raw_and_metaclass_predicates() {
    lisp_val_t name = os_make_symbol("<INTEGER>");
    lisp_val_t supers = nil;
    lisp_val_t slots = nil;

    lisp_val_t class = primitive_make_builtin_class_raw(
        os_make_cons(name, os_make_cons(supers, os_make_cons(slots, nil))), nil);
    assert((class & TAG_MASK) == TAG_INSTANCE, "(%%make-builtin-class-raw ...)の戻り値はTAG_INSTANCEを持つ");

    UINT64 *obj = (UINT64 *)(class & ~TAG_MASK);
    assert(obj[0] == MAGIC_BUILTIN_CLASS, "word0はMAGIC_BUILTIN_CLASS");

    assert(primitive_class_name(os_make_cons(class, nil), nil) == name, "(%%class-name c)は渡したnameをそのまま返す");

    assert(primitive_classp(os_make_cons(class, nil), nil) == g_sym_t, "(%%classp c)は組み込みクラスでもtを返す");
    assert(primitive_builtin_classp(os_make_cons(class, nil), nil) == g_sym_t, "(%%builtin-classp c)はtを返す");
    assert(primitive_standard_classp(os_make_cons(class, nil), nil) == nil, "(%%standard-classp c)はnilを返す(組み込みクラスなので)");
    assert(primitive_builtin_classp(os_make_cons(os_make_fixnum(1), nil), nil) == nil, "(%%builtin-classp 1)はnilを返す");
    assert(primitive_standard_classp(os_make_cons(os_make_fixnum(1), nil), nil) == nil, "(%%standard-classp 1)はnilを返す");
}

void test_primitive_make_instance_raw_and_accessors() {
    lisp_val_t class = os_make_instance(MAGIC_STANDARD_CLASS, os_make_symbol("POINT"), nil, nil);
    lisp_val_t slots_vector = primitive_make_array(os_make_cons(os_make_fixnum(1), nil), nil);

    lisp_val_t instance = primitive_make_instance_raw(
        os_make_cons(class, os_make_cons(slots_vector, nil)), nil);
    assert((instance & TAG_MASK) == TAG_INSTANCE, "(%%make-instance-raw ...)の戻り値はTAG_INSTANCEを持つ");

    UINT64 *obj = (UINT64 *)(instance & ~TAG_MASK);
    assert(obj[0] == MAGIC_CLASS_INSTANCE, "word0はMAGIC_CLASS_INSTANCE");

    assert(primitive_instance_class(os_make_cons(instance, nil), nil) == class, "(%%instance-class i)は渡したclassをそのまま返す");
    assert(primitive_instance_slots(os_make_cons(instance, nil), nil) == slots_vector, "(%%instance-slots i)は渡したslots-vectorをそのまま返す");

    assert(primitive_class_instance_p(os_make_cons(instance, nil), nil) == g_sym_t, "(%%class-instance-p i)はtを返す");
    assert(primitive_class_instance_p(os_make_cons(class, nil), nil) == nil, "(%%class-instance-p c)(クラス自身)はnilを返す");
}

void test_imm_page_alloc_survives_gc_and_free_list_reuses_page() {
    UINT8 *page = (UINT8 *)os_imm_page_alloc();
    for (int i = 0; i < IMM_PAGE_SIZE; i++) {
        page[i] = (UINT8)(i & 0xFF);
    }

    // From/To空間側にゴミを積んでGCが実際に走る状況を作る
    lisp_val_t garbage = nil;
    for (int i = 0; i < 100; i++) {
        garbage = os_make_cons(os_make_fixnum(i), garbage);
    }
    os_gc_collect();

    for (int i = 0; i < IMM_PAGE_SIZE; i++) {
        assert(page[i] == (UINT8)(i & 0xFF),
               "Immobilized Spaceのページはos_gc_collectを挟んでも移動・破棄されない");
    }

    os_imm_page_free(page);
    UINT8 *reused = (UINT8 *)os_imm_page_alloc();
    assert(reused == page, "os_imm_page_freeで返却したページはos_imm_page_allocで再利用される");
}

void test_imm_slot_alloc_carves_aligned_slots_from_pages() {
    imm_slot_cursor_t cursor = {0, 0};
    void *a = os_imm_slot_alloc(&cursor, 8);
    void *b = os_imm_slot_alloc(&cursor, 8);
    assert((UINT64)a % 16 == 0, "os_imm_slot_allocが返すアドレスは16byteアライメント");
    assert((UINT64)b % 16 == 0, "os_imm_slot_allocが返すアドレスは16byteアライメント");
    assert(b == (UINT8 *)a + 16, "8byte要求でも16byteアライメントに切り上げられ、次のスロットは16byte先になる");
    assert(cursor.page != 0, "カーソルはos_imm_page_allocで確保したページを保持する");
}

// Phase3動作確認: os_imm_pages_alloc_contiguousが返す複数ページは物理的に連続しており、
// (フリーリストを経由しない)続く単ページ確保はその直後から始まることを確認する。
void test_imm_pages_alloc_contiguous_returns_physically_contiguous_pages() {
    UINT8 *dest = (UINT8 *)os_imm_pages_alloc_contiguous(3);
    assert(dest != 0, "3ページの確保が成功する");

    UINT8 *next = (UINT8 *)os_imm_page_alloc();
    assert(next == dest + 3 * IMM_PAGE_SIZE,
           "3ページ分は物理的に連続しており、続く単ページ確保はその直後から始まる");
}

// Phase3動作確認: 空き容量を大きく超える要求は、os_imm_page_allocのようにハードハルトせず
// NULLを返し、かつbump領域を消費しないことを確認する。
void test_imm_pages_alloc_contiguous_returns_null_when_request_exceeds_space() {
    UINT8 *before = (UINT8 *)os_imm_page_alloc();

    void *huge = os_imm_pages_alloc_contiguous(1000000);
    assert(huge == 0, "空き容量を超える要求はハードハルトせずNULLを返す");

    UINT8 *after = (UINT8 *)os_imm_page_alloc();
    assert(after == before + IMM_PAGE_SIZE,
           "失敗した確保はbump領域を消費しない(続く単ページ確保は直前の続きから始まる)");
}

// Phase3動作確認: 環境の7番目のslot「pages」に登録したImmobilized Pageが、
// os_environment_reclaim_pagesでos_imm_page_freeへ返却され、フリーリスト経由で
// os_imm_page_allocから再利用可能になることを確認する。
void test_os_environment_register_and_reclaim_pages() {
    lisp_val_t env = os_make_environment(os_make_symbol("PAGES-TEST-ENV"), nil);
    void *dest = os_imm_pages_alloc_contiguous(2);
    assert(dest != 0, "2ページの確保が成功する");

    os_environment_register_pages(env, dest, 2);
    os_environment_reclaim_pages(env);

    void *r1 = os_imm_page_alloc();
    void *r2 = os_imm_page_alloc();
    UINT8 *page0 = (UINT8 *)dest;
    UINT8 *page1 = (UINT8 *)dest + IMM_PAGE_SIZE;
    assert((r1 == page0 || r1 == page1), "回収した1ページ目がフリーリスト経由で再利用される");
    assert((r2 == page0 || r2 == page1) && r2 != r1, "回収した2ページ目もフリーリスト経由で再利用される");
}

// Phase3.6動作確認: os_gc_unregister_rootで登録を外したrootはGCのスキャン対象から外れ
// (アドレスが更新されない)、他の登録済みroot(swap-remove後に配列末尾から詰め替わった
// 要素を含む)は影響を受けず正しく追跡され続けることを確認する。
void test_os_gc_unregister_root_removes_only_target_and_keeps_others_tracked() {
    lisp_val_t a = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
    lisp_val_t b = os_make_cons(os_make_fixnum(3), os_make_fixnum(4));
    lisp_val_t c = os_make_cons(os_make_fixnum(5), os_make_fixnum(6));
    os_gc_register_root(&a);
    os_gc_register_root(&b);
    os_gc_register_root(&c);

    lisp_addr_t b_before = b & ~TAG_MASK;

    os_gc_unregister_root(&b);

    os_gc_collect();

    assert((a & TAG_MASK) == TAG_CONS, "登録を外していないaはGC後もconsのまま");
    assert((c & TAG_MASK) == TAG_CONS, "登録を外していないcはGC後もconsのまま");
    assert(cc_car(a) == os_make_fixnum(1), "登録を外していないaはGC後も正しい値を保持する(swap-removeで巻き込まれない)");
    assert(cc_car(c) == os_make_fixnum(5), "登録を外していないcはGC後も正しい値を保持する(swap-removeで巻き込まれない)");
    assert((b & ~TAG_MASK) == b_before,
           "登録解除したbはGCでアドレスが更新されない(rootとして追跡されなくなった)");

    os_gc_unregister_root(&a);
    os_gc_unregister_root(&c);
}

// Phase3.6動作確認: 環境の8番目のslot「literal-slots」に登録したアドレスが、
// os_environment_reclaim_literal_slotsで登録した順にコールバックへ渡され、
// 呼び出し後にスロットがnilへ戻ることを確認する(za.c側のza_free_literal_slotに
// 相当する処理を、テスト側の記録用コールバックで代替して検証する)。
static lisp_val_t *g_literal_slot_reclaim_seen[8];
static UINT64 g_literal_slot_reclaim_seen_count = 0;
static void test_literal_slot_reclaim_callback(lisp_val_t *slot_addr) {
    g_literal_slot_reclaim_seen[g_literal_slot_reclaim_seen_count++] = slot_addr;
}
void test_os_environment_register_and_reclaim_literal_slots() {
    lisp_val_t env = os_make_environment(os_make_symbol("LITERAL-SLOTS-TEST-ENV"), nil);
    lisp_val_t fake_slot_a = os_make_fixnum(111);
    lisp_val_t fake_slot_b = os_make_fixnum(222);

    g_literal_slot_reclaim_seen_count = 0;
    os_environment_register_literal_slot(env, &fake_slot_a);
    os_environment_register_literal_slot(env, &fake_slot_b);
    os_environment_reclaim_literal_slots(env, test_literal_slot_reclaim_callback);

    assert(g_literal_slot_reclaim_seen_count == 2, "登録した2件それぞれについてコールバックが呼ばれる");
    assert((g_literal_slot_reclaim_seen[0] == &fake_slot_a || g_literal_slot_reclaim_seen[0] == &fake_slot_b),
           "コールバックに渡されるのは登録したアドレスのいずれか");
    assert((g_literal_slot_reclaim_seen[1] == &fake_slot_a || g_literal_slot_reclaim_seen[1] == &fake_slot_b),
           "コールバックに渡されるのは登録したアドレスのいずれか");
    assert(g_literal_slot_reclaim_seen[0] != g_literal_slot_reclaim_seen[1], "同じアドレスが重複して渡されない");

    // 回収後、literal-slotsスロットは空リスト(nil)へ戻っており、再度reclaimしても
    // コールバックは呼ばれない。
    g_literal_slot_reclaim_seen_count = 0;
    os_environment_reclaim_literal_slots(env, test_literal_slot_reclaim_callback);
    assert(g_literal_slot_reclaim_seen_count == 0, "回収後のliteral-slotsスロットは空になっている");
}

void test_gc_cons_survives_and_relocates() {
    // symをos_gc_collect()を挟んで直接使うと、sym自身がFrom空間の古いアドレスのまま
    // 更新されない(rootとして登録していないローカル変数はGCが書き換えてくれない)ため、
    // 以後の検索はGC後にos_make_symbolで同名をintern済みsymbolを取り直す必要がある
    // (これはGCが安全なのはセーフポイントに限られるという設計そのものの反映であり、
    // os_repl_stepが毎回formを読み直して新しいsymbolを得る動作と対応する)
    lisp_val_t sym = os_make_symbol("GC-TEST-CONS");
    lisp_val_t car = os_make_fixnum(111);
    lisp_val_t cdr = os_make_fixnum(222);
    lisp_val_t cons = os_make_cons(car, cdr);
    os_set_variable(sym, cons, global_environment);
    lisp_addr_t addr_before = cons & ~TAG_MASK;

    os_gc_collect();

    lisp_val_t after = os_get_variable(os_make_symbol("GC-TEST-CONS"), global_environment);
    assert((after & TAG_MASK) == TAG_CONS, "GCを跨いでもconsのタグはTAG_CONSのまま");
    assert((after & ~TAG_MASK) != addr_before, "global_environmentから参照されるconsはGCでTo空間の新しいアドレスへ再配置される");
    assert(cc_car(after) == car, "GCを跨いでもconsのcar(fixnum)の値は保たれる");
    assert(cc_cdr(after) == cdr, "GCを跨いでもconsのcdr(fixnum)の値は保たれる");
}

void test_gc_symbol_survives_via_symbol_table() {
    lisp_val_t sym = os_make_symbol("GC-TEST-SYMBOL");
    lisp_addr_t addr_before = sym & ~TAG_MASK;

    os_gc_collect();

    lisp_val_t sym_after = os_make_symbol("GC-TEST-SYMBOL");
    assert((sym_after & TAG_MASK) == TAG_SYMBOL, "GCを跨いでもsymbolのタグはTAG_SYMBOLのまま");
    assert((sym_after & ~TAG_MASK) != addr_before, "g_symbol_tableに登録済みのsymbolはGCでTo空間の新しいアドレスへ再配置される");

    char buf[16];
    os_string_to_cstr(primitive_symbol_name(os_make_cons(sym_after, nil), nil), buf, sizeof(buf));
    assert(strncmp(buf, "GC-TEST-SYMBOL", 14) == 0, "GCを跨いでもsymbol名の内容は保たれる");
}

void test_gc_string_with_forward_tag_colliding_length_is_not_misdetected() {
    // 長さ6の文字列はword0(=6)の下位3bitがTAG_FORWARD(6)と一致してしまう。
    // gc_copy_valueがTo空間の範囲外であることを見て誤検知を回避できているかを確認する
    lisp_val_t sym = os_make_symbol("GC-TEST-STRING-LEN6");
    lisp_val_t str = os_make_string("abcdef");
    UINT64 *header = (UINT64 *)(str & ~TAG_MASK);
    assert((header[0] & TAG_MASK) == TAG_FORWARD, "長さ6の文字列はword0の下位3bitがTAG_FORWARDと一致する(前提条件)");
    os_set_variable(sym, str, global_environment);
    lisp_addr_t addr_before = str & ~TAG_MASK;

    os_gc_collect();

    // symはGC前のFrom空間アドレスのままなので、検索にはGC後に取り直したsymbolを使う
    lisp_val_t after = os_get_variable(os_make_symbol("GC-TEST-STRING-LEN6"), global_environment);
    assert((after & TAG_MASK) == TAG_STRING, "誤検知が起きてもGC後のタグはTAG_STRINGのまま");
    assert((after & ~TAG_MASK) != addr_before, "誤検知が起きても文字列はTo空間へ再配置される");

    char buf[16];
    os_string_to_cstr(after, buf, sizeof(buf));
    assert(strncmp(buf, "abcdef", 6) == 0, "誤検知が起きても文字列の内容は破壊されず保たれる");
}

void test_gc_instance_survives() {
    lisp_val_t sym = os_make_symbol("GC-TEST-FLOAT-INSTANCE");
    lisp_val_t f = os_make_float(3.5);
    os_set_variable(sym, f, global_environment);
    lisp_addr_t addr_before = f & ~TAG_MASK;

    os_gc_collect();

    // symはGC前のFrom空間アドレスのままなので、検索にはGC後に取り直したsymbolを使う
    lisp_val_t after = os_get_variable(os_make_symbol("GC-TEST-FLOAT-INSTANCE"), global_environment);
    assert((after & TAG_MASK) == TAG_INSTANCE, "GCを跨いでもfloat instanceのタグはTAG_INSTANCEのまま");
    assert((after & ~TAG_MASK) != addr_before, "instanceはGCでTo空間の新しいアドレスへ再配置される");
    assert(os_float_value(after) == 3.5, "GCを跨いでもfloatの値は保たれる");
}

void test_gc_circular_cons_list_does_not_hang() {
    lisp_val_t sym = os_make_symbol("GC-TEST-CIRCULAR-LIST");
    lisp_val_t head = os_make_cons(os_make_fixnum(1), nil);
    lisp_val_t second = os_make_cons(os_make_fixnum(2), head); // headを自己参照させて循環させる
    cc_set_cdr(head, second);
    os_set_variable(sym, head, global_environment);

    os_gc_collect(); // 循環参照があってもワークキュー方式のため無限ループ/クラッシュしない

    // symはGC前のFrom空間アドレスのままなので、検索にはGC後に取り直したsymbolを使う
    lisp_val_t after_head = os_get_variable(os_make_symbol("GC-TEST-CIRCULAR-LIST"), global_environment);
    assert(cc_car(after_head) == os_make_fixnum(1), "GC後も循環リストの先頭要素の値は保たれる");
    lisp_val_t after_second = cc_cdr(after_head);
    assert(cc_car(after_second) == os_make_fixnum(2), "GC後も循環リストの2番目の要素の値は保たれる");
    assert(cc_cdr(after_second) == after_head, "GC後も循環リストの循環構造(2番目のcdrが先頭を指す)は保たれる");
}

void test_gc_reclaims_unreferenced_garbage() {
    os_gc_collect(); // まず一度回収し、以降の使用率比較の基準を揃える
    double ratio_baseline = os_heap_used_ratio();

    // どこにも束縛せず、ローカル変数にも残さない大量のconsを生成してFrom空間を消費させる
    for (int i = 0; i < 5000; i++) {
        os_make_cons(os_make_fixnum(i), os_make_fixnum(i + 1));
    }
    double ratio_before_gc = os_heap_used_ratio();
    assert(ratio_before_gc > ratio_baseline, "参照されないconsを大量に作るとFrom空間の使用率が上がる");

    os_gc_collect();
    double ratio_after_gc = os_heap_used_ratio();
    assert(ratio_after_gc < ratio_before_gc, "参照されなくなったconsはGCで回収され、使用率が下がる");
}

// ============================== os_alloc_bytesのOOM時にos_gc_collectが実際に
// 発火するケースの検証 ==============================
// 以降のテストは意図的に小さいヒープを使い、計算の途中でos_alloc_bytesがOOMを検出して
// os_gc_collectを呼ぶ状況を実際に作り、GCが発火しても結果が正しいことを確認する
// (documents/isiki-os.mdの「今後の課題」チェックリストに対応する検証)。
// setup_small_heapはos_reset_runtime_state_for_testでglobal_environment/symbol table等の
// 全状態をリセットするため、このセクションのテストはmain()の最後にまとめて実行する。

// %%ZA-COMPILED-Pの追加でos_bootstrap()が恒久的に消費するバイト数が増えたため、
// 元の64KBのままだとbignum加算テストがGC後も本当に確保不能になってしまう。
// GCが計算途中で発火するというテストの意図(margin)を保ったまま、その分だけ広げる
//
// Function Cell導入(os_set_function参照)により、os_bootstrapが登録する
// native関数1つにつきcellsスロットへ2つの新規cons(new_cell_pair/new_cells_alist、
// 計32byte)が恒久的にglobal_environmentから到達可能になった。os_bootstrapが
// 登録するnative関数は100個超あり、合計で3KB超の恒久消費が増える。68KBのままだと
// vector構築テスト(N=750)がGC後も確保不能になり停止するため、その分さらに広げる
#define SMALL_HEAP_SIZE (76 * 1024)

static void setup_small_heap(void) {
    void *heap = malloc(SMALL_HEAP_SIZE);
    assert(heap != NULL, "GC発火テスト用の小さいヒープをmallocで確保できる");
    os_heap_init((UINT64)heap, SMALL_HEAP_SIZE);
    os_reset_runtime_state_for_test();
    os_bootstrap();
}

void test_gc_fires_during_bignum_addition_and_result_is_correct() {
    setup_small_heap();

    // bignumのlimbは32bit単位(UINT64のスロットに32bit値を1つ格納する、base 2^32)。
    // 各項は2^(32*24)-1(24limbの全bit立て)。80項の総和は25limbのbignumになる
    UINT64 term_limbs[24];
    for (int i = 0; i < 24; i++) {
        term_limbs[i] = 0xFFFFFFFFULL;
    }
    lisp_val_t term = os_make_integer(0, term_limbs, 24);
    GC_PROTECT(term);

    lisp_val_t args = nil;
    GC_PROTECT(args);
    for (int i = 0; i < 80; i++) {
        args = os_make_cons(term, args);
    }

    UINT64 gc_count_before = os_gc_collect_count();
    lisp_val_t result = primitive_add(args, nil);
    UINT64 gc_count_after = os_gc_collect_count();
    assert(gc_count_after > gc_count_before,
           "64KBの小さいヒープで24limb×80項のbignum加算を行うと、計算の途中で実際にos_gc_collectが発火する");

    // Pythonで事前計算した((2^(32*24)-1) * 80)の期待値(25limb、32bit単位)
    UINT64 expected_limbs[25] = {
        0xffffffb0ULL, 0xffffffffULL, 0xffffffffULL, 0xffffffffULL,
        0xffffffffULL, 0xffffffffULL, 0xffffffffULL, 0xffffffffULL,
        0xffffffffULL, 0xffffffffULL, 0xffffffffULL, 0xffffffffULL,
        0xffffffffULL, 0xffffffffULL, 0xffffffffULL, 0xffffffffULL,
        0xffffffffULL, 0xffffffffULL, 0xffffffffULL, 0xffffffffULL,
        0xffffffffULL, 0xffffffffULL, 0xffffffffULL, 0xffffffffULL,
        0x0000004fULL
    };
    lisp_val_t expected = os_make_integer(0, expected_limbs, 25);
    assert(primitive_num_equal(os_make_cons(result, os_make_cons(expected, nil)), nil) == g_sym_t,
           "GC発火を挟んでも24limb×80項のbignum加算の結果は正しい");
}

void test_gc_fires_during_gcd_and_isqrt_and_results_are_correct() {
    setup_small_heap();

    // 連続する2つのフィボナッチ数(fib(600), fib(601))。互いに素(gcd=1)であり、
    // ユークリッドの互除法(mag_gcd)が多数回イテレーションする典型的な入力になる
    // (bignumのlimbは32bit単位。base 2^32でのlittle-endian表現)
    UINT64 a_limbs[13] = {
        0xcd62b020ULL, 0x1248e07eULL, 0xcf00478dULL, 0x312be5a8ULL, 0x99e1dfc1ULL, 0x934a4cc4ULL,
        0xbd946c67ULL, 0x33de6b20ULL, 0x44d1de10ULL, 0xcd7e1dd4ULL, 0xb54f58f7ULL, 0x4da336b7ULL,
        0xa70e38bcULL
    };
    UINT64 b_limbs[14] = {
        0x32c9ba91ULL, 0xb696f95bULL, 0xc2fd6bf8ULL, 0xa2b6cbd2ULL, 0xa345e1daULL, 0xe0322ba5ULL,
        0xb4501669ULL, 0xcc00edbaULL, 0x48d27415ULL, 0xa0dcc8f1ULL, 0x200eb5d0ULL, 0x7b2b4a8fULL,
        0x0e4d333dULL, 0x00000001ULL
    };
    lisp_val_t a = os_make_integer(0, a_limbs, 13);
    GC_PROTECT(a);
    lisp_val_t b = os_make_integer(0, b_limbs, 14);
    GC_PROTECT(b);

    UINT64 gc_count_before = os_gc_collect_count();
    lisp_val_t gcd_result = primitive_gcd(os_make_cons(a, os_make_cons(b, nil)), nil);
    UINT64 gc_count_after = os_gc_collect_count();
    assert(gc_count_after > gc_count_before,
           "小さいヒープで連続するフィボナッチ数のgcdを計算すると、mag_gcdのループ中に実際にos_gc_collectが発火する");
    assert(gcd_result == os_make_fixnum(1), "連続するフィボナッチ数のgcdは1");

    // Q = 2^100 + 12345678901234567890、Q2 = Q*Qの完全平方数でmag_isqrtを検証する
    // (bignumのlimbは32bit単位)
    UINT64 q_limbs[4] = { 0xeb1f0ad2ULL, 0xab54a98cULL, 0x00000000ULL, 0x00000010ULL };
    UINT64 q2_limbs[7] = {
        0x2b511444ULL, 0xa3ff1b51ULL, 0xf6e120d4ULL, 0xd68b90c1ULL, 0x6a95319dULL, 0x00000015ULL,
        0x00000100ULL
    };
    lisp_val_t q = os_make_integer(0, q_limbs, 4);
    GC_PROTECT(q);
    lisp_val_t q2 = os_make_integer(0, q2_limbs, 7);
    GC_PROTECT(q2);

    UINT64 gc_count_before2 = os_gc_collect_count();
    lisp_val_t isqrt_result = primitive_isqrt(os_make_cons(q2, nil), nil);
    UINT64 gc_count_after2 = os_gc_collect_count();
    assert(gc_count_after2 > gc_count_before2,
           "小さいヒープでbignumのisqrtを計算すると、mag_isqrtのループ中に実際にos_gc_collectが発火する");
    assert(primitive_num_equal(os_make_cons(isqrt_result, os_make_cons(q, nil)), nil) == g_sym_t,
           "GC発火を挟んでもisqrt(Q^2)はQに戻る(bignumの完全平方数)");
}

void test_gc_fires_during_vector_construction_and_elements_are_preserved() {
    setup_small_heap();

    #define GC_VECTOR_TEST_N 750
    UINT64 gc_count_before = os_gc_collect_count();

    // listはvec構築後は不要になる。検証ループの分の空きを確保するため、
    // GC_PROTECT(list)のスコープをこのブロック内に限定し、vec構築後は
    // listがGCのルートから外れるようにする
    lisp_val_t vec = nil;
    GC_PROTECT(vec);
    {
        lisp_val_t list = nil;
        GC_PROTECT(list);
        for (int i = 0; i < GC_VECTOR_TEST_N; i++) {
            // 到達不能な使い捨てのconsを混ぜてガベージを作り、GCが実際に回収する対象がある状態にする
            os_make_cons(os_make_fixnum((UINT64)i), nil);
            list = os_make_cons(os_make_fixnum((UINT64)i), list);
        }
        // consで逆順に積んだので、listの先頭からはN-1, N-2, ..., 0の順になる
        vec = primitive_vector(list, nil);
    }
    UINT64 gc_count_after = os_gc_collect_count();
    assert(gc_count_after > gc_count_before,
           "小さいヒープで使い捨てのconsを混ぜながら長いリストからvectorを構築すると、構築の途中で実際にos_gc_collectが発火する");

    lisp_val_t *header = os_vector_header(vec);
    assert(header[1] == GC_VECTOR_TEST_N, "GC発火を挟んでも構築したvectorの長さはNのまま");

    for (int i = 0; i < GC_VECTOR_TEST_N; i++) {
        lisp_val_t aref_args = os_make_cons(vec, os_make_cons(os_make_fixnum((UINT64)i), nil));
        UINT64 expected = (UINT64)(GC_VECTOR_TEST_N - 1 - i);
        assert(primitive_aref(aref_args, nil) == os_make_fixnum(expected),
               "GC発火を挟んでもvectorの各要素はリストの内容通りに保持される");
    }
    #undef GC_VECTOR_TEST_N
}

void test_gc_fires_during_string_append_and_result_is_correct() {
    setup_small_heap();

    #define GC_STRING_TEST_N 450
    UINT64 gc_count_before = os_gc_collect_count();

    lisp_val_t list = nil;
    GC_PROTECT(list);
    for (int i = 0; i < GC_STRING_TEST_N; i++) {
        char piece[2] = { (char)('0' + (i % 10)), '\0' };
        // 到達不能な使い捨てのstring+consを混ぜてガベージを作り、GCが実際に回収する対象がある状態にする
        os_make_cons(os_make_string(piece), nil);
        lisp_val_t s = os_make_string(piece);
        list = os_make_cons(s, list);
    }
    // consで逆順に積んだので、連結結果は生成順(0,1,2,...)を逆にした数字の並びになる

    lisp_val_t result = primitive_string_append(list, nil);
    UINT64 gc_count_after = os_gc_collect_count();
    assert(gc_count_after > gc_count_before,
           "小さいヒープで使い捨てのstringを混ぜながら多数の文字列を連結すると、入力構築を含めた計算の途中で実際にos_gc_collectが発火する");

    char expected[GC_STRING_TEST_N + 1];
    for (int i = 0; i < GC_STRING_TEST_N; i++) {
        expected[i] = (char)('0' + ((GC_STRING_TEST_N - 1 - i) % 10));
    }
    expected[GC_STRING_TEST_N] = '\0';

    char buf[GC_STRING_TEST_N + 1];
    os_string_to_cstr(result, buf, sizeof(buf));
    assert(strcmp(buf, expected) == 0, "GC発火を挟んでもprimitive_string_appendの連結結果は正しい");
    #undef GC_STRING_TEST_N
}

int main(int argc, char** argv) {
   (void)argc;
   (void)argv;
   test_os_make_fixnum();

   setup_heap();
   test_os_make_cons();
   test_os_make_char();
   test_os_make_string();
   test_os_make_symbol();
   test_os_make_symbol_prefix_is_not_confused();
   test_os_get_variable();
   test_os_get_function();
   test_primitive_global_environment_returns_global_environment_regardless_of_caller_env();
   test_primitive_set_current_environment_returns_t_or_nil_and_rejects_invalid_env();
   test_os_function_cell();
   test_os_make_fixnum_signed();
   test_os_make_integer_promotes_to_bignum();
   test_primitive_add_signed_and_bignum();
   test_primitive_subtract_unary_and_signed();
   test_primitive_multiply_signed_and_bignum();
   test_primitive_divide_signed();
   test_primitive_arithmetic_with_float();
   test_primitive_multiply();
   test_primitive_divide();
   test_primitive_less_than();
   test_primitive_greater_than();
   test_primitive_num_equal();
   test_primitive_comparisons_signed_and_bignum();
   test_primitive_num_not_equal_ge_le();
   test_primitive_max_min_abs();
   test_primitive_div_mod();
   test_primitive_gcd_lcm();
   test_primitive_isqrt();
   test_primitive_numberp_and_fixnump();
   test_primitive_bignump();
   test_primitive_floatp_and_float();
   test_primitive_sqrt();
   test_primitive_log();
   test_primitive_exp_sin_cos_atan2();
   test_primitive_floor_ceiling_truncate_round();
   test_primitive_parse_number();
   test_primitive_symbolp();
   test_primitive_consp();
   test_primitive_eql();
   test_primitive_eql_float();
   test_primitive_equal();
   test_primitive_eql_and_equal_bignum();
   test_primitive_listp();
   test_primitive_characterp();
   test_primitive_char_comparisons();
   test_primitive_stringp();
   test_primitive_functionp();
   test_primitive_generic_function_p();
   test_primitive_array_and_vector_predicates();
   test_primitive_streamp();
   test_primitive_symbol_name();
   test_primitive_string_to_symbol();
   test_primitive_gensym();
   test_primitive_make_array_1d();
   test_primitive_make_array_multi_dim();
   test_primitive_array_dimensions();
   test_primitive_array_dimensions_on_string();
   test_primitive_aref_reads_back_value();
   test_primitive_aref_out_of_bounds();
   test_primitive_vector();
   test_primitive_create_vector();
   test_primitive_garef_set_garef();
   test_primitive_set_car();
   test_primitive_set_cdr();
   test_primitive_set_aref_out_of_bounds();
   test_primitive_create_string_default_fill();
   test_primitive_create_string_with_char();
   test_primitive_string_elt();
   test_primitive_string_elt_out_of_bounds();
   test_primitive_string_comparisons();
   test_primitive_char_index();
   test_primitive_string_index();
   test_primitive_string_append();
   test_primitive_length();
   test_primitive_elt();
   test_primitive_set_elt();
   test_primitive_subseq();
   test_primitive_make_class_raw_and_accessors();
   test_primitive_make_builtin_class_raw_and_metaclass_predicates();
   test_primitive_make_instance_raw_and_accessors();

   test_imm_page_alloc_survives_gc_and_free_list_reuses_page();
   test_imm_slot_alloc_carves_aligned_slots_from_pages();
   test_imm_pages_alloc_contiguous_returns_physically_contiguous_pages();
   test_imm_pages_alloc_contiguous_returns_null_when_request_exceeds_space();
   test_os_environment_register_and_reclaim_pages();
   test_os_gc_unregister_root_removes_only_target_and_keeps_others_tracked();
   test_os_environment_register_and_reclaim_literal_slots();

   test_gc_cons_survives_and_relocates();
   test_gc_symbol_survives_via_symbol_table();
   test_gc_string_with_forward_tag_colliding_length_is_not_misdetected();
   test_gc_instance_survives();
   test_gc_circular_cons_list_does_not_hang();
   test_gc_reclaims_unreferenced_garbage();

   test_gc_fires_during_bignum_addition_and_result_is_correct();
   test_gc_fires_during_gcd_and_isqrt_and_results_are_correct();
   test_gc_fires_during_vector_construction_and_elements_are_preserved();
   test_gc_fires_during_string_append_and_result_is_correct();

   return g_test_failed ? 1 : 0;
}
