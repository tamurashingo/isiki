#include "types.h"
#include "version.h"
#include "interrupt.h"
#include "framebuffer.h"
#include "process.h"
#include "runtime.h"
#include "block_device.h"
#include "repl.h"
#include "subprimitive.h"
#include "ide_subprimitive.h"
#include "bench_subprimitive.h"
#include "virtio9p.h"
#include "load.h"
#include "eval.h"
#include "stream_lisp.h"
#include "format.h"
#include "clock.h"
#include "stream.h"
#include "za.h"
#include "disasm_lisp.h"
#include "mount.h"
#include "init_lisp_src.h"
#include "reader.h"

// src/c/lisp_compiled.c(トランスパイラがsrc/lisp/init_aot.lispから生成する、
// gitignore対象のビルド成果物)で定義される。init.lispから移動したmember/assoc等を
// os_set_function経由でglobal_environmentへ登録する(M13)
extern void os_register_aot_init_functions(void);

// src/c/lisp_compiled.c(トランスパイラがsrc/lisp/device.lisp等から生成する)で
// 定義される。device.lisp/ide.lisp等のdefun以外のトップレベルフォーム
// (defdynamicの初期化・defclassのクラス登録・%ide-register-devices等の
// 起動時に一度だけ実行すべき副作用)を、ソース上の順序のまま実行する(M15)。
// os_register_aot_init_functionsの直後に呼ぶ必要がある(前者が全defunを
// global_environmentへ登録し終えてから、それらを呼び出す副作用フォームを
// 実行するため)
extern void os_run_aot_toplevel_forms(void);

/**
 * バージョン文字列・ビルド日時・ハッシュ・VirtIO-9pの検出結果をfbへ表示する
 * @param fb 表示先のframe buffer
 */
static void kernel_show_information(frame_buffer *fb) {
    fb->write_string(fb, "isikiOS version ");
    fb->write_string(fb, ISIKIOS_VERSION);
    fb->write_char(fb, '\n');

    fb->write_string(fb, "build: ");
    fb->write_string(fb, ISIKIOS_BUILD_DATE);
    fb->write_string(fb, " (");
    fb->write_string(fb, ISIKIOS_BUILD_HASH);
    fb->write_string(fb, ")\n");

    char err_msg[160];
    err_msg[0] = '\0';
    if (os_virtio9p_ensure_session(err_msg, sizeof(err_msg))) {
        fb->write_string(fb, "virtio9p: VirtIO-9p device detected\n");
    } else {
        fb->write_string(fb, "virtio9p: not detected: ");
        fb->write_string(fb, err_msg);
        fb->write_char(fb, '\n');
    }
}


/**
 * カーネルのエントリポイント。ヒープ・仮想バッファ・プロセス・GDT/PIC/PIT/IDTを
 * 初期化し、最後にプロセススケジューラを起動する(戻ってこない)
 * @param fb_base 物理frame bufferのアドレス
 * @param fb_width frame bufferの横幅(pixel)
 * @param fb_height frame bufferの高さ(pixel)
 * @param fb_pixels_per_scanline frame buffer1行あたりのピクセル数
 * @param heap_base Lispヒープの先頭アドレス
 * @param heap_size Lispヒープのサイズ(バイト)
 */
/** kernel_mainに渡されたboot_epoch_secondsを保持する(kernel_get_boot_epoch_secondsで読める) */
static UINT64 g_boot_epoch_seconds = 0;

UINT64 kernel_get_boot_epoch_seconds(void) {
    return g_boot_epoch_seconds;
}

/** os_set_boot_entry経由でprocess 0の専用スタック上から呼ぶpower_off(kernel_mainの引数を保持)。
    **トリガーの有無にかかわらず保持する**: 9Pの無い起動でも電源断が要る段(BOOT_SMOKE)がある */
static void (*g_firmware_power_off)(void) = 0;

/** .qemu-test-triggerがあったか(= boot-entry台本を走らせて電源断するか) */
static int g_qemu_test_mode = 0;

/** .qemu-test-triggerの内容(cc_loadするboot-entryスクリプトのパス)を保持する */
static char g_qemu_test_boot_script[256] = "test/lisp/qemu_boot_test.lisp";

