#include "load.h"
#include "runtime.h"
#include "lisp.h"
#include "stream.h"
#include "reader.h"
#include "eval.h"
#include "process.h"
#include "print.h"
#include "mount.h"
#include "stream_lisp.h"

/** LOADの引数として渡せるパスの最大長(NUL終端込み) */
#define LOAD_PATH_MAX 256
/** stream open失敗時などのエラーメッセージ用バッファサイズ */
#define LOAD_ERR_MSG_MAX 128

/** report-conditionの出力を受ける固定長バッファ。これを超えるメッセージは途中で切れる */
#define LOAD_REPORT_MAX 256

/** 直列線へ出す1行を組むバッファ。名前(最大LOAD_PATH_MAX)と理由(最大LOAD_REPORT_MAX)と
    前置きが入る。あふれるぶんは切り捨てる(diag_appendが容量を見る) */
#define LOAD_DIAG_MAX 640

/* ===================== [#197] 打ち切りの集計 =====================
   **ここが#197の真因の置き場である。** 加算は以前os_load_from_streamの
   `if (out_aborted_count != 0)` の中にあった = **0を渡す呼び元では加算自体が
   起きていなかった**(捨てられていたのではなく、数えられていなかった)。
   0を渡していたのはcc_loadの3経路(相対 = 9P / マウント → 9P / FAT経由)、
   つまり**通常のloadのすべて**である。

   そこで総数は呼び元に依存しない静的変数へ積む。**呼び元が数えるかどうかと
   無関係に数える**のが要点で、これなら新しい呼び元が足されても
   「数え先を渡し忘れたので数えない」が起きない(原則6の形を作らない)。 */
static int g_load_aborted_total = 0;

/** 診断の1行の出し口。0なら何も出さない(os_load_set_diag_writerの宣言を参照) */
static void (*g_load_diag_writer)(const char *) = 0;

/** bufのpos以降へsを継ぎ足し、新しいposを返す。容量を超えるぶんは捨ててNUL終端する
    (kernel.cのboot_msg_appendと同じ形。あちらはstaticなので持ってこられない) */
static UINT32 diag_append(char *buf, UINT32 pos, const char *s) {
    while (*s != '\0' && pos + 1 < LOAD_DIAG_MAX) {
        buf[pos++] = *s++;
    }
    buf[pos] = '\0';
    return pos;
}

/** bufのpos以降へ10進数のvを継ぎ足す */
static UINT32 diag_append_uint(char *buf, UINT32 pos, UINT32 v) {
    char tmp[12];
    UINT32 n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0 && n < sizeof(tmp));
    while (n > 0 && pos + 1 < LOAD_DIAG_MAX) {
        buf[pos++] = tmp[--n];
    }
    buf[pos] = '\0';
    return pos;
}

static void diag_line(const char *line) {
    if (g_load_diag_writer != 0) {
        g_load_diag_writer(line);
    }
}

void os_load_set_diag_writer(void (*writer)(const char *)) {
    g_load_diag_writer = writer;
}

void os_load_abort_stats_reset(void) {
    g_load_aborted_total = 0;
}

int os_load_aborted_total(void) {
    return g_load_aborted_total;
}

void os_load_report_aborts(void) {
    char line[LOAD_DIAG_MAX];
    UINT32 pos = diag_append(line, 0, "[load] aborted top-level forms: ");
    pos = diag_append_uint(line, pos, (UINT32)g_load_aborted_total);
    diag_append(line, pos, "\n");
    diag_line(line);
}

/**
 * [#197] 宛先の無い非局所脱出が最上位まで戻ってきたときの理由の文面。
 *
 * **理由を「どの構文から来たか」で書く。** 実害の直し手は
 * 「どのファイルのどの構文が消えたか」を知りたいのであって、
 * 総数だけでは直せない(調査 §C-2)。
 */
static const char *transfer_reason(lisp_val_t v) {
    switch (os_control_transfer_magic(v)) {
    case MAGIC_BLOCK_EXIT:
        return "uncaught block-exit (return-from to a block that is not live here)";
    case MAGIC_CATCH_EXIT:
        return "uncaught catch-exit (throw with no matching catch)";
    case MAGIC_GO_EXIT:
        return "uncaught go-exit (go with no matching tagbody tag)";
    default:
        return "uncaught control transfer";
    }
}

/**
 * [P2 / #197] 打ち切られたフォーム1件を**数えて、画面と直列線の両方へ出す。**
 *
 * **cc_loadは評価結果を捨てるので、以前はロード中にフォームが打ち切られても
 * 何も出なかった。** どのファイルで落ちたかが分かるよう読み込み元の名前を前置きする
 * (9P経由ならパスそのもの)。
 * 打ち切られたフォームは飛ばして次のフォームへ進む(ロード自体は続行する)。
 *
 * report-conditionのメッセージが得られなければ、REPLと同じく生の値の印字へ落とす。
 *
 * **画面へ出す文面は変えていない**(既存の単体試験が文字列で固定している)。
 * 足したのは総数の加算と直列線への1行で、**画面だけに出す形では母艦から
 * 見えない**のが#197の本体だからである。
 *
 * @param is_transfer 生の非局所脱出シグナル(= conditionではない)が戻ってきた場合に1。
 *                    report-conditionにかけてはいけないのでここで分ける
 */
