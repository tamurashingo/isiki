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
    assert(f1 >> FIXNUM_VALUE_SHIFT == 42, "os_make_fixnum(42)はFIXNUM_VALUE_SHIFT分の右シフトで42に戻る");
}

#define BOOT_ALLOC_TEST_SIZE (64 * 1024)

void test_os_boot_alloc_advances_bump_pointer_with_alignment() {
    void *region = malloc(BOOT_ALLOC_TEST_SIZE);
    assert(region != NULL, "boot allocatorのテスト用領域をmallocで確保できる");
    os_boot_alloc_init((UINT64)region, BOOT_ALLOC_TEST_SIZE);

    void *a = os_boot_alloc(3, 8);
    assert((UINT64)a % 8 == 0, "os_boot_allocは要求したalignに揃えたアドレスを返す");
    assert((UINT64)a == (UINT64)region, "1回目の確保は領域の先頭から始まる");

    void *b = os_boot_alloc(5, 8);
    assert((UINT64)b % 8 == 0, "2回目の確保もalignに揃う");
    assert((UINT64)b >= (UINT64)a + 3, "2回目の確保は1回目の直後以降から始まる(重ならない)");

    free(region);
}

void test_os_boot_alloc_finalize_returns_remaining_region_after_usage() {
    void *region = malloc(BOOT_ALLOC_TEST_SIZE);
    assert(region != NULL, "boot allocatorのテスト用領域をmallocで確保できる");
    os_boot_alloc_init((UINT64)region, BOOT_ALLOC_TEST_SIZE);

    os_boot_alloc(100, 8);
    os_boot_alloc(200, 8);

    UINT64 out_base, out_size;
    UINT64 used = os_boot_alloc_finalize(&out_base, &out_size);

    assert(used >= 300, "finalizeが返す使用量は、それまでのos_boot_alloc要求の合計以上である");
    assert(out_base >= (UINT64)region + used, "残り領域の先頭は使用済み分より後ろにある");
    /* [境界] この戻り値がそのままos_heap_initへ渡りLispヒープの先頭になる。
       確保を16byteに切り上げても先頭が8 mod 16ならオブジェクトが全部ずれるので、
       ここはOS_HEAP_ALIGN境界であることまで要求する(以前は8byte境界だった) */
    assert(out_base % OS_HEAP_ALIGN == 0,
           "残り領域の先頭はOS_HEAP_ALIGN境界に整列される(Lispヒープの先頭になるため)");
    assert(out_base + out_size == (UINT64)region + BOOT_ALLOC_TEST_SIZE,
           "残り領域は元の領域の末尾まで隙間なく続く");

    free(region);
}

#undef BOOT_ALLOC_TEST_SIZE

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
    assert((c >> CHAR_VALUE_SHIFT) == 'A', "os_make_char('A')はCHAR_VALUE_SHIFT分の右シフトで'A'に戻る");
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
    assert(v1 >> FIXNUM_VALUE_SHIFT == 1, "1であること");

    // current_env 自身の変数が取得できること。シンボル名は大文字小文字を区別しない
    lisp_val_t v2 = os_get_variable(os_make_symbol("SYM2"), current_env);
    assert((v2 & TAG_MASK) == TAG_STRING, "stringが返ること");
    const char *s2 = (const char *)((v2 & ~TAG_MASK) + 8);
    assert(strncmp(s2, "hello world", 11) == 0, "\"hello world\"であること");


    // current_env で sym1 をセットしても base_env の値は書き換わらないこと
    os_set_variable(os_make_symbol("SYM1"), os_make_fixnum(42), current_env);

    lisp_val_t v3 = os_get_variable(os_make_symbol("sym1"), current_env);
    assert((v3 & TAG_MASK) == TAG_FIXNUM, "fixnumが返ること");
    assert(v3 >> FIXNUM_VALUE_SHIFT == 42, "current_envにセットした42が優先して返る");

    lisp_val_t v4 = os_get_variable(os_make_symbol("sym1"), base_env);
    assert(v4 >> FIXNUM_VALUE_SHIFT == 1, "base_env自身の値は書き換えられていないこと");

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
    // process_init(process.c)は生成する全プロセスのproc->envを直ちにos_gc_register_rootで
    // GCのrootとして登録するが、このテストはprocess_initを経由せずget_current_process()を
    // 直接使うため、その登録が行われていない。ここで明示的に登録しないと、
    // primitive_set_current_environmentが書き込んだproc->envはGCで再配置対象にならず、
    // 以降のGCでFrom空間の古いアドレスを指したまま残り、他のテストのos_gc_collectが
    // gc_fixup_environment_cells経由でその古いアドレスを環境として辿った際に
    // 未定義動作(実測でSEGV)を起こす
    os_gc_register_root(&get_current_process()->env);

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

    // このテスト用に登録したrootを片付け、proc->envも他のテストが前提とする初期値
    // (process.cのproc->env=0と同じ、rootでない生の0)へ戻す
    os_gc_unregister_root(&get_current_process()->env);
    get_current_process()->env = 0;
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
    assert(r >> FIXNUM_VALUE_SHIFT == 24, "(* 2 3 4) は24");

    lisp_val_t r2 = primitive_multiply(make_arg_list(1, 5), nil);
    assert(r2 >> FIXNUM_VALUE_SHIFT == 5, "(* 5) は5");
}

void test_primitive_divide() {
    lisp_val_t r = primitive_divide(make_arg_list(2, 12, 3), nil);
    assert(r >> FIXNUM_VALUE_SHIFT == 4, "(/ 12 3) は4");

    lisp_val_t r2 = primitive_divide(make_arg_list(3, 24, 4, 2), nil);
    assert(r2 >> FIXNUM_VALUE_SHIFT == 3, "(/ 24 4 2) は3");

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
    assert(primitive_max(make_arg_list(3, 1, 5, 3), nil) >> FIXNUM_VALUE_SHIFT == 5, "(max 1 5 3) は5");
    assert(primitive_min(make_arg_list(3, 5, 1, 3), nil) >> FIXNUM_VALUE_SHIFT == 1, "(min 5 1 3) は1");

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
    assert(cc_car(result) >> FIXNUM_VALUE_SHIFT == 2, "(array-dimensions a)の1番目は2");
    assert(cc_car(cc_cdr(result)) >> FIXNUM_VALUE_SHIFT == 3, "(array-dimensions a)の2番目は3");
    assert(cc_cdr(cc_cdr(result)) == nil, "(array-dimensions a)の終端はnil");
}

void test_primitive_array_dimensions_on_string() {
    lisp_val_t str = os_make_string("foo");

    lisp_val_t result = primitive_array_dimensions(os_make_cons(str, nil), nil);
    assert(cc_car(result) >> FIXNUM_VALUE_SHIFT == 3, "(array-dimensions \"foo\")の1番目は3");
    assert(cc_cdr(result) == nil, "(array-dimensions \"foo\")の終端はnil");
}

void test_primitive_aref_reads_back_value() {
    lisp_val_t dims = os_make_cons(os_make_fixnum(2), os_make_cons(os_make_fixnum(3), nil));
    lisp_val_t array = primitive_make_array(os_make_cons(dims, nil), nil);

    // 位置(1,2)は行優先オフセットで1*3+2=5番目。引数順はISLisp仕様通り (set-aref obj array z*)
    lisp_val_t set_args = os_make_cons(os_make_fixnum(99),
        os_make_cons(array,
        os_make_cons(os_make_fixnum(1),
        os_make_cons(os_make_fixnum(2), nil))));
    lisp_val_t set_result = primitive_set_aref(set_args, nil);
    assert(set_result >> FIXNUM_VALUE_SHIFT == 99, "(set-aref 99 a 1 2)は書き込んだ99を返す");

    lisp_val_t aref_args = os_make_cons(array,
        os_make_cons(os_make_fixnum(1),
        os_make_cons(os_make_fixnum(2), nil)));
    assert(primitive_aref(aref_args, nil) >> FIXNUM_VALUE_SHIFT == 99, "(aref a 1 2)は書き込んだ99を返す");

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
    assert(primitive_length(os_make_cons(empty, nil), nil) >> FIXNUM_VALUE_SHIFT == 0, "(vector)は空のvectorを返す");
}

