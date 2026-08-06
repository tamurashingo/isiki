#include "repl.h"
#include "runtime.h"
#include "reader.h"
#include "eval.h"
#include "print.h"
#include "framebuffer.h"

/**
 * proc に対して READ→EVAL→PRINT を1サイクル実行する。
 * proc の環境(env)は初回呼び出し時に global_environment の子環境として遅延生成される。
 * 1行の入力を使い切っている場合、os_read が内部で os_wait_for_more_input を通じて
 * 次の入力行が確定するまでブロックする。
 * @param proc 実行対象のプロセス
 */
void os_repl_step(process_t *proc) {
    if (proc->env == 0) {
        proc->env = os_make_environment(os_make_symbol(proc->name), global_environment);
    }

    lisp_val_t form = os_read(proc);

    if (form == nil) {
        // 1行分を使い切った(次の入力を待つのは次回呼び出し時のos_read/ensure_dataに委ねる)
        return;
    }

    lisp_val_t result = os_eval(form, proc->env);

    os_print(result, proc->stdout_buffer);
    proc->stdout_buffer->write_char(proc->stdout_buffer, '\n');
}
