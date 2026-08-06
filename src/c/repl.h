#ifndef _REPL_H_
#define _REPL_H_

#include "process.h"

/**
 * proc に対して READ→EVAL→PRINT を1サイクル実行する。
 * proc の環境(env)は初回呼び出し時に global_environment の子環境として遅延生成される。
 * 1行の入力を使い切っている場合、os_read が内部で os_wait_for_more_input を通じて
 * 次の入力行が確定するまでブロックする。
 * @param proc 実行対象のプロセス
 */
void os_repl_step(process_t *proc);

#endif /* _REPL_H_ */