/** g_qemu_test_boot_scriptが指すboot-entryスクリプトをcc_loadしてからpower_offする(process 0専用スタック上で実行される) */
static void run_qemu_boot_test(void) {
    cc_load(os_make_cons(os_make_string(g_qemu_test_boot_script), nil), global_environment);
#ifdef ISIKIOS_ALIGN_AUDIT
    /* [調査] 16byte境界監査(ALIGN_AUDIT=1ビルドのみ)。boot-entryスクリプトを
       変えずにどのQEMU試験でも集計が取れるよう、電源断の直前でシリアルへ出す */
    os_align_audit_report();
#endif
    g_firmware_power_off();
}

#if !defined(ISIKIOS_NO_EMBEDDED_INIT) || defined(ISIKIOS_BOOT_SMOKE)
/** 診断メッセージを組む固定長バッファ。freestandingなのでsnprintfは無い */
#define BOOT_MSG_MAX 192

/** msgの末尾(NUL位置)へsrcを足す。あふれたら切る。戻り値は新しいNUL位置 */
static UINT32 boot_msg_append(char *msg, UINT32 pos, const char *src) {
    while (*src != '\0' && pos + 1 < BOOT_MSG_MAX) {
        msg[pos++] = *src++;
    }
    msg[pos] = '\0';
    return pos;
}

