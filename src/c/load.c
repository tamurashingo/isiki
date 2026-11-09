#include "load.h"
#include "runtime.h"
#include "lisp.h"
#include "stream.h"
#include "reader.h"
#include "eval.h"
#include "process.h"
#include "print.h"

/** LOADの引数として渡せるパスの最大長(NUL終端込み) */
#define LOAD_PATH_MAX 256
/** stream open失敗時などのエラーメッセージ用バッファサイズ */
#define LOAD_ERR_MSG_MAX 128

/** report-conditionの出力を受ける固定長バッファ。これを超えるメッセージは途中で切れる */
#define LOAD_REPORT_MAX 256

/**
 * [P2] エラーで打ち切られたフォームの理由を表示する。
 *
 * **cc_loadは評価結果を捨てるので、以前はロード中にフォームが打ち切られても
 * 何も出なかった。** どのファイルで落ちたかが分かるよう読み込み元の名前を前置きする
 * (9P経由ならパスそのもの)。
 * 打ち切られたフォームは飛ばして次のフォームへ進む(ロード自体は続行する)。
 *
 * report-conditionのメッセージが得られなければ、REPLと同じく生の値の印字へ落とす。
 */
static void print_load_abort(const char *name, lisp_val_t condition, lisp_val_t env) {
    process_t *proc = get_current_process();
    proc->stdout_buffer->write_string(proc->stdout_buffer, "load: ");
    proc->stdout_buffer->write_string(proc->stdout_buffer, name);
    proc->stdout_buffer->write_string(proc->stdout_buffer, ": ");

    char msg[LOAD_REPORT_MAX];
    if (os_condition_report_cstr(condition, env, msg, sizeof(msg))) {
        proc->stdout_buffer->write_string(proc->stdout_buffer, msg);
    } else {
        os_print(condition, proc->stdout_buffer);
    }
    proc->stdout_buffer->write_char(proc->stdout_buffer, '\n');
}

lisp_val_t os_load_from_stream(os_stream_t *stream, const char *name, lisp_val_t env,
                               int *out_aborted_count) {
    // envはループの複数回のイテレーションを跨いで再利用される。os_eval_top_level経由で
    // 呼び出したform(load対象のファイル自体が(defun ...)等でヒープ確保を伴う)の評価中に
    // GCが走るとenvが指す先(例: global_environment)が再配置されるため、GC_PROTECTで
    // 毎回のGCで自動的に追随させないと、以後の全formが再配置前の古いアドレス(GCにより
    // 転送マーカーで上書き済み)を参照してしまう
    GC_PROTECT(env);

    // [M2-T0 / #196] **輪は反復ごとに包みから実体を取り直す。**
    // MAGIC_STREAMに包まれたストリーム(ディスク経由のopen-input-streamが作る
    // ものなど)のos_stream_t本体は、GCのたびに別アドレスへ複製される
    // (gc_relocate_stream、runtime.c)。輪は毎反復でフォームを評価する=確保するので、
    // 引数で受け取った生ポインタを輪の全体で使い回すと、1回目のGCでその参照は
    // 古い複製を指し、**2回目のGCでその古い複製が別オブジェクトの複製先として
    // 上書きされる。** 以後は化けた中身から読み続けるので、後ろのフォームが
    // 評価されない / 二度評価される / 偽の構文エラーになる(= 静かな破壊。
    // test/c/load_test.cのtest_load_from_wrapped_stream_survives_gcが陽性対照)。
    //
    // 包まれていない(self_handleがnil)ストリーム、すなわち9p経由のcc_loadと
    // カーネル同梱の読み込み(kernel.c)はCのローカル/静的領域にあってGCで
    // 動かないので、従来どおり引数の生ポインタをそのまま使う(挙動は変わらない)
    lisp_val_t handle = stream->self_handle;
    GC_PROTECT(handle);

    for (;;) {
        os_stream_t *s = (handle != nil) ? os_stream_from_handle(handle) : stream;
        lisp_val_t form = os_read_stream(s);

        if (form == g_sym_read_error) {
            os_stream_close((handle != nil) ? os_stream_from_handle(handle) : stream);
            return os_signal_io_error("load", name, "syntax error", env);
        }

        if (form == nil) {
            // [読み取り] os_read_streamのnilは「クリーンEOF」と「I/Oエラー」を
            // 区別しない(reader.h)。nilを受けたらstream->errorを確認する規約。
            // メモリ同梱の文字列入力ではerrorは立たないが、この関数は9P経由と
            // 共有するので判定は残す。g_sym_read_errorとnilの判別順も変えない
            // (nilはTAG_CONSを持つ。documents/pitfalls.md)
            if (s->error) {
                os_stream_close(s);
                return os_signal_io_error("load", name, "I/O error", env);
            }
            break;
        }

        // [P2] REPLと同じく、エラーで打ち切られたフォームの中での
        // switch-environment(proc->envの恒久的な書き換え)は無かったことにする。
        // この関数自身はenv引数のもとで評価するのでproc->envを読まないが、
        // proc->envはload後のREPLに残るため、やりかけの切り替えを残さない。
        // 正常終了したフォームのswitch-environmentは従来どおり有効なままにする
        process_t *proc = get_current_process();
        lisp_val_t saved_proc_env = proc->env;
        GC_PROTECT(saved_proc_env);

        int aborted = 0;
        lisp_val_t result = os_eval_top_level_ex(form, env, &aborted);
        GC_PROTECT(result);
        if (aborted) {
            // saved_proc_envが0のときは「まだ遅延生成されていなかった」ので戻さない。
            // 0へ書き戻すと、フォームの中で生成されたprocess environmentが捨てられ、
            // 次の%%current-environmentが同名の環境をもう1つ*environments*へ足してしまう
            if (saved_proc_env != 0) {
                // procはGCで動かないが、os_eval_top_level_exの中でGCが走っていれば
                // proc->envは書き換わっている。取り直してから戻す
                get_current_process()->env = saved_proc_env;
            }
            // [L1] 打ち切りを数えて呼び出し元へ返す。この関数は打ち切られたフォームを
            // 飛ばして次へ進み、最後にg_sym_tを返すので、**戻り値だけでは
            // 「半分失敗した」が分からない**。同梱の読み込み(M1-T4)は1件でも
            // あればboot errorで止める必要があるため、件数を別経路で渡す
            if (out_aborted_count != 0) {
                (*out_aborted_count)++;
            }
            print_load_abort(name, result, env);
        }
    }

    // 輪の中でGCが走っている可能性があるので、閉じる前にも取り直す
    os_stream_close((handle != nil) ? os_stream_from_handle(handle) : stream);
    return g_sym_t;
}