static void record_load_abort(const char *name, lisp_val_t result, lisp_val_t env, int is_transfer) {
    /* os_condition_report_cstrはLisp側の関数を呼ぶ = 確保する。resultとenvは
       その呼び出しを跨いでこの関数のスロットで生き続ける必要がある(原則4) */
    GC_PROTECT(result);
    GC_PROTECT(env);

    g_load_aborted_total++;

    char msg[LOAD_REPORT_MAX];
    const char *reason = 0;
    if (is_transfer) {
        reason = transfer_reason(result);
    } else if (os_condition_report_cstr(result, env, msg, sizeof(msg))) {
        reason = msg;
    }

    process_t *proc = get_current_process();
    proc->stdout_buffer->write_string(proc->stdout_buffer, "load: ");
    proc->stdout_buffer->write_string(proc->stdout_buffer, name);
    proc->stdout_buffer->write_string(proc->stdout_buffer, ": ");
    if (reason != 0) {
        proc->stdout_buffer->write_string(proc->stdout_buffer, reason);
    } else {
        os_print(result, proc->stdout_buffer);
    }
    proc->stdout_buffer->write_char(proc->stdout_buffer, '\n');

    /* 直列線(= 母艦が読む側)。理由が引けないときも**行は出す。**
       「理由が無いから1行も出さない」は、静かに欠けるのと同じことである */
    char line[LOAD_DIAG_MAX];
    UINT32 pos = diag_append(line, 0, "[load] abort ");
    pos = diag_append(line, pos, name);
    pos = diag_append(line, pos, ": ");
    pos = diag_append(line, pos, (reason != 0) ? reason : "<reason unavailable>");
    diag_append(line, pos, "\n");
    diag_line(line);
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

        /* [#197] **宛先の無い脱出も「完了していない」として数える。**
           os_eval_top_level_exが打ち切りとみなすのは**宛先が%TOP-LEVELで、かつ
           MAGIC_BLOCK_EXIT**のときだけである(eval.c。%abort-top-levelが作るものだけに
           限る意図があって、そこは変えない)。だが対応するcatchの無いthrowや
           対応するtagbodyの無いgoは、**生の脱出シグナルのまま**ここへ戻ってくる。
           いままではそれがabortedに入らないので**件数も増えず、理由の表示も
           行われなかった**(= 画面にすら何も出ない。調査
           docs/investigation/detector-t4-passline.refs.md §C-2)。

           **最上位には行き先が無いので、そこへ戻ってきた制御移転は定義上
           「その式は完了していない」。** 種類を問わず数える。

           **判定をeval.c側ではなくここでするのはなぜか。** os_eval_top_level_exは
           REPL(repl.c)と9Pの無い起動の判定(kernel.c)も通る。あちらの意味論を
           変えずに済ませたいので、「読み込まれなかった最上位の式を数える」責務は
           loadの輪だけが持つ。REPLは脱出を人間に見せる場であって、
           静かに欠けることが問題になる場ではない */
        int uncaught_transfer = (!aborted && os_is_control_transfer(result));

        if (aborted || uncaught_transfer) {
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
            // [#197] **総数の加算はこの外(record_load_abort)で、呼び元に依存せず行う。**
            // 以前は加算が上のifの中だけにあり、0を渡す呼び元(= 通常のloadの
            // すべて)では加算自体が起きていなかった
            record_load_abort(name, result, env, uncaught_transfer);
        }
    }

    // 輪の中でGCが走っている可能性があるので、閉じる前にも取り直す
    os_stream_close((handle != nil) ? os_stream_from_handle(handle) : stream);
    return g_sym_t;
}

/**
 * [#197] 1本ぶんの読み込みを終えて、**打ち切りがあったファイル名と件数を直列線へ出す。**
 *
 * **cc_loadの3経路が0を渡していたのをやめる。** 渡していたせいで
 * os_load_from_streamの加算(`if (out_aborted_count != 0)`の中)が起きておらず、
 * 件数は「捨てられていた」のではなく**最初から数えられていなかった**。
 *
 * **総数だけでは直せない。** #197の実害は「ファイルが半分だけ静かに欠ける」ことで、
 * 直し手が必要なのは**どのファイルか**である。だからここで1本ごとに出す。
 * loadの戻り値と契約は変えない(打ち切りがあってもg_sym_tのまま)。
 *
 * @param name 表示に使う名前。**マウント接頭辞を剥がす前のパスを渡すこと**
 *             (母艦の読み手が台本に書いたとおりの綴りで探せるように)
 */