/** msgの末尾へvを10進で足す(件数を出すため。桁数は小さい) */
static UINT32 boot_msg_append_uint(char *msg, UINT32 pos, UINT32 v) {
    char rev[12];
    UINT32 n = 0;
    if (v == 0) {
        rev[n++] = '0';
    }
    while (v > 0 && n < sizeof(rev)) {
        rev[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && pos + 1 < BOOT_MSG_MAX) {
        msg[pos++] = rev[--n];
    }
    msg[pos] = '\0';
    return pos;
}
#endif /* 診断メッセージの組み立て */

#ifndef ISIKIOS_NO_EMBEDDED_INIT
/**
 * [M1-T3/T4] カーネルへ同梱したLispソースを1本読む。
 *
 * **[L3] 素のos_stream_t(Cのローカル)として読む。言語側の値(os_make_stream)には
 * 包まない。** 包むとGCのたびにstr_capぶん(init.lispは72KB)が丸ごとヒープへ
 * 複製される(runtime.cのgc_relocate_stream)。os_stream_open_string_inputは
 * ポインタと長さを持つだけでコピーも書き換えもしないので、同梱した配列を
 * そのままゼロコピーで読める(stream.c)。
 *
 * **[D1/L1] 1フォームでも打ち切られたらos_panicで止める。** os_load_from_streamは
 * 打ち切られたフォームを飛ばして最後までg_sym_tを返すので、戻り値だけでは
 * 「半分失敗した」が分からない。半分欠けた基盤ライブラリで立ち上げを続けると、
 * 欠けた定義を使う所まで進んでから意味の分からない形で失敗する。
 *
 * **自前のfor(;;)は書かない**(os_panicはhltで止まるので「異常停止」と
 * 「極端に遅い」を外から区別できる。空のfor(;;)で33分のハングに気づけなかった
 * 実例がruntime.cのos_panicのコメントにある)。QEMU試験ではos_set_panic_hookの
 * 電源断が働くので、試験は固まらずに落ちる。
 *
 * @param src 同梱したソース(init_lisp_src.h。NUL終端しない)
 * @param len srcのバイト数
 * @param name エラー表示に使う名前(開く用途には使わない)
 */
static void load_embedded_lisp(const UINT8 *src, UINT32 len, const char *name) {
    os_stream_t stream;
    os_stream_open_string_input(&stream, (UINT8 *)src, len);

    int aborted = 0;
    lisp_val_t result = os_load_from_stream(&stream, name, global_environment, &aborted);

    char msg[BOOT_MSG_MAX];
    UINT32 pos = boot_msg_append(msg, 0, "boot error: ");
    pos = boot_msg_append(msg, pos, name);
    if (aborted != 0) {
        pos = boot_msg_append(msg, pos, ": aborted forms=");
        pos = boot_msg_append_uint(msg, pos, (UINT32)aborted);
        os_panic(msg);
    }
    if (result != g_sym_t) {
        /* 構文エラー・読み出し失敗。os_load_from_streamはos_signal_io_errorの
           戻り値を返す。条件クラスが引けない起動前半ではsignalが上がらず
           g_sym_eval_errorが素通りで戻ってくるため、戻り値でも見る */
        boot_msg_append(msg, pos, ": read failed");
        os_panic(msg);
    }
}
#endif /* !ISIKIOS_NO_EMBEDDED_INIT */


#ifdef ISIKIOS_BOOT_SMOKE
/* ===================== [M1の本番の判定] 9Pの無い起動の判定 =====================
   **-fsdev / -device virtio-9p-pci を外したQEMUで立ち上がり、(let ((x 1)) x) が
   通ること。** これがM1の完了条件そのものである(docs/plans/local-load-and-core-image.md
   のG2)。make test-qemu-no-9p から BOOT_SMOKE=1 のビルドで走る。

   **なぜC側に書くことになるのか。** 9Pが無いと、従来の試験の仕組みが3つとも使えない:
     - .qemu-test-trigger が開けない        -> 台本を指定できない(試験モードにならない)
     - 台本がホスト共有にある               -> そもそも台本を読めない
       (cc_load 自体は M2-T1 / #193 でマウント経由になり**ディスクも読める**。
        読めないのは引き金のファイルと台本の置き場所がホスト共有だからである)
     - test-results.txt が9P経由            -> 判定結果をホストへ渡せない
   残る線はシリアル(os_diag_serial_write)だけである(-display none では画面は見えない)。
   したがってこの段は「式を直接持ち、判定をシリアルへ出し、自分で電源断する」。

   **陰性対照を内蔵している。** 1件目で「9Pが本当に無いこと」を確かめる。
   これが無いと、9Pを付けたまま走らせても合格してしまい、
   「9Pが無くても動く」を何も確かめていない段になる。 */

static UINT32 g_smoke_pass = 0;
static UINT32 g_smoke_fail = 0;

/** 1件の判定をシリアルへ出して数える */
static void smoke_ck(const char *label, int ok) {
    if (ok) {
        g_smoke_pass++;
    } else {
        g_smoke_fail++;
    }
    os_diag_serial_write(ok ? "[no9p] ok   " : "[no9p] NG   ");
    os_diag_serial_write(label);
    os_diag_serial_write("\n");
}

/**
 * 文字列を1つの式として読んで評価する(台本をloadできないので式を直接持つ)。
 *
 * **成否を戻り値で返し、値は*out_valueへ書く。** 失敗をg_sym_eval_errorの
 * 「値」で表さないのは、EVAL-ERROR を値として返す箇所を増やさない方針
 * (tools/check_eval_error_budget.py の ratchet)に従うためである。
 *
 * @param src 評価する式のソース(NUL終端しなくてよい)
 * @param len srcのバイト数
 * @param out_value 評価結果の書き込み先(失敗時は触らない)
 * @return 読めて打ち切られずに評価できたら1、そうでなければ0
 */
static int smoke_eval(const char *src, UINT32 len, lisp_val_t *out_value) {
    os_stream_t stream;
    os_stream_open_string_input(&stream, (UINT8 *)src, len);
    lisp_val_t form = os_read_stream(&stream);
    os_stream_close(&stream);
    if (form == g_sym_read_error || form == nil) {
        return 0;
    }
    int aborted = 0;
    lisp_val_t value = os_eval_top_level_ex(form, global_environment, &aborted);
    if (aborted) {
        return 0;
    }
    *out_value = value;
    return 1;
}

#define SMOKE_EVAL(lit, pv) smoke_eval((lit), (UINT32)(sizeof(lit) - 1), (pv))

/** 判定を全部出してから電源断する(戻ってこない) */
static void run_boot_smoke(void) {
    os_diag_serial_write("[no9p] boot smoke begin\n");

    /* 陰性対照: 9Pが本当に無いこと。**あったらこの段は何も確かめていない** */
    char err_msg[160];
    err_msg[0] = '\0';
    smoke_ck("9p-absent", !os_virtio9p_ensure_session(err_msg, sizeof(err_msg)));

    lisp_val_t value = nil;

    /* **これが完了条件。** letは基盤ライブラリ(同梱したinit.lisp)のマクロなので、
       同梱が読めていなければここで落ちる */
    smoke_ck("let-returns-1",
             SMOKE_EVAL("(let ((x 1)) x)", &value) && value == os_make_fixnum(1));

    /* 条件クラスはinit.lispのdefclassが登録する。引ければ**同梱の後半まで
       読み終えている**ことの証拠になる(途中で打ち切られていれば欠ける) */
    value = nil;
    smoke_ck("condition-class-registered",
             SMOKE_EVAL("(%find-class '<simple-error>)", &value) && value != nil);

    /* [L2] 補助ライブラリ(disassemble.lisp)も同梱して「init.lisp -> disassemble.lisp」の
       順でC側から読んでいる。**以前はinit.lispの末尾の(load ...)が読んでいたので、
       9Pの無い起動ではここが黙って欠けていた。** その回帰を押さえる */
    smoke_ck("disassemble-defined",
             os_get_function(os_make_symbol("DISASSEMBLE"), global_environment) != nil);

    /* [M2-T3 / #195] **9Pを外した起動でも、ディスクのアプリが読めること。**
       これがM2の「アプリをディスクに置けるようにする」の本番の判定である
       (ホスト共有が有る間は、アプリがディスクに在る必要がそもそも無い)。

       **装置の名前(blk0/blk1/...)は探査順の連番**で、付けるディスクの構成が
       変わると動く。9Pの無い起動にはデータディスクが付かないので、ここでの
       blk番号は他の段と違う。**だからチャネルの名前で引く**
       (%device-named、src/lisp/device.lisp)。アプリ専用ディスクは
       Makefileが**どの起動にもPrimaryのslaveとして**付けるので、
       名前は常に "ide-primary-slave" である。

       **自動の割り当ては1つも無い**ので、読む前にmountを呼ぶ前段が要る。
       名前引きが外れていてもmountは(nil . :fat32)を積んで成功してしまうので、
       **引けたこと自体を1件に分けて数える**(でないと「読めない」が
       別の理由で出たときに見分けられない)。 */
    value = nil;
    smoke_ck("app-disk-named",
             SMOKE_EVAL("(%device-named \"ide-primary-slave\")", &value) && value != nil);

    value = nil;
    smoke_ck("app-disk-mounted",
             SMOKE_EVAL("(mount \"/app\" (%device-named \"ide-primary-slave\") ':fat32)",
                        &value) && value != nil);

    /* **ここが完了条件。** 絶対パスの `.lisp` をディスクから読む。
       `.lisp`(拡張子4文字)が置けるのはFAT32だから(旧方式は8.3名のみ) */
    value = nil;
    smoke_ck("app-disk-load",
             SMOKE_EVAL("(load \"/app/hello.lisp\")", &value) && value == g_sym_t);

    /* 読めただけでなく**中身が評価されている**こと。定数ではなく計算させる */
    value = nil;
    smoke_ck("app-hello-42",
             SMOKE_EVAL("(isiki-hello)", &value) && value == os_make_fixnum(42));

    /* まとめ。**ホスト側はこの1行をgrepする** */
    char msg[BOOT_MSG_MAX];
    UINT32 pos = boot_msg_append(msg, 0, "[no9p] ");
    pos = boot_msg_append_uint(msg, pos, g_smoke_pass);
    pos = boot_msg_append(msg, pos, " passed, ");
    pos = boot_msg_append_uint(msg, pos, g_smoke_fail);
    boot_msg_append(msg, pos, " failed\n");
    os_diag_serial_write(msg);

    g_firmware_power_off();
}
#endif /* ISIKIOS_BOOT_SMOKE */

/**
 * [L4] process 0の専用スタック上で、REPLループへ入る前に一度だけ走る
 * 「起動時にやること」。**フックは1つで、中で「同梱 → 台本」の順に実行する。**
 *
 * 同梱の読み込みが台本より前でなければならない: 台本(test/lisp/qemu_boot_*.lisp)は
 * 基盤ライブラリのマクロ(letを含む31個)と条件クラスを前提に書かれており、
 * 決定D2でその先頭の(load "src/lisp/init.lisp")を外したため、
 * **同梱が先に読まれていないと台本が動かない。**
 *
 * 台本は従来どおり任意である(.qemu-test-triggerがあるときだけ)。
 */
static void run_boot_entry(void) {
#ifndef ISIKIOS_NO_EMBEDDED_INIT
    /* [L2] 依存の向きをC側で明示する。disassemble.lispは基盤ライブラリの
       while/formatに依存するので**後**に読む。以前はinit.lispの末尾の
       (load "src/lisp/disassemble.lisp")がこれを担っていたが、あの1行は
       ホスト共有(9P)を要求し続けるため外した(「9Pが無くても立ち上がる」と直接矛盾する) */
    load_embedded_lisp(g_init_lisp_src, g_init_lisp_src_len, "<embedded>src/lisp/init.lisp");
    load_embedded_lisp(g_disassemble_lisp_src, g_disassemble_lisp_src_len,
                       "<embedded>src/lisp/disassemble.lisp");
#endif
#ifdef ISIKIOS_BOOT_SMOKE
    /* 9Pの無い起動の判定。電源断するので戻ってこない */
    run_boot_smoke();
#endif
    if (g_qemu_test_mode) {
        run_qemu_boot_test();
    }
}

void kernel_main(UINT64 fb_base, UINT32 fb_width, UINT32 fb_height, UINT32 fb_pixels_per_scanline, UINT64 heap_base, UINT64 heap_size, UINT64 boot_epoch_seconds, void (*power_off)(void)) {

    asm volatile ("cli");

    g_boot_epoch_seconds = boot_epoch_seconds;
    /* **トリガーの有無にかかわらず保持する。** 9Pの無い起動の判定(BOOT_SMOKE)も
       自分で電源断する必要があるため、試験モードの中だけで覚えるのでは足りない */
    g_firmware_power_off = power_off;

    os_boot_alloc_init(heap_base, heap_size);

    // 仮想バッファのcontent(スクロールバックの履歴)をboot allocatorから取る。
    // GOPの解像度はkernel_mainの引数として既に分かっているので、桁数はここで確定する
    // (documents/console-scrollback.md)。
    //
    // os_boot_alloc_finalizeを呼ぶと残りがLispのGCヒープになってしまうため、
    // 確保は必ずその**前**に行う。あわせてinitialize_virtual_buffersもここまで
    // 繰り上げる。副作用として、これ以降のos_panic/boot_alloc枯渇の診断表示が
    // 実際に画面へ出るようになる(従来はブート後半までフレームバッファの
    // 関数ポインタが0のままだった)。
    UINT32 vbuf_hist_rows = VBUF_DEFAULT_HIST_ROWS;
    UINT8 *vbuf_content = (UINT8 *)os_boot_alloc_try(os_vbuf_content_bytes(fb_width, fb_height, vbuf_hist_rows), 8);
    if (vbuf_content == 0) {
        // 履歴ぶんが取れなければ画面1枚ぶんまで落として続行する。スクロールバックは
        // 効かなくなるが、コンソールは従来どおり動く(起動しないよりよい)
        vbuf_hist_rows = fb_height / VBUF_GLYPH_HEIGHT;
        vbuf_content = (UINT8 *)os_boot_alloc(os_vbuf_content_bytes(fb_width, fb_height, vbuf_hist_rows), 8);
    }
    frame_buffer *fb = initialize_virtual_buffers(fb_base, fb_width, fb_height, fb_pixels_per_scanline,
                                                  vbuf_content, vbuf_hist_rows);
    // clear_screenは物理ピクセルだけを黒く塗り、contentには触れない。初期化直後
    // (contentがまだ空の時点)で呼んでおかないと、これより前に出力があった場合に
    // 「contentには残っているが画面には出ていない」食い違いが生まれ、
    // バッファ切り替えやスクロールで唐突に現れることになる
    fb->clear_screen(fb);
    fb->draw_cursor(fb);

    os_block_device_probe_all();
    UINT64 lisp_heap_base, lisp_heap_size;
    os_boot_alloc_finalize(&lisp_heap_base, &lisp_heap_size);

    os_heap_init(lisp_heap_base, lisp_heap_size);
    /* [境界] JITリテラルスロットの配列先頭を、最初のJITコンパイルより前に確かめる。
       os_bootstrap内でnil自身が検査されるのと同じ趣旨(documents/static-align16-survey.md
       「本採用」節)。ここより早く置けないのは、os_panicが使う診断経路
       (シリアル/フレームバッファ)がこの時点で初めて使える状態になるため */
    za_assert_slot_alignment();
    os_bootstrap();
    os_register_subprimitives();
    os_register_ide_subprimitives();
    os_register_mount_native_subprimitives();
    os_register_bench_subprimitives();
    os_register_load();
    os_register_eval_primitives();
    os_register_streams();
    os_register_format();
    os_register_clock();
    os_register_za_primitives();
    os_register_disasm();
    os_register_aot_init_functions();
    os_run_aot_toplevel_forms();

    initialize_processes(fb);

    init_fpu();
    init_gdt();
    init_pic();
    init_pit();
    init_idt();

    // リポジトリルートに.qemu-test-triggerがあれば、その内容が指すLispファイルを
    // 自動loadしてから電源を切る(make test-qemu/test-qemu-milestoneからの全自動実行用)。
    // 内容が空(従来通りのtouchのみ)ならg_qemu_test_boot_scriptの初期値
    // (test/lisp/qemu_boot_test.lisp、全milestoneを通しで実行する)をそのまま使う。
    //
    // kernel_mainのブート時スタック上で直接cc_loadを呼ぶとスタックサイズ不足で
    // クラッシュするため、process 0がタイマー割り込み経由で最初に起動される際に
    // (process 0専用のスタック(STACK_SIZE、process.c参照)上で)実行させる(process_trampoline_c参照)。
    // **カーネルへ同梱した基盤ライブラリの読み込みも同じ理由で同じ場所に相乗りする**
    // (下のos_set_boot_entry。run_boot_entryが「同梱 → 台本」の順に実行する)
    os_stream_t trigger_probe;
    char trigger_err[64];
    if (os_stream_open_9p_file(&trigger_probe, ".qemu-test-trigger", trigger_err, sizeof(trigger_err))) {
        UINT64 len = 0;
        char ch;
        while (len + 1 < sizeof(g_qemu_test_boot_script) && os_stream_read_char(&trigger_probe, &ch)) {
            if (ch == '\n' || ch == '\r') {
                break;
            }
            g_qemu_test_boot_script[len++] = ch;
        }
        os_stream_close(&trigger_probe);
        if (len > 0) {
            g_qemu_test_boot_script[len] = '\0';
        }
        g_qemu_test_mode = 1;
        // ヒープ/Immobilized Spaceの枯渇等でos_panicへ落ちた場合、テスト実行中は
        // 電源断して「テスト失敗」として終わらせる。旧実装は空のfor(;;)で永久に
        // 停止し、-display noneでは診断メッセージも見えないため、外からは
        // 「極端に遅い処理」と区別がつかなかった(letのImmobilized Spaceリークの
        // 調査で33分間ハングに気づけなかった実例がある)
        os_set_panic_hook(power_off);
    }

    if (!g_qemu_test_mode) {
        kernel_show_information(fb);
    }

    // [L4] 「起動時にやること」を**無条件に**登録する。.qemu-test-triggerの有無に
    // かかわらず同梱した基盤ライブラリを読む必要があるため、登録はトリガー判定の外に置く
    // (試験モードのときだけ登録していた旧実装のままだと、通常の対話起動では
    // 一度も呼ばれず、9Pの無い実機でletが使えないままになる)
    os_set_boot_entry(run_boot_entry);

#ifdef ISIKIOS_BOOT_SMOKE
    /* [D1] 9Pが無いと.qemu-test-triggerが開けないので、上のブロックは走らない。
       **panicフックをここで登録する。** 登録が無いとos_panicがhltで止まり、
       外からは「極端に遅い」と区別できないまま試験が時間切れを待つことになる */
    os_set_panic_hook(power_off);
#endif

    // [原則6] 各プロセススタックの直下を未マップにする。溢れた瞬間に#PFが出て、
    // IDT/GDT(BSS上、スタックの下側にある)へ到達する前に止まる。
    // プロセスを起動する前に済ませる必要がある
    os_process_install_stack_guards();

    process_scheduler_start();
}


