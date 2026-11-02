#include "repl.h"
#include "runtime.h"
#include "reader.h"
#include "eval.h"
#include "print.h"
#include "framebuffer.h"

/** From空間の使用率がこれを超えたら、次のセーフポイントでGCを起動する */
#define GC_TRIGGER_HEAP_RATIO 0.8

/** report-conditionの出力を受ける固定長バッファ。これを超えるメッセージは途中で切れる
    (Lisp側の%report-condition-stringが使うcreate-string-output-streamの容量
    STREAM_STRING_OUTPUT_CAP=1024が実質の上限で、こちらはさらにその手前で切る) */
#define REPL_REPORT_MAX 256

/**
 * [P2] 打ち切られたフォームのconditionを、report-condition経由の自然言語メッセージで
 * 表示する。表示できたら1を返す。0のときは呼び出し元が従来どおりos_printへ落とす
 * (判定の中身はos_condition_report_cstr(eval.c)のコメント参照。load.cと共用する)。
 */
static int print_condition_report(process_t *proc, lisp_val_t condition) {
    char msg[REPL_REPORT_MAX];
    if (!os_condition_report_cstr(condition, proc->env, msg, sizeof(msg))) {
        return 0;
    }
    proc->stdout_buffer->write_string(proc->stdout_buffer, msg);
    return 1;
}

/**
 * proc に対して READ→EVAL→PRINT を1サイクル実行する。
 * proc の環境(env)は初回呼び出し時に global_environment の子環境として遅延生成される。
 * 1行の入力を使い切っている場合、os_read が内部で os_wait_for_more_input を通じて
 * 次の入力行が確定するまでブロックする。
 * @param proc 実行対象のプロセス
 */
void os_repl_step(process_t *proc) {
    if (proc->env == 0) {
        proc->env = os_make_process_environment(proc->name);
    }

    lisp_val_t form = os_read(proc);

    if (form == nil) {
        // 1行分を使い切った(次の入力を待つのは次回呼び出し時のos_read/ensure_dataに委ねる)
        return;
    }

    // [P2] フォームの評価開始時のenvironmentを控える。**打ち切られたときだけ**戻す。
    // 正常終了したフォームの中のswitch-environmentは従来どおり有効なままにする
    // (switch-environmentはproc->envを恒久的に書き換える仕様のため)。
    // saved_envはos_eval_top_level_ex(任意の深さの評価。GCを誘発する)を跨いで
    // 生存するのでGC_PROTECTが要る(documents/pitfalls.md 原則4)
    lisp_val_t saved_env = proc->env;
    GC_PROTECT(saved_env);

    int aborted = 0;
    lisp_val_t result = os_eval_top_level_ex(form, proc->env, &aborted);
    GC_PROTECT(result);

    if (aborted) {
        // エラーで打ち切られたフォームの中でswitch-environmentしていた場合、
        // その切り替えは「やりかけ」なので無かったことにする
        proc->env = saved_env;
        // 打ち切りはreport-conditionで理由を出す。出せなければ従来の表示へ落とす
        if (!print_condition_report(proc, result)) {
            os_print(result, proc->stdout_buffer);
        }
    } else {
        os_print(result, proc->stdout_buffer);
    }
    proc->stdout_buffer->write_char(proc->stdout_buffer, '\n');

    // ここ(os_eval_top_level_exの呼び出しの間、次のos_readより前)がGCを安全に起動できる
    // セーフポイント: proc->envはos_gc_register_root経由で正しく書き換えられ、
    // このフレームに残るlisp_val_t(saved_env/result)はGC_PROTECT済みである
    if (os_heap_used_ratio() > GC_TRIGGER_HEAP_RATIO) {
        os_gc_collect();
    }
}