static lisp_val_t load_one_file(os_stream_t *stream, const char *name, lisp_val_t env) {
    GC_PROTECT(env);
    int aborted = 0;
    lisp_val_t result = os_load_from_stream(stream, name, env, &aborted);
    if (aborted != 0) {
        char line[LOAD_DIAG_MAX];
        UINT32 pos = diag_append(line, 0, "[load] file ");
        pos = diag_append(line, pos, name);
        pos = diag_append(line, pos, ": aborted ");
        pos = diag_append_uint(line, pos, (UINT32)aborted);
        diag_append(line, pos, "\n");
        diag_line(line);
    }
    return result;
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

    /* [M2-T1 / #193] **先頭が'/'でなければホスト共有(9p)のまま。**
       マウント表は引かない。いまloadへ渡っている名前はほぼ全部この形で、
       マウント表の解決は絶対パス前提なので、ここをマウント表へ回すと
       **どのマウントにも解決できず全試験が1行も走らなくなる**
       (docs/investigation/m2-load-from-disk.logic.md §8-2、.refs.md §B-2)。
       **心覚え: 相対 = 母艦(開発時)、絶対 = 機械の中。**
       9pのストリームはCのローカルでよい(os_stream_open_9p_fileはLispを
       呼ばず確保もしないので、GCで動く心配が無い) */
    if (path[0] != '/') {
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
        /* [#197] **件数を受け取る。** LOADの戻り値は従来どおり
           「打ち切りがあってもg_sym_t」のままで、受け取った件数は直列線へ出すだけである
           (load_one_fileの注記) */
        return load_one_file(&stream, path, env);
    }

    /* 絶対パスはマウント表で解決する。relativeの容量はSTREAM_PATH_MAX
       (stream.h:8-10 が「stream_lisp.c/load.c/mount.c で共有する規約」と
        明記している。LOAD_PATH_MAXと同じ値だが、os_mount_resolveへ渡す
        バッファなので規約側の名前を使う) */
    char relative[STREAM_PATH_MAX];
    lisp_val_t device = nil;
    GC_PROTECT(device);
    mount_kind_t kind = os_mount_resolve(path, relative, sizeof(relative), &device);

    if (kind == MOUNT_KIND_9P) {
        os_stream_t stream;
        if (!os_stream_open_9p_file(&stream, relative, err_msg, sizeof(err_msg))) {
            return os_signal_io_error("load", path, err_msg, env);
        }
        return load_one_file(&stream, path, env);  /* [#197] 件数を受け取る */
    }

    if (kind == MOUNT_KIND_FAT32 || kind == MOUNT_KIND_FAT16) {
        UINT8 *data;
        UINT32 len;
        /* FATドライバ(Lisp)が非局所脱出したらそれをそのまま返す。拾わないと
           **その経路のエラーだけが静かに消える**(stream_lisp.c の omission list)。
           **GC_PROTECTは呼び出しより前に置くこと**(mount.c側が書き込んだ値が、
           以後の確保で再配置されても追随するように) */
        lisp_val_t transfer = nil;
        GC_PROTECT(transfer);
        if (!os_mount_fat_read_file(kind, device, relative, &data, &len, &transfer)) {
            if (transfer != nil) {
                return transfer;
            }
            /* **ファイルが無いことと空ファイルは区別できない**(FATドライバの
               既知の制約。mount.c)。どちらもここへ来る */
            return os_signal_io_error("load", path, "cannot read file", env);
        }
        /* [#49 / M6] os_stream_t(raw)はMAGIC_STREAMに包まれるまでGCから到達不能
           なので、**確保を伴う処理をすべて終えてから、包む直前に**確保する
           (cc_open_input_streamと同じ並び)。dataはos_mount_fat_read_file内で
           os_alloc_raw済みの専有バッファなので、そのままstr_bufとして渡せる */
        os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
        os_stream_open_string_input(raw, data, len);
        /* **包むのは必須である。** 包まないとGCで本体ごとバッファが消える。
           包んだ以上、輪は反復ごとにself_handleから実体を取り直す
           (os_load_from_streamがそうしている。M2-T0 / #196) */
        lisp_val_t handle = os_make_stream(raw);
        GC_PROTECT(handle);
        return load_one_file(os_stream_from_handle(handle), path, env);  /* [#197] 件数を受け取る */
    }

    /* [#193 の決定] **ホスト共有へ落とさない。**
       落とすと「割り当て名の綴り間違い」がホスト共有の文面で失敗して
       原因が分からなくなる(logic.md §9-5)。文面は cc_open_input_stream と揃える */
    return os_signal_io_error("load", path, "no such mount", env);
}

void os_register_load(void) {
    os_set_function(os_make_symbol("LOAD"), os_make_native_function((lisp_addr_t)(void *)cc_load), global_environment);
}
