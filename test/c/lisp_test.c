#include <stdlib.h>
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

// cc_car/cc_cdr はヒープ確保とnilの初期化が前提なので、
// 各テスト実行前に heap_init と boot を済ませておく
static void setup_heap() {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
}

void test_cc_car() {
  // (cons 1 2) の car は 1
  lisp_val_t c1 = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
  assert(cc_car(c1) == os_make_fixnum(1), "(cons 1 2) の car は 1");

  // (cons "hello" "world") の car は "hello"
  lisp_val_t hello = os_make_string("hello");
  lisp_val_t world = os_make_string("world");
  lisp_val_t c2 = os_make_cons(hello, world);
  assert(cc_car(c2) == hello, "(cons \"hello\" \"world\") の car は \"hello\"");

  // (cons (cons 1 2) 3) の car は cons
  lisp_val_t inner = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
  lisp_val_t outer = os_make_cons(inner, os_make_fixnum(3));
  lisp_val_t outer_car = cc_car(outer);
  assert(outer_car == inner, "(cons (cons 1 2) 3) の car は (cons 1 2) 自身");
  assert((outer_car & TAG_MASK) == TAG_CONS, "(cons (cons 1 2) 3) の car は TAG_CONS を持つ");

  //   その car は 1
  assert(cc_car(outer_car) == os_make_fixnum(1), "(cons (cons 1 2) 3) の car の car は 1");

  // nil の car は nil
  assert(cc_car(nil) == nil, "nil の car は nil");
}

void test_cc_cdr() {
  // (cons 1 2) の cdr は 2
  lisp_val_t c1 = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
  assert(cc_cdr(c1) == os_make_fixnum(2), "(cons 1 2) の cdr は 2");

  // (cons "hello" "world") の cdr は "world"
  lisp_val_t hello = os_make_string("hello");
  lisp_val_t world = os_make_string("world");
  lisp_val_t c2 = os_make_cons(hello, world);
  assert(cc_cdr(c2) == world, "(cons \"hello\" \"world\") の cdr は \"world\"");

  // (cons (cons 1 2) 3) の cdr は 3
  lisp_val_t inner = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
  lisp_val_t outer = os_make_cons(inner, os_make_fixnum(3));
  assert(cc_cdr(outer) == os_make_fixnum(3), "(cons (cons 1 2) 3) の cdr は 3");

  // nil の cdr は nil
  assert(cc_cdr(nil) == nil, "nil の cdr は nil");
}


void test_cc_assoc_eq() {
    lisp_val_t cons_sym1 = os_make_cons(os_make_symbol("sym1"), os_make_fixnum(100));
    lisp_val_t cons_sym2 = os_make_cons(os_make_symbol("sym2"), os_make_fixnum(200));
    lisp_val_t cons_fixnum3 = os_make_cons(os_make_fixnum(3), os_make_fixnum(300));
    lisp_val_t cons_fixnum4 = os_make_cons(os_make_fixnum(4), os_make_fixnum(400));

    lisp_val_t alist = os_make_cons(cons_sym1,
                          os_make_cons(cons_sym2,
                            os_make_cons(cons_fixnum3,
                              os_make_cons(cons_fixnum4, nil))));

    // symbol で値が取得できること
    lisp_val_t found_sym1 = cc_assoc_eq(os_make_symbol("sym1"), alist);
    assert(found_sym1 == cons_sym1, "symbol sym1 で assoc すると (sym1 . 100) が見つかる");
    assert(cc_cdr(found_sym1) == os_make_fixnum(100), "見つかった pair の cdr は 100");

    lisp_val_t found_sym2 = cc_assoc_eq(os_make_symbol("sym2"), alist);
    assert(found_sym2 == cons_sym2, "symbol sym2 で assoc すると (sym2 . 200) が見つかる");
    assert(cc_cdr(found_sym2) == os_make_fixnum(200), "見つかった pair の cdr は 200");

    // fixnum で値が取得できること
    lisp_val_t found_fixnum3 = cc_assoc_eq(os_make_fixnum(3), alist);
    assert(found_fixnum3 == cons_fixnum3, "fixnum 3 で assoc すると (3 . 300) が見つかる");
    assert(cc_cdr(found_fixnum3) == os_make_fixnum(300), "見つかった pair の cdr は 300");

    lisp_val_t found_fixnum4 = cc_assoc_eq(os_make_fixnum(4), alist);
    assert(found_fixnum4 == cons_fixnum4, "fixnum 4 で assoc すると (4 . 400) が見つかる");
    assert(cc_cdr(found_fixnum4) == os_make_fixnum(400), "見つかった pair の cdr は 400");

    // 存在しない symbol の場合 nil が返ること
    lisp_val_t not_found_sym = cc_assoc_eq(os_make_symbol("sym3"), alist);
    assert(not_found_sym == nil, "存在しない symbol sym3 で assoc すると nil が返る");

    // 存在しない fixnum の場合 nil が返ること
    lisp_val_t not_found_fixnum = cc_assoc_eq(os_make_fixnum(999), alist);
    assert(not_found_fixnum == nil, "存在しない fixnum 999 で assoc すると nil が返る");
}

void test_cc_set_cdr() {
    lisp_val_t c1 = os_make_cons(os_make_fixnum(1), os_make_fixnum(2));
    cc_set_cdr(c1, os_make_fixnum(99));
    assert(cc_cdr(c1) == os_make_fixnum(99), "cc_set_cdrで書き換えたcdrはcc_cdrで読み直せる");
    assert(cc_car(c1) == os_make_fixnum(1), "cc_set_cdrはcarには影響しない");

    // 自己参照させて循環リストの起点を作れること
    lisp_val_t c2 = os_make_cons(os_make_fixnum(42), nil);
    cc_set_cdr(c2, c2);
    assert(cc_cdr(c2) == c2, "cc_set_cdrで自己参照させると循環する");
}

int main(int argc, char** argv) {
   (void)argc;
   (void)argv;

   setup_heap();

   test_cc_car();
   test_cc_cdr();
   test_cc_assoc_eq();
   test_cc_set_cdr();

   return g_test_failed ? 1 : 0;
}
