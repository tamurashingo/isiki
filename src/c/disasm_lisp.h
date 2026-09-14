#ifndef _DISASM_LISP_H_
#define _DISASM_LISP_H_

#include "types.h"

/**
 * 逆アセンブラのprimitive(%%DISASM-CODE-BASE / %%DISASM-CODE-LEN /
 * %%DISASM-ENTRY-OFFSET / %%DISASM-ITEM)をglobal_environmentへ登録する。
 * za.c→runtime.cの一方向依存と同じ理由で、os_bootstrap()以降のブート列から呼ぶ。
 */
void os_register_disasm(void);

#endif /* _DISASM_LISP_H_ */