void test_primitive_create_vector() {
    lisp_val_t vec = primitive_create_vector(os_make_cons(os_make_fixnum(3), nil), nil);
    assert(primitive_length(os_make_cons(vec, nil), nil) >> FIXNUM_VALUE_SHIFT == 3, "(create-vector 3)の長さは3");
    lisp_val_t aref_args = os_make_cons(vec, os_make_cons(os_make_fixnum(0), nil));
    assert(primitive_aref(aref_args, nil) == nil, "初期値省略時は各要素がnil");

    lisp_val_t vec_filled = primitive_create_vector(
        os_make_cons(os_make_fixnum(2), os_make_cons(os_make_fixnum(99), nil)), nil);
    lisp_val_t aref_args2 = os_make_cons(vec_filled, os_make_cons(os_make_fixnum(1), nil));
    assert(primitive_aref(aref_args2, nil) >> FIXNUM_VALUE_SHIFT == 99, "初期値指定時は各要素がその値で初期化される");
}

void test_primitive_garef_set_garef() {
    lisp_val_t vec = primitive_create_vector(os_make_cons(os_make_fixnum(3), nil), nil);
    lisp_val_t set_args = os_make_cons(os_make_fixnum(42),
        os_make_cons(vec, os_make_cons(os_make_fixnum(1), nil)));
    assert(primitive_set_aref(set_args, nil) >> FIXNUM_VALUE_SHIFT == 42, "set-garefはset-arefと同じ実体で書き込める");

    lisp_val_t aref_args = os_make_cons(vec, os_make_cons(os_make_fixnum(1), nil));
    assert(primitive_aref(aref_args, nil) >> FIXNUM_VALUE_SHIFT == 42, "garefはarefと同じ実体で読み込める");
}

void test_primitive_set_car() {
    lisp_val_t c = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
    lisp_val_t args = os_make_cons(c, os_make_cons(os_make_fixnum(99), nil));

    lisp_val_t r = primitive_set_car(args, nil);
    assert(r >> FIXNUM_VALUE_SHIFT == 99, "(set-car c 99)は書き込んだ99を返す");
    assert(cc_car(c) >> FIXNUM_VALUE_SHIFT == 99, "carが99に書き換えられている");
    assert(cc_cdr(c) >> FIXNUM_VALUE_SHIFT == 2, "cdrは書き換えられていない");
}

void test_primitive_set_cdr() {
    lisp_val_t c = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
    lisp_val_t args = os_make_cons(c, os_make_cons(os_make_fixnum(99), nil));

    lisp_val_t r = primitive_set_cdr(args, nil);
    assert(r >> FIXNUM_VALUE_SHIFT == 99, "(set-cdr c 99)は書き込んだ99を返す");
    assert(cc_cdr(c) >> FIXNUM_VALUE_SHIFT == 99, "cdrが99に書き換えられている");
    assert(cc_car(c) >> FIXNUM_VALUE_SHIFT == 1, "carは書き換えられていない");
}

void test_primitive_set_aref_out_of_bounds() {
    lisp_val_t array = primitive_make_array(os_make_cons(os_make_fixnum(3), nil), nil);
    lisp_val_t args = os_make_cons(os_make_fixnum(1),
        os_make_cons(array,
        os_make_cons(os_make_fixnum(3), nil)));
    assert(primitive_set_aref(args, nil) == g_sym_eval_error, "(set-aref 1 a 3)は範囲外なのでg_sym_eval_error");
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
    assert((r >> CHAR_VALUE_SHIFT) == 'e', "(string-elt \"hello\" 1)は'e'を返す");
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
    assert(primitive_char_index(os_make_cons(os_make_char('b'), os_make_cons(str, nil)), nil) >> FIXNUM_VALUE_SHIFT == 1,
           "(char-index #\\b \"abcab\") は1");
    assert(primitive_char_index(os_make_cons(os_make_char('B'), os_make_cons(str, nil)), nil) == nil,
           "(char-index #\\B \"abcab\") はnil(大文字小文字を区別する)");
    lisp_val_t args_with_start = os_make_cons(os_make_char('b'), os_make_cons(str, os_make_cons(os_make_fixnum(2), nil)));
    assert(primitive_char_index(args_with_start, nil) >> FIXNUM_VALUE_SHIFT == 4, "(char-index #\\b \"abcab\" 2) は4");
    assert(primitive_char_index(os_make_cons(os_make_char('d'), os_make_cons(str, nil)), nil) == nil,
           "(char-index #\\d \"abcab\") はnil");
    lisp_val_t args_a_from_4 = os_make_cons(os_make_char('a'), os_make_cons(str, os_make_cons(os_make_fixnum(4), nil)));
    assert(primitive_char_index(args_a_from_4, nil) == nil, "(char-index #\\a \"abcab\" 4) はnil");
}

