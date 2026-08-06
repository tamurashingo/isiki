#ifndef _PRINT_H_
#define _PRINT_H_

#include "types.h"
#include "runtime.h"
#include "framebuffer.h"

/**
 * val を TAG に応じて fb に表示する。
 * 表示した val 自身を返す(REPLでの `(print (eval (read)))` 的な合成のため)。
 * @param val 表示するLisp値
 * @param fb 表示先のframe buffer
 * @return val 自身
 */
lisp_val_t os_print(lisp_val_t val, frame_buffer *fb);

#endif /* _PRINT_H_ */
