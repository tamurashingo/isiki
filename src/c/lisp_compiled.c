#include "runtime.h"

/**
 * トランスパイラの生成物置き場所の仮配置(M1)。M2以降でtranspile.lispの
 * 実際の出力に置き換わる。
 * @param args 未使用
 * @param env 未使用
 * @return fixnum 42
 */
lisp_val_t lisp_ll_transpiler_placeholder(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    return os_make_fixnum(42);
}
