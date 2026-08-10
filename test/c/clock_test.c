#include <stdlib.h>
#include <stdint.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "lisp.h"
#include "clock.h"

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

// interrupt.c/kernel.cはリンクしないため、clock.cが参照する
// get_tick_counter/kernel_get_boot_epoch_secondsをテスト用固定値で置き換える
static UINT64 g_fake_tick_counter = 350;
static UINT64 g_fake_boot_epoch_seconds = 3000000000ULL;

uint64_t get_tick_counter(void) {
    return g_fake_tick_counter;
}

UINT64 kernel_get_boot_epoch_seconds(void) {
    return g_fake_boot_epoch_seconds;
}

#define HEAP_SIZE (1024 * 1024)

static void setup_heap() {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
}

void test_get_internal_real_time_returns_tick_counter() {
    lisp_val_t result = primitive_get_internal_real_time(nil, nil);
    assert(result >> 3 == g_fake_tick_counter, "get-internal-real-timeはtickカウンタをそのまま返す");
}

void test_get_internal_run_time_returns_tick_counter() {
    lisp_val_t result = primitive_get_internal_run_time(nil, nil);
    assert(result >> 3 == g_fake_tick_counter, "get-internal-run-timeはget-internal-real-timeと同じtickカウンタを流用する");
}

void test_internal_time_units_per_second_returns_100() {
    lisp_val_t result = primitive_internal_time_units_per_second(nil, nil);
    assert(result >> 3 == 100, "internal-time-units-per-secondはPITの約100Hzに対応する100を返す");
}

void test_get_universal_time_adds_elapsed_seconds_to_boot_epoch() {
    lisp_val_t result = primitive_get_universal_time(nil, nil);
    UINT64 expected = g_fake_boot_epoch_seconds + (g_fake_tick_counter / 100);
    assert(result >> 3 == expected, "get-universal-timeは起動時UTCにtickカウンタ由来の経過秒数を加算する");
}

void test_os_register_clock_binds_symbols() {
    os_register_clock();
    lisp_val_t fn = os_get_function(os_make_symbol("GET-UNIVERSAL-TIME"), global_environment);
    assert(fn != nil, "os_register_clockはGET-UNIVERSAL-TIMEをglobal_environmentへ束縛する");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    setup_heap();

    test_get_internal_real_time_returns_tick_counter();
    test_get_internal_run_time_returns_tick_counter();
    test_internal_time_units_per_second_returns_100();
    test_get_universal_time_adds_elapsed_seconds_to_boot_epoch();
    test_os_register_clock_binds_symbols();

    return g_test_failed ? 1 : 0;
}
