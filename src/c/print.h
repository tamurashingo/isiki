#ifndef _PRINT_H_
#define _PRINT_H_

#include "types.h"
#include "runtime.h"
#include "framebuffer.h"

/**
 * print系の出力先を抽象化する1文字書き込みシンク。frame_buffer/os_stream_t等、
 * 「1文字書き込める」ものであれば何でもctxに詰めて使える。
 */
typedef struct {
    void *ctx;
    void (*write_char)(void *ctx, UINT8 c);
} os_char_sink_t;

/**
 * val を TAG に応じて sink に表示する。
 * @param val 表示するLisp値
 * @param sink 表示先のシンク
 * @param escaped 真なら prin1 相当(STRINGをダブルクオートで囲む)、
 *                偽なら princ 相当(STRINGの内容のみをそのまま出力する)
 */
void os_print_to_sink(lisp_val_t val, os_char_sink_t *sink, int escaped);

/**
 * val を TAG に応じて fb に表示する(prin1相当、escaped=1でos_print_to_sinkを呼ぶ薄いラッパー)。
 * 表示した val 自身を返す(REPLでの `(print (eval (read)))` 的な合成のため)。
 * @param val 表示するLisp値
 * @param fb 表示先のframe buffer
 * @return val 自身
 */
lisp_val_t os_print(lisp_val_t val, frame_buffer *fb);

#endif /* _PRINT_H_ */
