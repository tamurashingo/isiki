#include "clock.h"
#include "runtime.h"
#include "interrupt.h"
#include "kernel.h"

/** PITの分周設定(約100Hz)に対応する1秒あたりのinternal time unit数 */
#define TICKS_PER_SECOND 100

lisp_val_t primitive_get_universal_time(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    UINT64 elapsed_seconds = get_tick_counter() / TICKS_PER_SECOND;
    return os_make_fixnum(kernel_get_boot_epoch_seconds() + elapsed_seconds);
}

lisp_val_t primitive_get_internal_real_time(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    return os_make_fixnum(get_tick_counter());
}

lisp_val_t primitive_get_internal_run_time(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    return os_make_fixnum(get_tick_counter());
}

lisp_val_t primitive_internal_time_units_per_second(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    return os_make_fixnum(TICKS_PER_SECOND);
}

void os_register_clock(void) {
    os_set_function(os_make_symbol("GET-UNIVERSAL-TIME"),
                     os_make_native_function((lisp_addr_t)(void *)primitive_get_universal_time),
                     global_environment);
    os_set_function(os_make_symbol("GET-INTERNAL-REAL-TIME"),
                     os_make_native_function((lisp_addr_t)(void *)primitive_get_internal_real_time),
                     global_environment);
    os_set_function(os_make_symbol("GET-INTERNAL-RUN-TIME"),
                     os_make_native_function((lisp_addr_t)(void *)primitive_get_internal_run_time),
                     global_environment);
    os_set_function(os_make_symbol("INTERNAL-TIME-UNITS-PER-SECOND"),
                     os_make_native_function((lisp_addr_t)(void *)primitive_internal_time_units_per_second),
                     global_environment);
}
