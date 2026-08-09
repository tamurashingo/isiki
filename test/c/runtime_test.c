#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "lisp.h"

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
    const char* s = (const char *)(str + 8);
    assert(strncmp(s, "hello world", (UINT64 *)str), "word0のstringは\"hello world\"である");
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
    assert(obj[0] == MAGIC_CLASS, "word0はMAGIC_CLASS");

    assert(primitive_class_name(os_make_cons(class, nil), nil) == name, "(%%class-name c)は渡したnameをそのまま返す");
    assert(primitive_class_supers(os_make_cons(class, nil), nil) == supers, "(%%class-supers c)は渡したsupersをそのまま返す");
    assert(primitive_class_slots(os_make_cons(class, nil), nil) == slots, "(%%class-slots c)は渡したslotsをそのまま返す");

    assert(primitive_classp(os_make_cons(class, nil), nil) == g_sym_t, "(%%classp c)はtを返す");
    assert(primitive_classp(os_make_cons(os_make_fixnum(1), nil), nil) == nil, "(%%classp 1)はnilを返す");
}

void test_primitive_make_instance_raw_and_accessors() {
    lisp_val_t class = os_make_instance(MAGIC_CLASS, os_make_symbol("POINT"), nil, nil);
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

int main(int argc, char** argv) {
   test_os_make_fixnum();

   setup_heap();
   test_os_make_cons();
   test_os_make_char();
   test_os_make_string();
   test_os_make_symbol();
   test_os_make_symbol_prefix_is_not_confused();
   test_os_get_variable();
   test_os_get_function();
   test_os_make_fixnum_signed();
   test_os_make_integer_promotes_to_bignum();
   test_primitive_add_signed_and_bignum();
   test_primitive_subtract_unary_and_signed();
   test_primitive_multiply_signed_and_bignum();
   test_primitive_divide_signed();
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
   test_primitive_symbolp();
   test_primitive_consp();
   test_primitive_eql();
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
   test_primitive_make_instance_raw_and_accessors();

   return g_test_failed ? 1 : 0;
}