lisp_val_t cc_load(lisp_val_t args, lisp_val_t env) {
    /* [M2-T0 / #196] **環境の値を開く前に守る。**
       envは読んだ各フォームを評価する環境で、os_load_from_streamへ渡す値である。
       いまの9pのopenは確保しないので守らなくても安全だったが、開き方を
       マウント経由にすると(#193)os_mount_resolveがos_make_symbolを呼び、
       FAT経路はLispを呼び戻すので**openの手前で確保が起きる。**
       ここでGCが走ってenvが再配置されると、以後の全フォームが再配置前の
       古い環境へ書き込まれる(load.c:45-50に同型の事故が記録されている)。
       開き方を変えるのは#193だが、**守りは先に置く**(それが#196の趣旨である) */
    GC_PROTECT(env);

    char path[LOAD_PATH_MAX];
    os_string_to_cstr(cc_car(args), path, sizeof(path));

    char err_msg[LOAD_ERR_MSG_MAX];
    os_stream_t stream;
    if (!os_stream_open_9p_file(&stream, path, err_msg, sizeof(err_msg))) {
        /* [P4-4] **load は ISLisp 仕様に無い**(実装独自)ので error-id の指定も無い。
           open-input-file に揃えて <simple-error> を signal する
           (クラスの選択理由は os_signal_io_error のコメント参照)。
           以前はここで自前に1行表示してから EVAL-ERROR を返していたが、
           condition のメッセージが同じ内容を運ぶので表示はやめる。
           握り潰されずトップレベルまで上がれば repl.c / cc_load が報告する */
        return os_signal_io_error("load", path, err_msg, env);
    }

    // 打ち切り件数は受け取らない(0を渡す)。LOADの戻り値は従来どおり
    // 「打ち切りがあってもg_sym_t」で、件数を使うのは同梱の読み込み(M1-T4)だけ
    return os_load_from_stream(&stream, path, env, 0);
}

void os_register_load(void) {
    os_set_function(os_make_symbol("LOAD"), os_make_native_function((lisp_addr_t)(void *)cc_load), global_environment);
}