void test_primitive_string_index() {
    lisp_val_t foobar = os_make_string("foobar");
    assert(primitive_string_index(os_make_cons(os_make_string("foo"), os_make_cons(foobar, nil)), nil) >> FIXNUM_VALUE_SHIFT == 0,
           "(string-index \"foo\" \"foobar\") は0");
    assert(primitive_string_index(os_make_cons(os_make_string("bar"), os_make_cons(foobar, nil)), nil) >> FIXNUM_VALUE_SHIFT == 3,
           "(string-index \"bar\" \"foobar\") は3");
    assert(primitive_string_index(os_make_cons(os_make_string("FOO"), os_make_cons(foobar, nil)), nil) == nil,
           "(string-index \"FOO\" \"foobar\") はnil(大文字小文字を区別する)");
    lisp_val_t foo_from_1 = os_make_cons(os_make_string("foo"), os_make_cons(foobar, os_make_cons(os_make_fixnum(1), nil)));
    assert(primitive_string_index(foo_from_1, nil) == nil, "(string-index \"foo\" \"foobar\" 1) はnil");
    lisp_val_t bar_from_1 = os_make_cons(os_make_string("bar"), os_make_cons(foobar, os_make_cons(os_make_fixnum(1), nil)));
    assert(primitive_string_index(bar_from_1, nil) >> FIXNUM_VALUE_SHIFT == 3, "(string-index \"bar\" \"foobar\" 1) は3");
    assert(primitive_string_index(os_make_cons(os_make_string("foo"), os_make_cons(os_make_string(""), nil)), nil) == nil,
           "(string-index \"foo\" \"\") はnil");
    assert(primitive_string_index(os_make_cons(os_make_string(""), os_make_cons(os_make_string("foo"), nil)), nil) >> FIXNUM_VALUE_SHIFT == 0,
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
    assert(primitive_length(os_make_cons(list, nil), nil) >> FIXNUM_VALUE_SHIFT == 3, "(length '(1 2 3))は3");
    assert(primitive_length(os_make_cons(nil, nil), nil) >> FIXNUM_VALUE_SHIFT == 0, "(length nil)は0");

    lisp_val_t str = os_make_string("hello");
    assert(primitive_length(os_make_cons(str, nil), nil) >> FIXNUM_VALUE_SHIFT == 5, "(length \"hello\")は5");

    lisp_val_t dims = os_make_cons(os_make_fixnum(2), os_make_cons(os_make_fixnum(3), nil));
    lisp_val_t array = primitive_make_array(os_make_cons(dims, nil), nil);
    assert(primitive_length(os_make_cons(array, nil), nil) >> FIXNUM_VALUE_SHIFT == 6, "(length (make-array '(2 3)))は次元に関わらず全要素数の6");
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
    assert(primitive_set_elt(set_list_args, nil) >> FIXNUM_VALUE_SHIFT == 99, "(set-elt 99 list 1) は書き込んだ99を返す");
    assert(primitive_elt(os_make_cons(list, os_make_cons(os_make_fixnum(1), nil)), nil) >> FIXNUM_VALUE_SHIFT == 99,
           "set-eltで書き換えた後のlistの1番目は99");

    lisp_val_t vec = primitive_create_vector(os_make_cons(os_make_fixnum(3), nil), nil);
    lisp_val_t set_vec_args = os_make_cons(os_make_fixnum(42), os_make_cons(vec, os_make_cons(os_make_fixnum(1), nil)));
    assert(primitive_set_elt(set_vec_args, nil) >> FIXNUM_VALUE_SHIFT == 42, "(set-elt 42 vec 1) は書き込んだ42を返す");
    assert(primitive_elt(os_make_cons(vec, os_make_cons(os_make_fixnum(1), nil)), nil) >> FIXNUM_VALUE_SHIFT == 42,
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
    assert(primitive_length(os_make_cons(list_result, nil), nil) >> FIXNUM_VALUE_SHIFT == 3, "(subseq '(a b c d e f) 1 4) の長さは3");

    lisp_val_t vec = primitive_vector(list, nil);
    lisp_val_t vec_result = primitive_subseq(
        os_make_cons(vec, os_make_cons(os_make_fixnum(1), os_make_cons(os_make_fixnum(4), nil))), nil);
    assert((vec_result & TAG_MASK) == TAG_INSTANCE, "(subseq (vector ...) 1 4) の戻り値はTAG_INSTANCEを持つ(VECTOR)");
    assert(primitive_length(os_make_cons(vec_result, nil), nil) >> FIXNUM_VALUE_SHIFT == 3, "(subseq (vector 'a 'b 'c 'd 'e 'f) 1 4) の長さは3");
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
    /* [4bit化] スロットのアドレスは TAG_RAW_POINTER を付けて保存されるので、
       **16byte境界に置かないと下位4bitが落ちて別々のスロットが同一視される**。
       本番のリテラルスロットは za_slot_t(16byteストライド、PR #74)なので条件を
       満たしている。テスト側のダミーも同じ条件にそろえる
       (3bitタグのころは8byte間隔のスタック変数でもたまたま区別できていた) */
    static lisp_val_t fake_slots[2] __attribute__((aligned(OS_HEAP_ALIGN)));
    _Static_assert(sizeof(lisp_val_t) * 2 <= 2 * OS_HEAP_ALIGN, "fake_slots stride assumption");
    lisp_val_t *fake_slot_a = &fake_slots[0];
    lisp_val_t *fake_slot_b = (lisp_val_t *)((UINT8 *)&fake_slots[0] + OS_HEAP_ALIGN);
    *fake_slot_a = os_make_fixnum(111);
    *fake_slot_b = os_make_fixnum(222);

    g_literal_slot_reclaim_seen_count = 0;
    os_environment_register_literal_slot(env, fake_slot_a);
    os_environment_register_literal_slot(env, fake_slot_b);
    os_environment_reclaim_literal_slots(env, test_literal_slot_reclaim_callback);

    assert(g_literal_slot_reclaim_seen_count == 2, "登録した2件それぞれについてコールバックが呼ばれる");
    assert((g_literal_slot_reclaim_seen[0] == fake_slot_a || g_literal_slot_reclaim_seen[0] == fake_slot_b),
           "コールバックに渡されるのは登録したアドレスのいずれか");
    assert((g_literal_slot_reclaim_seen[1] == fake_slot_a || g_literal_slot_reclaim_seen[1] == fake_slot_b),
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

/* ===== 4bitタグ体系の網羅テスト =========================================
 *
 * タグは16値ある。**未割当の値も含めて全16値を固定する**のが要点で、
 * 「今使っている値だけ」を試すと、未割当タグの扱いが決まっていないことが
 * テストからは見えない。GCが即値をポインタとして追いかけると静かに
 * ヒープが壊れるので、ここは網羅でなければ意味がない。
 */

/** タグ1つぶんの期待値。documents/tag4-step2.md の表と同じ内容 */
typedef struct {
    UINT64 tag;
    const char *name;
    int holds_address;   /* bit0。アドレスを持つか */
    int is_heap_ref;     /* GCが追いかけるか */
} tag_expectation_t;

static const tag_expectation_t g_tag_expectations[16] = {
    /* tag                 name              addr  heap */
    { 0x0ULL,              "FIXNUM",            0,   0 },
    { 0x1ULL,              "CONS",              1,   1 },
    { 0x2ULL,              "CHAR",              0,   0 },
    { 0x3ULL,              "SYMBOL",            1,   1 },
    { 0x4ULL,              "SINGLE-FLOAT(予約)", 0,   0 },
    { 0x5ULL,              "STRING",            1,   1 },
    { 0x6ULL,              "即値・未割当",        0,   0 },
    { 0x7ULL,              "INSTANCE",          1,   1 },
    { 0x8ULL,              "即値・未割当",        0,   0 },
    { 0x9ULL,              "RAW_POINTER",       1,   0 },
    { 0xAULL,              "即値・未割当",        0,   0 },
    { 0xBULL,              "アドレス・未割当",     1,   0 },
    { 0xCULL,              "即値・未割当",        0,   0 },
    { 0xDULL,              "アドレス・未割当",     1,   0 },
    { 0xEULL,              "MARKER(予約)",       0,   0 },
    { 0xFULL,              "FORWARD",           1,   1 },
};

void test_tag_table_covers_all_sixteen_values() {
    assert(TAG_MASK == 0xFULL, "TAG_MASKは4bit(0xF)である");

    /* 名前付き定数が表のどこに座っているか。表と定数がずれたらここで落ちる */
    assert(TAG_FIXNUM       == 0x0ULL, "TAG_FIXNUMは0x0");
    assert(TAG_CONS         == 0x1ULL, "TAG_CONSは0x1");
    assert(TAG_CHAR         == 0x2ULL, "TAG_CHARは0x2");
    assert(TAG_SYMBOL       == 0x3ULL, "TAG_SYMBOLは0x3");
    assert(TAG_SINGLE_FLOAT == 0x4ULL, "TAG_SINGLE_FLOATは0x4(予約のみ)");
    assert(TAG_STRING       == 0x5ULL, "TAG_STRINGは0x5");
    assert(TAG_INSTANCE     == 0x7ULL, "TAG_INSTANCEは0x7");
    assert(TAG_RAW_POINTER  == 0x9ULL, "TAG_RAW_POINTERは0x9");
    assert(TAG_MARKER       == 0xEULL, "TAG_MARKERは0xE");
    assert(TAG_FORWARD      == 0xFULL, "TAG_FORWARDは0xF");

    int addr_mismatch = 0, heap_mismatch = 0, index_mismatch = 0;
    for (UINT64 t = 0; t < 16; t++) {
        const tag_expectation_t *e = &g_tag_expectations[t];
        if (e->tag != t) { index_mismatch++; }
        if (os_tag_holds_address(t) != e->holds_address) { addr_mismatch++; }
        if (os_tag_is_heap_ref(t)   != e->is_heap_ref)   { heap_mismatch++; }
    }
    assert(index_mismatch == 0, "期待値テーブルの添字とタグ値が一致している");
    assert(addr_mismatch == 0,
           "os_tag_holds_addressが全16値で期待どおり(bit0=1がアドレス側)");
    assert(heap_mismatch == 0,
           "os_tag_is_heap_refが全16値で期待どおり(未割当・予約・raw pointerは偽)");
}

void test_tag_immediate_pointer_split_is_bit0() {
    int mismatch = 0;
    for (UINT64 t = 0; t < 16; t++) {
        /* bit0 が即値/ポインタの唯一の判断材料であること */
        if (((t & 0x1ULL) != 0) != (os_tag_holds_address(t) != 0)) { mismatch++; }
    }
    assert(mismatch == 0, "即値とアドレスの区別は全16値でbit0だけで決まる");

    /* GCが追いかけるタグは必ずアドレスを持つ(逆は成り立たない) */
    int follows_immediate = 0;
    for (UINT64 t = 0; t < 16; t++) {
        if (os_tag_is_heap_ref(t) && !os_tag_holds_address(t)) { follows_immediate++; }
    }
    assert(follows_immediate == 0,
           "GCが追いかけるタグに即値(bit0=0)は1つもない");
}

void test_unassigned_and_reserved_tags_are_not_followed_by_gc() {
    /* 未割当・予約タグは「まだ誰も作らない」だけで「安全」ではない。
       GCが追いかけると生のビットパターンをヒープオブジェクトとして複製し、
       word0へ転送先を書き込む(= 静かなヒープ破壊)。偽であることを固定する */
    const UINT64 unassigned[] = { 0x6ULL, 0x8ULL, 0xAULL, 0xCULL, 0xBULL, 0xDULL };
    int followed = 0;
    for (UINT64 i = 0; i < sizeof(unassigned) / sizeof(unassigned[0]); i++) {
        if (os_tag_is_heap_ref(unassigned[i])) { followed++; }
    }
    assert(followed == 0, "未割当6値はどれもos_tag_is_heap_refが偽");
    assert(!os_tag_is_heap_ref(TAG_SINGLE_FLOAT), "予約のTAG_SINGLE_FLOATは追いかけない");
    assert(!os_tag_is_heap_ref(TAG_MARKER), "予約のTAG_MARKERは追いかけない");
    assert(!os_tag_is_heap_ref(TAG_RAW_POINTER),
           "TAG_RAW_POINTERはアドレスを持つがGC管理外なので追いかけない");
}

void test_immediate_encode_decode_round_trip() {
    /* fixnum: 0・1・境界値・境界値-1 */
    const UINT64 mags[] = { 0ULL, 1ULL, 42ULL, FIXNUM_MAGNITUDE_MASK - 1, FIXNUM_MAGNITUDE_MASK };
    int bad_tag = 0, bad_value = 0;
    for (UINT64 i = 0; i < sizeof(mags) / sizeof(mags[0]); i++) {
        lisp_val_t v = os_make_fixnum(mags[i]);
        if ((v & TAG_MASK) != TAG_FIXNUM) { bad_tag++; }
        if (os_fixnum_magnitude(v) != mags[i]) { bad_value++; }
    }
    assert(bad_tag == 0, "fixnumのエンコードは全ケースでTAG_FIXNUMになる");
    assert(bad_value == 0, "fixnumはエンコード→デコードで値が保たれる");

    /* 符号付き。符号絶対値なので下端は上端の符号反転ちょうど */
    lisp_val_t neg = os_make_fixnum_signed(1, FIXNUM_MAGNITUDE_MASK);
    assert((neg & TAG_MASK) == TAG_FIXNUM, "負のfixnumもTAG_FIXNUM");
    assert(os_fixnum_is_negative(neg), "負のfixnumは符号bitが立つ");
    assert(os_fixnum_magnitude(neg) == FIXNUM_MAGNITUDE_MASK, "負のfixnumもマグニチュードが保たれる");
    assert(os_make_fixnum_signed(1, 0) == os_make_fixnum(0), "-0は0に正規化される");

    /* character: ASCII・0x80境界・BMP・コードポイント上限 */
    const UINT32 codes[] = { 0x00u, 0x41u, 0x7Fu, 0x80u, 0xFFu, 0x3042u, 0xFFFFu, 0x10FFFFu, 0xFFFFFFFFu };
    int c_bad_tag = 0, c_bad_value = 0;
    for (UINT64 i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        lisp_val_t v = os_make_char(codes[i]);
        if ((v & TAG_MASK) != TAG_CHAR) { c_bad_tag++; }
        if ((UINT32)(v >> CHAR_VALUE_SHIFT) != codes[i]) { c_bad_value++; }
    }
    assert(c_bad_tag == 0, "characterのエンコードは全ケースでTAG_CHARになる");
    assert(c_bad_value == 0, "characterはエンコード→デコードでコードポイントが保たれる");
}

void test_char_high_code_points_do_not_corrupt_upper_bits() {
    /* [4bit化] os_make_char の引数が const char だったころ、0x80以上で符号拡張が
       起きて上位ビットがすべて1になっていた。UINT32化で解消したことを固定する */
    lisp_val_t c80 = os_make_char(0x80u);
    assert((c80 & TAG_MASK) == TAG_CHAR, "0x80のcharacterはTAG_CHAR");
    assert((UINT32)(c80 >> CHAR_VALUE_SHIFT) == 0x80u, "0x80のコードポイントがそのまま戻る");
    /* 符号拡張が起きていれば bit32-63 が全部1になり、この比較で落ちる */
    assert((c80 >> CHAR_VALUE_SHIFT) == 0x80ULL, "0x80で上位ビットが1で埋まらない");

    lisp_val_t cff = os_make_char(0xFFu);
    assert((cff >> CHAR_VALUE_SHIFT) == 0xFFULL, "0xFFで上位ビットが1で埋まらない");

    /* タグ領域とコードポイント領域の**外側**(bit4-31の空き)には何も入らない */
    const lisp_val_t char_used_bits = TAG_MASK | (0xFFFFFFFFULL << CHAR_VALUE_SHIFT);
    assert((c80 & ~char_used_bits) == 0, "0x80のcharacterはタグ/コード以外のビットを持たない");
    assert((cff & ~char_used_bits) == 0, "0xFFのcharacterはタグ/コード以外のビットを持たない");
    assert((os_make_char(0x10FFFFu) & ~char_used_bits) == 0,
           "コードポイント上限のcharacterもタグ/コード以外のビットを持たない");
}

void test_immediates_survive_gc_unchanged() {
    /* 即値はGCが素通しする。GCを挟んで値が変わらないことを確かめる。
       未割当タグの値も一緒に通し、GCが追いかけて落ちないことを見る */
    lisp_val_t fix = os_make_fixnum(123456789ULL);
    lisp_val_t neg = os_make_fixnum_signed(1, 987654321ULL);
    lisp_val_t chr = os_make_char(0x3042u);
    os_gc_register_root(&fix);
    os_gc_register_root(&neg);
    os_gc_register_root(&chr);

    /* 未割当タグ・予約タグの値をルートに置く。GCが追いかけたら
       canonicalでないアドレスを読んで落ちるか、値が書き換わる */
    lisp_val_t unassigned = (lisp_val_t)0xDEADBEEF0000ULL | 0x6ULL;
    lisp_val_t marker     = (lisp_val_t)0xDEADBEEF0000ULL | TAG_MARKER;
    lisp_val_t sfloat     = (lisp_val_t)0xDEADBEEF0000ULL | TAG_SINGLE_FLOAT;
    os_gc_register_root(&unassigned);
    os_gc_register_root(&marker);
    os_gc_register_root(&sfloat);

    lisp_val_t fix_before = fix, neg_before = neg, chr_before = chr;
    lisp_val_t un_before = unassigned, mk_before = marker, sf_before = sfloat;

    os_gc_collect();

    assert(fix == fix_before, "fixnumはGCを跨いでも値が変わらない");
    assert(neg == neg_before, "負のfixnumはGCを跨いでも値が変わらない");
    assert(chr == chr_before, "characterはGCを跨いでも値が変わらない");
    assert(unassigned == un_before, "未割当タグの値はGCを跨いでも変わらない(追いかけられていない)");
    assert(marker == mk_before, "TAG_MARKERの値はGCを跨いでも変わらない");
    assert(sfloat == sf_before, "TAG_SINGLE_FLOATの値はGCを跨いでも変わらない");

    os_gc_unregister_root(&sfloat);
    os_gc_unregister_root(&marker);
    os_gc_unregister_root(&unassigned);
    os_gc_unregister_root(&chr);
    os_gc_unregister_root(&neg);
    os_gc_unregister_root(&fix);
}

void test_single_float_bit_pattern_round_trip() {
    /* float → 即値 → float でビットパターンが完全に戻ること。
       union経由でしか触らないので、非正規化数・NaN・無限大も素通しできるはず */
    union { float f; UINT32 u; } conv;
    const UINT32 patterns[] = {
        0x00000000UL,   /* +0.0 */
        0x80000000UL,   /* -0.0 */
        0x3F800000UL,   /* 1.0 */
        0xBFC00000UL,   /* -1.5 */
        0x00000001UL,   /* 最小の非正規化数 */
        0x007FFFFFUL,   /* 最大の非正規化数 */
        0x00800000UL,   /* 最小の正規化数 */
        (UINT32)SINGLE_FLOAT_MAX_BITS,              /* FLT_MAX */
        (UINT32)SINGLE_FLOAT_MAX_BITS | 0x80000000UL, /* -FLT_MAX */
        0x7F800000UL,   /* +inf */
        0xFF800000UL,   /* -inf */
        0x7FC00000UL,   /* quiet NaN */
    };
    int bad_tag = 0, bad_bits = 0, bad_upper = 0;
    for (UINT64 i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        conv.u = patterns[i];
        lisp_val_t v = os_make_single_float(conv.f);
        if ((v & TAG_MASK) != TAG_SINGLE_FLOAT) { bad_tag++; }
        /* bit4-31 は未使用(0)でなければならない */
        if ((v & ~(lisp_val_t)TAG_MASK & 0xFFFFFFFFULL) != 0) { bad_upper++; }
        union { float f; UINT32 u; } back;
        back.f = os_single_float_value(v);
        if (back.u != patterns[i]) { bad_bits++; }
    }
    assert(bad_tag == 0, "single-floatのエンコードは全ケースでTAG_SINGLE_FLOATになる");
    assert(bad_upper == 0, "single-floatの即値はbit4-31が0(タグとビットパターン以外を汚さない)");
    assert(bad_bits == 0,
           "single-floatはエンコード→デコードでビットパターンが完全に保たれる(0.0/-0.0/非正規化数/inf/NaNを含む)");

    /* +0.0 と -0.0 は**別のビットパターン**なので即値としても別の値になる */
    assert(os_make_single_float(0.0f) != os_make_single_float(-0.0f),
           "+0.0と-0.0のsingle-floatは別のビットパターンになる");
    /* ただし数としては等しい。equalは数値をeql比較する(ISLisp §13)。
       引数はどちらも即値なので、consを組む間にGCが走っても壊れない */
    lisp_val_t zero_args = os_make_cons(os_make_single_float(0.0f),
                                        os_make_cons(os_make_single_float(-0.0f), nil));
    assert(primitive_equal(zero_args, nil) == g_sym_t,
           "+0.0と-0.0はequalで等しい(値として比較される)");
}

void test_single_float_is_a_number_and_a_float() {
    lisp_val_t v = os_make_single_float(1.5f);
    assert(primitive_floatp1(v) == g_sym_t, "single-floatはfloatpで真");
    assert(primitive_numberp1(v) == g_sym_t, "single-floatはnumberpで真");
    assert(primitive_single_float_p1(v) == g_sym_t, "single-floatは%%single-float-pで真");
    assert(primitive_fixnump1(v) == nil, "single-floatはfixnumpで偽");

    lisp_val_t d = os_make_float(1.5);
    assert(primitive_floatp1(d) == g_sym_t, "double-floatもfloatpで真");
    assert(primitive_single_float_p1(d) == nil, "double-floatは%%single-float-pで偽");

    /* os_float_valueは両方を受ける。即値をアドレスとして読まないこと */
    assert(os_float_value(v) == 1.5, "os_float_valueはsingle-floatも受ける");
    assert(os_float_value(d) == 1.5, "os_float_valueはdouble-floatも受ける");
}

void test_single_float_limits_come_from_c() {
    lisp_val_t pos = primitive_most_positive_single_float(nil, nil);
    lisp_val_t neg = primitive_most_negative_single_float(nil, nil);
    assert((pos & TAG_MASK) == TAG_SINGLE_FLOAT, "*most-positive-single-float*はsingle-float");
    assert((neg & TAG_MASK) == TAG_SINGLE_FLOAT, "*most-negative-single-float*はsingle-float");
    /* 有限であること: 10倍すると無限大になるが、自分自身は無限大ではない */
    float p = os_single_float_value(pos);
    assert(p == p && p * 10.0f != p, "*most-positive-single-float*は有限の最大値");
    assert(os_single_float_value(neg) == -p, "負側は正側の符号反転ちょうど");

    lisp_val_t dpos = primitive_most_positive_double_float(nil, nil);
    lisp_val_t dneg = primitive_most_negative_double_float(nil, nil);
    double dp = os_float_value(dpos);
    assert(dp == dp && dp * 10.0 != dp, "*most-positive-double-float*は有限の最大値");
    assert(os_float_value(dneg) == -dp, "double側も負は正の符号反転ちょうど");
    /* singleの最大値はdoubleの最大値より小さい */
    assert((double)p < dp, "単精度の最大値は倍精度の最大値より小さい");
}

void test_single_floats_survive_gc_unchanged() {
    /* 即値なのでGCは追いかけない。大量に作ってGCを跨いでも値が壊れないこと */
    lisp_val_t kept[8];
    for (int i = 0; i < 8; i++) {
        kept[i] = os_make_single_float((float)i + 0.5f);
        os_gc_register_root(&kept[i]);
    }
    lisp_val_t before[8];
    for (int i = 0; i < 8; i++) { before[i] = kept[i]; }

    /* GCを確実に動かすため、間にヒープを使う値を挟んで2回回す */
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < 64; i++) {
            (void)os_make_cons(os_make_single_float((float)i), nil);
        }
        os_gc_collect();
    }

    int changed = 0;
    for (int i = 0; i < 8; i++) {
        if (kept[i] != before[i]) { changed++; }
        if (os_single_float_value(kept[i]) != (float)i + 0.5f) { changed++; }
    }
    assert(changed == 0, "single-floatはGCを跨いでもビットパターンが変わらない");

    for (int i = 0; i < 8; i++) { os_gc_unregister_root(&kept[i]); }
}

/* --- float の型昇格 (c-1) ----------------------------------------------
   規則: より広い形式へ寄せる。整数は float に従う。
   documents/float-contagion.md */

/** ヒープのバンプポインタが進んだ量(byte)。確保がゼロかどうかを直接見る */
static UINT64 heap_used_now(void) {
    return os_fixnum_magnitude(primitive_heap_used_bytes(nil, nil));
}

void test_float_contagion_two_arg_result_types() {
    lisp_val_t sf = os_make_single_float(1.5f);
    lisp_val_t df = os_make_float(1.5);
    lisp_val_t fx = os_make_fixnum(2);

    struct { lisp_val_t a, b; int expect_single; const char *what; } cases[] = {
        { sf, sf, 1, "single + single -> single" },
        { fx, sf, 1, "fixnum + single -> single" },
        { sf, fx, 1, "single + fixnum -> single" },
        { sf, df, 0, "single + double -> double" },
        { df, sf, 0, "double + single -> double" },
        { df, df, 0, "double + double -> double" },
        { fx, df, 0, "fixnum + double -> double" },
        { df, fx, 0, "double + fixnum -> double" },
    };
    int bad_add = 0, bad_sub = 0, bad_mul = 0;
    for (UINT64 i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (os_is_single_float(primitive_add2(cases[i].a, cases[i].b))      != cases[i].expect_single) { bad_add++; }
        if (os_is_single_float(primitive_subtract2(cases[i].a, cases[i].b)) != cases[i].expect_single) { bad_sub++; }
        if (os_is_single_float(primitive_multiply2(cases[i].a, cases[i].b)) != cases[i].expect_single) { bad_mul++; }
    }
    assert(bad_add == 0, "primitive_add2の結果の型が全8通りで型昇格の規則どおり");
    assert(bad_sub == 0, "primitive_subtract2の結果の型が全8通りで型昇格の規則どおり");
    assert(bad_mul == 0, "primitive_multiply2の結果の型が全8通りで型昇格の規則どおり");

    /* 値も確かめる。fixnum同士は従来どおりfixnumのまま */
    assert(primitive_add2(os_make_fixnum(1), os_make_fixnum(2)) == os_make_fixnum(3),
           "fixnum同士の加算は従来どおりfixnum");
    assert(os_single_float_value(primitive_add2(sf, sf)) == 3.0f, "1.5f + 1.5f = 3.0f");
    assert(os_float_value(primitive_add2(sf, df)) == 3.0, "1.5f + 1.5d = 3.0d");
    assert(os_single_float_value(primitive_multiply2(sf, fx)) == 3.0f, "1.5f * 2 = 3.0f");
}

void test_single_float_arithmetic_allocates_nothing() {
    /* **c-1 の性能上の眼目。** single はタグ0x4の即値なので、結果を包むための
       ヒープ確保が要らない。以前は primitive_add2 が cons を2個作って
       primitive_add へ委譲していたため、型が正しくても確保が残る形だった
       (改善Aで見つけた「32byte/回」の正体と同じ構図)。

       バンプポインタ(g_from_ptr - g_from_start)を直接見て、1byteも
       進んでいないことを確認する。 */
    lisp_val_t a = os_make_single_float(1.5f);
    lisp_val_t b = os_make_single_float(2.25f);

    UINT64 before = heap_used_now();
    lisp_val_t r = a;
    for (int i = 0; i < 1000; i++) {
        r = primitive_add2(r, b);
        r = primitive_subtract2(r, b);
        r = primitive_multiply2(a, b);
    }
    UINT64 after = heap_used_now();
    assert(after == before, "single x single の + - * はヒープを1byteも確保しない");
    assert(os_is_single_float(r), "3000回まわしても結果はsingle-floatの即値のまま");

    /* 対照: double は MAGIC_FLOAT の instance なので必ず確保が起きる。
       この差が single 即値化の効果そのものである */
    lisp_val_t da = os_make_float(1.5);
    lisp_val_t db = os_make_float(2.25);
    UINT64 dbefore = heap_used_now();
    lisp_val_t dr = da;
    for (int i = 0; i < 1000; i++) {
        dr = primitive_add2(dr, db);
    }
    UINT64 dafter = heap_used_now();
    assert(dafter > dbefore, "double x double の加算はヒープを確保する(対照)");
    assert(!os_is_single_float(dr), "double同士の結果はsingle-floatではない");

    /* 整数 x single も確保ゼロ(整数側は float に従うだけなので) */
    UINT64 mbefore = heap_used_now();
    lisp_val_t mr = a;
    for (int i = 0; i < 1000; i++) {
        mr = primitive_add2(mr, os_make_fixnum(0));
    }
    UINT64 mafter = heap_used_now();
    assert(mafter == mbefore, "fixnum x single の加算もヒープを確保しない");
    assert(os_is_single_float(mr), "fixnum x single の結果はsingle-float");
}

void test_float_comparison_widens_to_double() {
    /* **狭いほうへ落としてはいけない。** double を single へ丸めてから比べると
       (= 0.1f0 0.1d0) が真になってしまう(精度を捨てている)。
       single を double へ広げるのが正しい。 */
    lisp_val_t sf_tenth = os_make_single_float(0.1f);
    lisp_val_t df_tenth = os_make_float(0.1);

    lisp_val_t pair = os_make_cons(sf_tenth, os_make_cons(df_tenth, nil));
    assert(primitive_num_equal(pair, nil) == nil,
           "(= 0.1f0 0.1d0) は偽。singleをdoubleへ広げて比較している");
    assert(primitive_less_than(pair, nil) == nil,
           "single の 0.1 を double へ広げた値は double の 0.1 より大きいので < は偽");
    assert(primitive_greater_than(pair, nil) == g_sym_t,
           "同じ理由で > は真");

    /* 1.5 は両形式で正確に表せるので、実装が誤っていても通る。
       テストに 0.1 系を必ず含めるのはこのため */
    lisp_val_t exact = os_make_cons(os_make_single_float(1.5f),
                                    os_make_cons(os_make_float(1.5), nil));
    assert(primitive_num_equal(exact, nil) == g_sym_t, "(= 1.5f0 1.5d0) は真");

    /* 整数と float も整数側を広げる */
    lisp_val_t int_vs_single = os_make_cons(os_make_fixnum(1),
                                            os_make_cons(os_make_single_float(1.0f), nil));
    assert(primitive_num_equal(int_vs_single, nil) == g_sym_t, "(= 1 1.0f0) は真");
}

void test_equal_still_requires_matching_tags() {
    /* equal はタグ一致を要求する。= とは別物で、本作業で変えていない */
    lisp_val_t mixed = os_make_cons(os_make_single_float(1.5f),
                                    os_make_cons(os_make_float(1.5), nil));
    assert(primitive_equal(mixed, nil) == nil, "(equal 1.5f0 1.5d0) は偽(型が違う)");
    assert(primitive_num_equal(mixed, nil) == g_sym_t, "(= 1.5f0 1.5d0) は真(値が同じ)");

    lisp_val_t same = os_make_cons(os_make_single_float(1.5f),
                                   os_make_cons(os_make_single_float(1.5f), nil));
    assert(primitive_equal(same, nil) == g_sym_t, "(equal 1.5f0 1.5f0) は真");
}

void test_gc_string_with_forward_tag_colliding_length_is_not_misdetected() {
    /* STRINGのword0は**生の長さ**なので、長さの下位ビットがたまたま TAG_FORWARD と
       一致する文字列が必ず存在する。長さは Lisp プログラムが決めるので制御できない
       (documents/tag4-design.md 5.4 が「範囲検査は廃止できない」と結論した理由)。
       gc_copy_value が To空間の範囲外であることを見て誤検知を回避できているかを確認する。

       [4bit化] 長さは TAG_FORWARD から作る。直書きの6のままだと、タグ値を動かした
       ときにこのテストだけが「衝突しない長さ」を試し続けて黙って無意味になる。 */
    const UINT64 colliding_len = TAG_FORWARD;
    char content[64];
    assert(colliding_len < sizeof(content), "衝突する長さがテスト用バッファに収まる");
    for (UINT64 i = 0; i < colliding_len; i++) { content[i] = (char)('a' + (i % 26)); }
    content[colliding_len] = '\0';

    lisp_val_t sym = os_make_symbol("GC-TEST-STRING-FWD-LEN");
    lisp_val_t str = os_make_string(content);
    UINT64 *header = (UINT64 *)(str & ~TAG_MASK);
    assert((header[0] & TAG_MASK) == TAG_FORWARD,
           "長さがTAG_FORWARDと同値の文字列は、word0の下位ビットがTAG_FORWARDと一致する(前提条件)");
    os_set_variable(sym, str, global_environment);
    lisp_addr_t addr_before = str & ~TAG_MASK;

    os_gc_collect();

    // symはGC前のFrom空間アドレスのままなので、検索にはGC後に取り直したsymbolを使う
    lisp_val_t after = os_get_variable(os_make_symbol("GC-TEST-STRING-FWD-LEN"), global_environment);
    assert((after & TAG_MASK) == TAG_STRING, "誤検知が起きてもGC後のタグはTAG_STRINGのまま");
    assert((after & ~TAG_MASK) != addr_before, "誤検知が起きても文字列はTo空間へ再配置される");

    char buf[64];
    os_string_to_cstr(after, buf, sizeof(buf));
    assert(strncmp(buf, content, (size_t)colliding_len) == 0, "誤検知が起きても文字列の内容は破壊されず保たれる");
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
//
// ISLisp仕様のCREATE-ARRAY(make-arrayと同じ実体)をos_bootstrapへ追加したことで、
// native関数1つ分(function object + Function Cellのcons 2つ + 環境alistのcons)が
// さらに恒久消費になり、76KBではgcd/isqrtテストがGC後も確保不能になって停止する
// ようになったため、その分だけ広げる
//
// 2026-09-15 declaim(Phase2): %%CURRENT-OPTIMIZE と %%OPTIMIZE-OF の2つを
// os_bootstrapへ追加し、さらに全environmentへ10番目のスロット(declaim)を足した。
//
// [境界] さらに os_alloc_bytes/gc_to_alloc の切り上げ単位を 8→16(OS_HEAP_ALIGN)へ
// 変えたことで、os_bootstrap直後の生存量が実測 21,120 → 21,536 byte(+416 byte)に
// 増えた。増えるのはSTRINGだけで(CONS 16/SYMBOL 32/INSTANCE 32は元々16の倍数)、
// 8+len が 8の奇数倍になる長さのsymbol名がここに当たる。
//
// **この2つは独立に効き、生存量の増加が足し合わさる。** そのため
// マージ後の窓は、どちらのブランチが単独で決めた値とも一致しない。
//   declaim側(feature/compiler-optimization)単独の窓: 79〜80KB → 80KBを採用していた
//   16byte切り上げ側(main)単独の窓:                  78〜80KB → 79KBを採用していた
//   **マージ後の窓: 81〜84KB**
// 両ブランチの値(79/80KB)はどちらもマージ後にはハングする。実際に80KBで
// runtime_testが無限ループした(os_panicがユニットテストでは抜けないため、
// CPU100%のまま止まらない)。
//
// 掃引結果(マージ後のツリー、2026-09-18):
//   80KB: ハング  — GC後も確保不能
//   81KB〜84KB: 5395 OK / 0 NG (82KBを採用していた)
//   85KB: isqrtテストの「GCが発火する」アサーションがNG
//   86KB: bignum加算テストのアサーションもNG
//
// [single-float] 2026-09-19: 組み込み関数を5個(%%SINGLE-FLOAT-P と
// float境界の定数4つ)追加したため、os_bootstrap直後の生存量がまた増え、
// 82KBはハングするようになった。**組み込みを1つ足すたびにここは動く**
// (memory: builtin_count_small_heap_calibration)。
//
// 掃引結果(single-float導入後、2026-09-19):
//   82KB: ハング — GC後も確保不能(4625件出たところで止まる)
//   83KB: 5416 OK / 0 NG
//   84KB: 5416 OK / 0 NG
//   85KB: 5416 OK / 0 NG   ← 採用(窓83〜86の中央寄り。ハング側へ3KB、GC未発火側へ2KB)
//   86KB: 5416 OK / 0 NG
//   87KB: isqrtテストの「GCが発火する」アサーションがNG
//   88KB: bignum加算テストのアサーションもNG
//
// 中央付近を採るのは、両端のどちらに寄っても壊れ方が違うためである。
// 小さすぎるとGC後も確保不能でハングし(os_panicがユニットテストでは無限ループ)、
// 大きすぎるとGCが発火せず「GCを跨いでも正しい」系のアサーションがNGになる。
// ハングのほうが切り分けに時間がかかるので、下側に寄せない。
//
// 値を変えるときのために可変にしてある。-DSMALL_HEAP_SIZE=... で上書きして
// 掃引すれば、「ぎりぎり通る値」を推測ではなく実測で決められる。
// 掃引するときは timeout を**コンテナの中**で効かせること。
// `timeout N docker run ...` は docker クライアントしか殺さず、ハングした
// コンテナが生き残って掃引が進まなくなる。
#ifndef SMALL_HEAP_SIZE
#define SMALL_HEAP_SIZE (85 * 1024)
#endif

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

/* ===================== disassembler用のアドレス分類 ===================== */

/** 1領域ぶんの境界を、内側と外側の両方から確認する。
    半開区間なので start は範囲内、end は**範囲外**でなければならない
    (ここを取り違えると、隣接する領域の先頭1バイトを誤分類する) */
static void check_region_boundaries(os_addr_region_t region, const char *label) {
    lisp_addr_t start = 0;
    lisp_addr_t end = 0;
    char msg[160];

    if (!os_addr_region_bounds(region, &start, &end)) {
        sprintf(msg, "%s: 境界が未確定なら start/end は 0 になる", label);
        assert(start == 0 && end == 0, msg);
        return;
    }

    sprintf(msg, "%s: start < end", label);
    assert(start < end, msg);

    sprintf(msg, "%s: start は範囲内", label);
    assert(os_classify_addr(start) == region, msg);

    sprintf(msg, "%s: end-1 は範囲内(上端の1バイト手前)", label);
    assert(os_classify_addr(end - 1) == region, msg);

    sprintf(msg, "%s: end は範囲外(半開区間の上端)", label);
    assert(os_classify_addr(end) != region, msg);

    sprintf(msg, "%s: start-1 は範囲外", label);
    assert(os_classify_addr(start - 1) != region, msg);
}

void test_os_classify_addr_boundaries() {
    /* Immobilized Space はカーネルイメージ内の静的配列なので、ヒープ初期化とは
       無関係に常に境界が確定している */
    check_region_boundaries(OS_ADDR_IMMOBILIZED, "immobilized");
    /* GCヒープは setup_heap() 済みなので確定している */
    check_region_boundaries(OS_ADDR_GC_HEAP, "gc-heap");
    /* .text はネイティブgccのユニットテストビルド(ELF)では取得できず未確定。
       os_addr_region_bounds が 0 を返し、check_region_boundaries は
       「start/end が 0」だけを確認する。実機での .text 判定は
       test/lisp/disassemble_test.lisp が確認する */
    check_region_boundaries(OS_ADDR_KERNEL_TEXT, "kernel-text");
}

void test_os_classify_addr_rejects_out_of_range() {
    assert(os_classify_addr(0) == OS_ADDR_UNKNOWN, "0 はどの領域にも属さない");
    assert(os_classify_addr((lisp_addr_t)~(UINT64)0) == OS_ADDR_UNKNOWN,
           "UINTPTR_MAX はどの領域にも属さない");
    /* Cスタック上のローカル変数は3領域のいずれでもない */
    int on_stack = 0;
    assert(os_classify_addr((lisp_addr_t)(void *)&on_stack) == OS_ADDR_UNKNOWN,
           "Cスタック上のアドレスはどの領域にも属さない");
}

void test_os_classify_addr_regions_do_not_overlap() {
    /* 領域同士が重なっていれば、判定順序によって結果が変わる設計上の誤りになる。
       確定している領域の組み合わせだけを総当たりで確認する */
    for (int a = OS_ADDR_KERNEL_TEXT; a <= OS_ADDR_GC_HEAP; a++) {
        for (int b = a + 1; b <= OS_ADDR_GC_HEAP; b++) {
            lisp_addr_t a_start = 0, a_end = 0, b_start = 0, b_end = 0;
            if (!os_addr_region_bounds((os_addr_region_t)a, &a_start, &a_end)) { continue; }
            if (!os_addr_region_bounds((os_addr_region_t)b, &b_start, &b_end)) { continue; }
            char msg[160];
            sprintf(msg, "領域 %d と %d が重なっていない", a, b);
            assert(a_end <= b_start || b_end <= a_start, msg);
        }
    }
}

void test_os_addr_region_name() {
    assert(strcmp(os_addr_region_name(OS_ADDR_KERNEL_TEXT), "kernel") == 0, "kernel の表示名");
    assert(strcmp(os_addr_region_name(OS_ADDR_IMMOBILIZED), "immobilized") == 0, "immobilized の表示名");
    assert(strcmp(os_addr_region_name(OS_ADDR_GC_HEAP), "gc-heap") == 0, "gc-heap の表示名");
    assert(os_addr_region_name(OS_ADDR_UNKNOWN) == 0, "UNKNOWN は名前を持たない(NULL)");
}

void test_os_classify_addr_matches_real_objects() {
    /* 実際に確保したオブジェクトとFunction Cellが、期待どおりの領域に落ちること。
       境界値だけでなく「本物がそこにある」ことを確認する */
    lisp_val_t cell = os_get_function_cell(os_make_symbol("CAR"), global_environment);
    assert(cell != nil, "CAR の Function Cell が引ける");
    assert(os_classify_addr((lisp_addr_t)(cell & ~TAG_MASK)) == OS_ADDR_IMMOBILIZED,
           "Function Cell は Immobilized Space にある");

    lisp_val_t cons = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
    assert(os_classify_addr((lisp_addr_t)(cons & ~TAG_MASK)) == OS_ADDR_GC_HEAP,
           "os_make_cons が返した cons は GCヒープにある");
}

/* [境界] 可変長オブジェクトの長さが端数になるケースを、確保 -> GC -> 内容確認まで
   通す。STRINGのサイズは 8+len なので、len が 0/7/8/9/15/16/17 のとき切り上げ前の
   要求は 8/15/16/17/23/24/25 byte になり、8byte切り上げだと 8/16/16/24/24/24/32 と
   「16の倍数でない値」が混ざる。そこが os_alloc_bytes / gc_to_alloc の切り上げ単位を
   8 から OS_HEAP_ALIGN へ変えた本命のケースである。

   確認するのは2点:
     1. 確保したアドレスが OS_HEAP_ALIGN 境界にあること(確保直後とGC後の両方)
     2. GCでコピーされた後も中身が壊れていないこと
   ALIGN_AUDIT ビルドでなくても回るよう、アドレス検査はこのテスト自身で行う。 */
static const UINT64 g_align_edge_lens[] = { 0, 1, 7, 8, 9, 15, 16, 17, 23, 24, 25 };
#define ALIGN_EDGE_CASES (sizeof(g_align_edge_lens) / sizeof(g_align_edge_lens[0]))

static void align_edge_fill(char *buf, UINT64 len) {
    for (UINT64 i = 0; i < len; i++) {
        buf[i] = (char)('a' + (i % 26));
    }
    buf[len] = '\0';
}

void test_heap_align_holds_for_variable_length_objects_across_gc() {
    setup_small_heap();

    lisp_val_t list = nil;
    GC_PROTECT(list);

    int alloc_misaligned = 0;
    for (UINT64 c = 0; c < ALIGN_EDGE_CASES; c++) {
        char piece[32];
        align_edge_fill(piece, g_align_edge_lens[c]);
        lisp_val_t str = os_make_string(piece);
        if (((str & ~TAG_MASK) & (OS_HEAP_ALIGN - 1)) != 0) { alloc_misaligned++; }
        list = os_make_cons(str, list);
        if (((list & ~TAG_MASK) & (OS_HEAP_ALIGN - 1)) != 0) { alloc_misaligned++; }
    }
    assert(alloc_misaligned == 0,
           "長さ0/1/7/8/9/15/16/17/23/24/25のstringとその間のconsが、確保直後にすべてOS_HEAP_ALIGN境界にある");

    /* ガベージを作りながらGCを確実に発火させる。GCを跨いだ後に、
       同じオブジェクトが再びすべて境界に乗っていることを確かめる */
    UINT64 gc_before = os_gc_collect_count();
    for (int i = 0; i < 4000; i++) {
        char piece[4] = { 'x', (char)('0' + (i % 10)), 'y', '\0' };
        os_make_cons(os_make_string(piece), nil);
    }
    UINT64 gc_after = os_gc_collect_count();
    assert(gc_after > gc_before,
           "境界テストの途中でos_gc_collectが実際に発火する(発火しなければコピー後の検査に意味がない)");

    int after_misaligned = 0;
    int content_mismatch = 0;
    UINT64 seen = 0;
    /* listはconsで積んだので、長さ列の逆順に並んでいる */
    for (lisp_val_t p = list; p != nil; p = cc_cdr(p)) {
        UINT64 expected_len = g_align_edge_lens[ALIGN_EDGE_CASES - 1 - seen];
        lisp_val_t str = cc_car(p);
        if (((p   & ~TAG_MASK) & (OS_HEAP_ALIGN - 1)) != 0) { after_misaligned++; }
        if (((str & ~TAG_MASK) & (OS_HEAP_ALIGN - 1)) != 0) { after_misaligned++; }

        char expected[32];
        align_edge_fill(expected, expected_len);
        char buf[32];
        os_string_to_cstr(str, buf, sizeof(buf));
        if (strcmp(buf, expected) != 0) { content_mismatch++; }
        seen++;
    }
    assert(seen == ALIGN_EDGE_CASES, "GC後もリストの要素数が変わらない");
    assert(after_misaligned == 0,
           "GCでTo空間へコピーされた後も、端数長のstringとconsがすべてOS_HEAP_ALIGN境界にある");
    assert(content_mismatch == 0, "GCでコピーされた後も端数長のstringの内容が一致する");
}

/* [境界] From/To両空間の先頭が OS_HEAP_ALIGN に乗っていること、および2つの
   半空間が同サイズであることを、境界に乗っていないbaseを渡して確かめる。
   os_heap_initは受け取ったbaseを切り上げる責務を負っている(呼び出し元の
   os_boot_alloc_finalizeだけに任せると、mallocから直接渡すこの経路が漏れる)。 */
void test_heap_init_aligns_both_half_spaces() {
    UINT64 total = 1024 * 1024;
    UINT8 *raw = (UINT8 *)malloc(total);
    assert(raw != NULL, "半空間境界テスト用のヒープをmallocで確保できる");

    /* 意図的に8 mod 16のbaseを作る。mallocは16境界を返すので+8でずらす */
    UINT64 skewed_base = (UINT64)raw + 8;
    UINT64 skewed_size = total - 8;
    os_heap_init(skewed_base, skewed_size);

    UINT64 from_start = 0, from_end = 0, to_start = 0, to_end = 0;
    os_heap_bounds_for_test(&from_start, &from_end, &to_start, &to_end);

    assert((from_start & (OS_HEAP_ALIGN - 1)) == 0,
           "baseが8 mod 16でもFrom空間の先頭はOS_HEAP_ALIGN境界へ切り上げられる");
    assert((to_start & (OS_HEAP_ALIGN - 1)) == 0,
           "To空間の先頭もOS_HEAP_ALIGN境界にある");
    assert((from_end - from_start) == (to_end - to_start),
           "From空間とTo空間のサイズが等しい(フリップしても容量が減らない)");
    assert(((from_end - from_start) & (OS_HEAP_ALIGN - 1)) == 0,
           "半空間のサイズがOS_HEAP_ALIGNの倍数である");
    assert(from_start >= skewed_base && to_end <= skewed_base + skewed_size,
           "2つの半空間は渡された領域の内側に収まっている");

    free(raw);
}

int main(int argc, char** argv) {
   (void)argc;
   (void)argv;
   test_os_make_fixnum();
   test_os_boot_alloc_advances_bump_pointer_with_alignment();
   test_os_boot_alloc_finalize_returns_remaining_region_after_usage();

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
   test_os_classify_addr_boundaries();
   test_os_classify_addr_rejects_out_of_range();
   test_os_classify_addr_regions_do_not_overlap();
   test_os_addr_region_name();
   test_os_classify_addr_matches_real_objects();
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
   test_tag_table_covers_all_sixteen_values();
   test_tag_immediate_pointer_split_is_bit0();
   test_unassigned_and_reserved_tags_are_not_followed_by_gc();
   test_immediate_encode_decode_round_trip();
   test_char_high_code_points_do_not_corrupt_upper_bits();
   test_immediates_survive_gc_unchanged();
   test_single_float_bit_pattern_round_trip();
   test_single_float_is_a_number_and_a_float();
   test_single_float_limits_come_from_c();
   test_single_floats_survive_gc_unchanged();
   test_float_contagion_two_arg_result_types();
   test_single_float_arithmetic_allocates_nothing();
   test_float_comparison_widens_to_double();
   test_equal_still_requires_matching_tags();
   test_gc_string_with_forward_tag_colliding_length_is_not_misdetected();
   test_gc_instance_survives();
   test_gc_circular_cons_list_does_not_hang();
   test_gc_reclaims_unreferenced_garbage();

   test_gc_fires_during_bignum_addition_and_result_is_correct();
   test_gc_fires_during_gcd_and_isqrt_and_results_are_correct();
   test_gc_fires_during_vector_construction_and_elements_are_preserved();
   test_gc_fires_during_string_append_and_result_is_correct();

   test_heap_align_holds_for_variable_length_objects_across_gc();
   /* os_heap_initを別のヒープで呼び直すので、他のテストより後に置く */
   test_heap_init_aligns_both_half_spaces();

   return g_test_failed ? 1 : 0;
}
