#include "stream_lisp.h"
#include "runtime.h"
#include "lisp.h"
#include "process.h"
#include "reader.h"
#include "mount.h"
#include "eval.h"

/* [P1] **C→Lispの呼び戻しは、Cの境界で脱出を値に化けさせる。**
   FAT系ストリームのflush/refillはwrite-from!/read-into!をos_apply_functionで
   呼び戻すが、stream.cの公開APIはintしか返せないので、脱出値は
   stream->pending_transferへ預けられる(stream.hのコメント参照)。
   **ストリームを触るcc_*は、Lispへ戻る前に必ずos_stream_take_transferで拾うこと。**
   1箇所でも漏らすと、そのAPI経由のエラーだけが静かに消える
   (documents/pitfalls.mdのomission list、documents/error-unwind-survey.md §F-6)。 */

/** stream(TAG_INSTANCE, MAGIC_STREAM)のword1に埋め込んだ生ポインタを取り出す */
static os_stream_t *stream_raw(lisp_val_t stream) {
    lisp_addr_t addr = stream & ~TAG_MASK;
    UINT64 *obj = (UINT64 *)addr;
    return (os_stream_t *)(lisp_addr_t)obj[1];
}

/** kindが入力可能(READ-CHAR等が使える)なストリーム種別かどうかを判定する */
static int stream_kind_is_input(stream_kind_t kind) {
    return kind == STREAM_9P_FILE_READ || kind == STREAM_9P_FILE_IO || kind == STREAM_STRING_INPUT
        || kind == STREAM_FAT_FILE_IO || kind == STREAM_INPUT_KEYBOARD;
}

/** キーボード入力ストリーム(ISLisp仕様の既定の(standard-input))を新しく作る */
static lisp_val_t make_keyboard_input_stream(void) {
    os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
    os_stream_open_keyboard_input(raw, os_process_stdin_read_char);
    return os_make_stream(raw);
}

/** (%%keyboard-input-stream) → 新しいキーボード入力ストリーム。init.lispのstandard-inputが
 * *standard-input*が束縛されていないときの既定値として使う */
lisp_val_t cc_keyboard_input_stream(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    return make_keyboard_input_stream();
}

/**
 * ISLisp仕様§27.1の入力関数共通の引数 [input-stream [eos-error-p [eos-value]]] を解釈する。
 * input-stream省略時は動的変数*standard-input*(with-standard-inputで束縛される)を使い、
 * それも束縛されていなければキーボード入力ストリームを新しく作る。
 * eos-error-pの既定はt(終端でエラー)、eos-valueの既定はnil。
 * @param args 評価済み引数リスト
 * @param out_stream 使う入力ストリーム(MAGIC_STREAM)の格納先
 * @param out_eos_error_p 終端でconditionをsignalするかどうかの格納先
 * @param out_eos_value 終端でsignalしない場合に返す値の格納先
 */
static void resolve_input_args(lisp_val_t args, lisp_val_t *out_stream, int *out_eos_error_p, lisp_val_t *out_eos_value) {
    // os_make_symbol/make_keyboard_input_streamは割り当てを伴いGCを誘発しうるので、
    // それを跨いで使うargs/streamは保護する
    GC_PROTECT(args);
    lisp_val_t stream = (args != nil) ? cc_car(args) : nil;
    GC_PROTECT(stream);
    if (stream == nil) {
        stream = os_get_dynamic(os_make_symbol("*STANDARD-INPUT*"));
    }
    if (stream == nil) {
        stream = make_keyboard_input_stream();
    }
    lisp_val_t rest = (args != nil) ? cc_cdr(args) : nil;
    *out_eos_error_p = 1;
    *out_eos_value = nil;
    if (rest != nil) {
        *out_eos_error_p = (cc_car(rest) != nil);
        lisp_val_t rest2 = cc_cdr(rest);
        if (rest2 != nil) {
            *out_eos_value = cc_car(rest2);
        }
    }
    *out_stream = stream;
}

/**
 * 入力ストリームの終端に達した場合の共通処理(ISLisp仕様§27.1)。eos-error-pが真なら
 * <end-of-stream>(:stream stream)をsignalし、偽ならeos-valueを返す。
 */
static lisp_val_t handle_end_of_stream(lisp_val_t stream, int eos_error_p, lisp_val_t eos_value, lisp_val_t env) {
    if (!eos_error_p) {
        return eos_value;
    }
    GC_PROTECT(stream);
    GC_PROTECT(env);
    // 割り当てを1つずつ行い、それぞれの結果を次の割り当てを跨いで保護する
    // (C言語の引数評価順に依存したネストは避ける。documents/pitfalls.md 原則4)
    lisp_val_t kw_stream = os_make_symbol(":STREAM");
    GC_PROTECT(kw_stream);
    lisp_val_t class_sym = os_make_symbol("<END-OF-STREAM>");
    GC_PROTECT(class_sym);
    lisp_val_t initargs = os_make_cons(stream, nil);
    GC_PROTECT(initargs);
    initargs = os_make_cons(kw_stream, initargs);
    return os_signal_condition(class_sym, initargs, env);
}

/** kindが出力可能(WRITE-CHAR等が使える)なストリーム種別かどうかを判定する */
static int stream_kind_is_output(stream_kind_t kind) {
    return kind == STREAM_9P_FILE_WRITE || kind == STREAM_9P_FILE_IO
        || kind == STREAM_OUTPUT_SCREEN || kind == STREAM_STRING_OUTPUT
        || kind == STREAM_FAT_FILE_WRITE || kind == STREAM_FAT_FILE_IO;
}

/** STRINGオブジェクトのレイアウト([len(8byte)][chars...])からデータ先頭とバイト数を取り出す */
static void string_bytes(lisp_val_t str, const UINT8 **out_data, UINT32 *out_len) {
    lisp_addr_t addr = str & ~TAG_MASK;
    UINT64 len = ((UINT64 *)addr)[0];
    *out_data = (const UINT8 *)(addr + 8);
    *out_len = (UINT32)len;
}

/** data(len バイト)をコピーしてSTRINGオブジェクトを作る。os_make_string(NUL終端cstr版)の生バイト版 */
static lisp_val_t make_string_from_bytes(const UINT8 *data, UINT32 len) {
    lisp_addr_t addr = os_alloc_raw(8 + len);
    lisp_val_t *header = (lisp_val_t *)addr;
    header[0] = len;
    UINT8 *bytes = (UINT8 *)(addr + 8);
    for (UINT32 i = 0; i < len; i++) {
        bytes[i] = data[i];
    }
    return (lisp_val_t)(addr | TAG_STRING);
}

lisp_val_t os_make_stream(os_stream_t *raw) {
    // [ファイルI/O]#49(M6): raw->self_handleは、rawをまだどのMAGIC_STREAMも
    // 参照していない(=GCから見て到達不能で、gc_relocate_streamが動きようが無い)
    // この時点でだけ安全に生ポインタ経由で書き込める。os_make_instance自体が
    // GCを誘発してもrawはまだ影響を受けない
    lisp_val_t wrapper = os_make_instance(MAGIC_STREAM, (lisp_addr_t)(void *)raw, 0, 0);
    raw->self_handle = wrapper;
    return wrapper;
}

os_stream_t *os_stream_from_lisp(lisp_val_t stream) {
    return stream_raw(stream);
}

lisp_val_t cc_open_input_stream(lisp_val_t args, lisp_val_t env) {
    (void)env;
    char path[STREAM_PATH_MAX];
    os_string_to_cstr(cc_car(args), path, sizeof(path));

    char relative[STREAM_PATH_MAX];
    lisp_val_t device;
    mount_kind_t kind = os_mount_resolve(path, relative, sizeof(relative), &device);

    // [ファイルI/O]#49で発覚したGC安全性バグへの対処: os_stream_t(raw)は
    // MAGIC_STREAMとして包まれるまでGCから到達不能なため、確保してから
    // os_make_streamで包むまでの間に他のアロケーション(os_mount_fat_read_file内の
    // 多数のLisp呼び出し等)を挟むと、2回以上のGCを跨いだ際にrawの領域が別オブジェクトの
    // 複製先として上書きされうる。rawの確保はアロケーションを伴いうる処理をすべて
    // 終えた後、os_make_streamの直前まで遅らせる
    if (kind == MOUNT_KIND_9P) {
        os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
        char err_msg[128];
        if (!os_stream_open_9p_file(raw, relative, err_msg, sizeof(err_msg))) {
            /* [P4-4] 下位層のメッセージをそのまま condition へ載せる。
               **signal には global_environment を渡す(P4-2 と同じ理由)。**
               呼び出し元の env をエラー分岐まで生かすと、成功経路の側で
               callee-saved レジスタへの退避が増える(実測: この3関数の
               プロローグが +2 命令になった)。os_signal_condition が env を
               使うのは MAKE-INSTANCE / SIGNAL-CONDITION / %FIND-CLASS の
               解決だけで、どれも global_environment にしか登録されない */
            return os_signal_io_error("open-input-file", path, err_msg, global_environment);
        }
        return os_make_stream(raw);
    }

    if (kind == MOUNT_KIND_FAT32 || kind == MOUNT_KIND_FAT16) {
        UINT8 *data;
        UINT32 len;
        // FATドライバ(Lisp)が非局所脱出したらそれをそのまま返す。**GC_PROTECTは
        // 呼び出しより前に置くこと**(mount.c側が書き込んだ値が、以後のアロケーションで
        // 再配置されても追随するように)
        lisp_val_t transfer = nil;
        GC_PROTECT(transfer);
        if (!os_mount_fat_read_file(kind, device, relative, &data, &len, &transfer)) {
            if (transfer != nil) {
                return transfer;
            }
            return os_signal_io_error("open-input-file", path, "cannot read file", global_environment);
        }
        // dataはos_mount_fat_read_file内でos_alloc_raw済みの専有バッファ(コピー元の
        // Lisp vectorから既に切り離されている)なので、そのままstr_bufとして渡せる
        os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
        os_stream_open_string_input(raw, data, len);
        return os_make_stream(raw);
    }

    /* どのマウントにも解決できなかった(*mounts* に無いパス) */
    return os_signal_io_error("open-input-file", path, "no such mount", global_environment);
}

lisp_val_t cc_open_output_stream(lisp_val_t args, lisp_val_t env) {
    // ISLisp仕様§27.2: (open-output-stream filename [element-class]) はファイルへの
    // バイト出力ストリーム。ファイル名を渡された場合はopen-output-fileと同じ実装。
    // 引数無しの(open-output-stream)はこのカーネル独自の拡張で、画面出力ストリームを返す
    // ((standard-output)の既定値、device.lisp/ide.lisp/utility.lispが使う)
    if (args != nil && (cc_car(args) & TAG_MASK) == TAG_STRING) {
        return cc_open_output_file(args, env);
    }
    (void)env;
    os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
    os_stream_open_screen_output(raw, get_current_process()->stdout_buffer);
    return os_make_stream(raw);
}

lisp_val_t cc_close(lisp_val_t args, lisp_val_t env) {
    (void)env;
    // FAT系のcloseは最後のflush(write-from!の呼び戻し)を行うため、ハンドルは
    // 呼び出しを跨いで保護し、呼び出し後はハンドル経由で脱出を拾う
    lisp_val_t stream = cc_car(args);
    GC_PROTECT(stream);
    os_stream_close(stream_raw(stream));
    lisp_val_t transfer = os_stream_take_transfer(stream);
    if (transfer != nil) {
        return transfer;
    }
    return nil;
}

lisp_val_t cc_read_char(lisp_val_t args, lisp_val_t env) {
    lisp_val_t stream;
    int eos_error_p;
    lisp_val_t eos_value;
    resolve_input_args(args, &stream, &eos_error_p, &eos_value);
    // FAT系のreadはread-into!を呼び戻すのでGCが走りうる。streamハンドルを保護
    // しないと、以後のhandle_end_of_stream/os_stream_take_transferが古い実体を指す
    GC_PROTECT(stream);
    GC_PROTECT(eos_value);
    os_stream_t *raw = stream_raw(stream);
    char ch;
    if (!os_stream_read_char(raw, &ch)) {
        lisp_val_t transfer = os_stream_take_transfer(stream);
        if (transfer != nil) {
            return transfer;
        }
        return handle_end_of_stream(stream, eos_error_p, eos_value, env);
    }
    /* [4bit化] chはsigned char。UINT8を経由しないと0x80以上で符号拡張する */
    return os_make_char((UINT8)ch);
}

lisp_val_t cc_write_char(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t ch = cc_car(args);
    // FAT系の書き込みはバッファが満杯になるとwrite-from!を呼び戻す
    lisp_val_t stream = cc_car(cc_cdr(args));
    GC_PROTECT(stream);
    GC_PROTECT(ch);
    os_stream_write_char(stream_raw(stream), (char)(ch >> CHAR_VALUE_SHIFT));
    lisp_val_t transfer = os_stream_take_transfer(stream);
    if (transfer != nil) {
        return transfer;
    }
    return ch;
}

lisp_val_t cc_read(lisp_val_t args, lisp_val_t env) {
    lisp_val_t stream;
    int eos_error_p;
    lisp_val_t eos_value;
    resolve_input_args(args, &stream, &eos_error_p, &eos_value);
    GC_PROTECT(stream);
    GC_PROTECT(eos_value);
    os_stream_t *raw = stream_raw(stream);
    int eof;
    int has_pending;
    char pending;
    lisp_val_t result = os_read_stream_ex(raw, &eof, &has_pending, &pending);
    GC_PROTECT(result);
    lisp_val_t transfer = os_stream_take_transfer(stream);
    if (transfer != nil) {
        return transfer;
    }
    if (eof) {
        return handle_end_of_stream(stream, eos_error_p, eos_value, env);
    }
    // readerが先読みして未消費のまま残した1文字を、(読み取り中のGCで再配置されて
    // いるかもしれないので)ハンドルから取り直したos_stream_tの先読みスロットへ戻し、
    // 続くread-char等で失われないようにする
    if (has_pending) {
        raw = stream_raw(stream);
        if (!raw->has_lookahead) {
            raw->has_lookahead = 1;
            raw->lookahead = pending;
        }
    }
    return result;
}

lisp_val_t cc_open_stream_p(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) != TAG_INSTANCE) {
        return nil;
    }
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    if (obj[0] != MAGIC_STREAM) {
        return nil;
    }
    return stream_raw(val)->closed ? nil : g_sym_t;
}

lisp_val_t cc_input_stream_p(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) != TAG_INSTANCE) {
        return nil;
    }
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    if (obj[0] != MAGIC_STREAM) {
        return nil;
    }
    return stream_kind_is_input(stream_raw(val)->kind) ? g_sym_t : nil;
}

lisp_val_t cc_output_stream_p(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t val = cc_car(args);
    if ((val & TAG_MASK) != TAG_INSTANCE) {
        return nil;
    }
    UINT64 *obj = (UINT64 *)(val & ~TAG_MASK);
    if (obj[0] != MAGIC_STREAM) {
        return nil;
    }
    return stream_kind_is_output(stream_raw(val)->kind) ? g_sym_t : nil;
}

lisp_val_t cc_open_output_file(lisp_val_t args, lisp_val_t env) {
    (void)env;
    char path[STREAM_PATH_MAX];
    os_string_to_cstr(cc_car(args), path, sizeof(path));

    char relative[STREAM_PATH_MAX];
    lisp_val_t device;
    mount_kind_t kind = os_mount_resolve(path, relative, sizeof(relative), &device);

    // [ファイルI/O]#49で発覚したGC安全性バグへの対処(cc_open_input_streamと同じ理由):
    // rawの確保はアロケーションを伴いうる処理(os_mount_fat_resolve_file_node)の後、
    // os_make_streamの直前まで遅らせる
    if (kind == MOUNT_KIND_9P) {
        os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
        char err_msg[128];
        if (!os_stream_open_9p_file_write(raw, relative, 1 /* create_if_missing */, err_msg, sizeof(err_msg))) {
            /* [P4-4] 下位層のメッセージをそのまま condition へ載せる */
            return os_signal_io_error("open-output-file", path, err_msg, global_environment);
        }
        return os_make_stream(raw);
    }

    if (kind == MOUNT_KIND_FAT32 || kind == MOUNT_KIND_FAT16) {
        lisp_val_t node;
        lisp_val_t transfer = nil;
        GC_PROTECT(transfer);
        if (!os_mount_fat_resolve_file_node(kind, device, relative, 1 /* truncate */, 1 /* create_if_missing */,
                                             &node, &transfer)) {
            if (transfer != nil) {
                return transfer;
            }
            return os_signal_io_error("open-output-file", path, "cannot resolve file", global_environment);
        }
        // nodeはこの後のrawのアロケーション(GCを誘発しうる)を跨いで生存する必要がある
        // ため、書き込み先(os_stream_open_fat_file_write)に渡すまでGC_PROTECTする
        GC_PROTECT(node);
        os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
        os_stream_open_fat_file_write(raw, node);
        return os_make_stream(raw);
    }

    /* どのマウントにも解決できなかった(*mounts* に無いパス) */
    return os_signal_io_error("open-output-file", path, "no such mount", global_environment);
}

lisp_val_t cc_open_io_file(lisp_val_t args, lisp_val_t env) {
    (void)env;
    char path[STREAM_PATH_MAX];
    os_string_to_cstr(cc_car(args), path, sizeof(path));

    char relative[STREAM_PATH_MAX];
    lisp_val_t device;
    mount_kind_t kind = os_mount_resolve(path, relative, sizeof(relative), &device);

    // [ファイルI/O]#49で発覚したGC安全性バグへの対処(cc_open_input_streamと同じ理由):
    // rawの確保はアロケーションを伴いうる処理(os_mount_fat_resolve_file_node)の後、
    // os_make_streamの直前まで遅らせる
    if (kind == MOUNT_KIND_9P) {
        os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
        char err_msg[128];
        if (!os_stream_open_9p_file_io(raw, relative, 1 /* create_if_missing */, err_msg, sizeof(err_msg))) {
            /* [P4-4] 下位層のメッセージをそのまま condition へ載せる */
            return os_signal_io_error("open-io-file", path, err_msg, global_environment);
        }
        return os_make_stream(raw);
    }

    if (kind == MOUNT_KIND_FAT32 || kind == MOUNT_KIND_FAT16) {
        lisp_val_t node;
        lisp_val_t transfer = nil;
        GC_PROTECT(transfer);
        if (!os_mount_fat_resolve_file_node(kind, device, relative, 0 /* truncate */, 1 /* create_if_missing */,
                                             &node, &transfer)) {
            if (transfer != nil) {
                return transfer;
            }
            return os_signal_io_error("open-io-file", path, "cannot resolve file", global_environment);
        }
        // nodeはこの後のrawのアロケーション(GCを誘発しうる)を跨いで生存する必要がある
        // ため、書き込み先(os_stream_open_fat_file_io)に渡すまでGC_PROTECTする
        GC_PROTECT(node);
        os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
        os_stream_open_fat_file_io(raw, node);
        return os_make_stream(raw);
    }

    /* どのマウントにも解決できなかった(*mounts* に無いパス) */
    return os_signal_io_error("open-io-file", path, "no such mount", global_environment);
}

lisp_val_t cc_finish_output(lisp_val_t args, lisp_val_t env) {
    (void)env;
    // FAT系のfinish-outputはwrite-from!を呼び戻す
    lisp_val_t stream = cc_car(args);
    GC_PROTECT(stream);
    os_stream_finish_output(stream_raw(stream));
    lisp_val_t transfer = os_stream_take_transfer(stream);
    if (transfer != nil) {
        return transfer;
    }
    return nil;
}

lisp_val_t cc_create_string_input_stream(lisp_val_t args, lisp_val_t env) {
    (void)env;
    const UINT8 *data;
    UINT32 len;
    string_bytes(cc_car(args), &data, &len);

    // [ファイルI/O]#49で発覚したGC安全性バグへの対処: dataは元のSTRINGオブジェクトの
    // 内部を指す生ポインタなので、bufへコピーし終えるまでの間に他のアロケーションを
    // 挟まない(コピー自体はos_alloc_rawを1回呼ぶのみで、その間dataは未使用)
    UINT8 *buf = (UINT8 *)os_alloc_raw(len);
    for (UINT32 i = 0; i < len; i++) {
        buf[i] = data[i];
    }

    os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
    os_stream_open_string_input(raw, buf, len);
    return os_make_stream(raw);
}

lisp_val_t cc_create_string_output_stream(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    UINT8 *buf = (UINT8 *)os_alloc_raw(STREAM_STRING_OUTPUT_CAP);
    os_stream_t *raw = (os_stream_t *)os_alloc_raw(sizeof(os_stream_t));
    os_stream_open_string_output(raw, buf, STREAM_STRING_OUTPUT_CAP);
    return os_make_stream(raw);
}

lisp_val_t cc_get_output_stream_string(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_t *raw = stream_raw(cc_car(args));
    lisp_val_t result = make_string_from_bytes(raw->str_buf, raw->str_len);
    raw->str_len = 0; // 呼び出すたびに「前回呼び出し以降に書き込まれた分」へリセットする(仕様通り)
    return result;
}

lisp_val_t cc_preview_char(lisp_val_t args, lisp_val_t env) {
    lisp_val_t stream;
    int eos_error_p;
    lisp_val_t eos_value;
    resolve_input_args(args, &stream, &eos_error_p, &eos_value);
    GC_PROTECT(stream);
    GC_PROTECT(eos_value);
    os_stream_t *raw = stream_raw(stream);
    char ch;
    if (!os_stream_preview_char(raw, &ch)) {
        lisp_val_t transfer = os_stream_take_transfer(stream);
        if (transfer != nil) {
            return transfer;
        }
        return handle_end_of_stream(stream, eos_error_p, eos_value, env);
    }
    /* [4bit化] chはsigned char。UINT8を経由しないと0x80以上で符号拡張する */
    return os_make_char((UINT8)ch);
}

lisp_val_t cc_read_line(lisp_val_t args, lisp_val_t env) {
    lisp_val_t stream;
    int eos_error_p;
    lisp_val_t eos_value;
    resolve_input_args(args, &stream, &eos_error_p, &eos_value);
    GC_PROTECT(stream);
    GC_PROTECT(eos_value);

    #define READ_LINE_MAX 512
    char buf[READ_LINE_MAX];
    UINT32 n = 0;
    int got_any = 0;
    char ch;
    // **ループのたびにハンドルから取り直す。** FAT系のrefillはos_stream_tを
    // 再配置しうるので、ループの外で1回取った生ポインタは途中で古くなる
    while (os_stream_read_char(stream_raw(stream), &ch)) {
        got_any = 1;
        if (ch == '\n') {
            break;
        }
        if (n < READ_LINE_MAX - 1) {
            buf[n++] = ch;
        }
    }
    lisp_val_t transfer = os_stream_take_transfer(stream);
    if (transfer != nil) {
        return transfer;
    }
    if (!got_any) {
        return handle_end_of_stream(stream, eos_error_p, eos_value, env);
    }
    return make_string_from_bytes((const UINT8 *)buf, n);
    #undef READ_LINE_MAX
}

lisp_val_t cc_stream_ready_p(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    // 非同期I/Oが無く読み込みは常に同期的にブロックするため、常にtrueを返すスタブとする
    return g_sym_t;
}

/** cc_read_byteの固定引数版(read-file-into-vector等のバイト単位ループが
 * 1byteごとにconsリストを構築するコストを避けるため、consチェーンを経由せず
 * 直接呼べるようにする)。意味論はcc_read_byteと完全に同じ。 */
lisp_val_t cc_read_byte1(lisp_val_t stream) {
    // **ここは AOT 生成コードから直接呼ばれる**(file-cmd.lisp の
    // read-file-into-vector のループ)。生成側は戻り値を os_is_control_transfer で
    // 検査しているので、脱出値をそのまま返せば正しく伝播する
    GC_PROTECT(stream);
    char ch;
    if (!os_stream_read_char(stream_raw(stream), &ch)) {
        lisp_val_t transfer = os_stream_take_transfer(stream);
        if (transfer != nil) {
            return transfer;
        }
        return nil;
    }
    return os_make_fixnum((UINT64)(UINT8)ch);
}

lisp_val_t cc_read_byte(lisp_val_t args, lisp_val_t env) {
    lisp_val_t stream;
    int eos_error_p;
    lisp_val_t eos_value;
    resolve_input_args(args, &stream, &eos_error_p, &eos_value);
    GC_PROTECT(stream);
    GC_PROTECT(eos_value);
    lisp_val_t result = cc_read_byte1(stream);
    GC_PROTECT(result);
    // cc_read_byte1が脱出値を拾って返してくる(ここで二重に取りに行かない)
    if (os_is_control_transfer(result)) {
        return result;
    }
    if (result == nil) {
        return handle_end_of_stream(stream, eos_error_p, eos_value, env);
    }
    return result;
}

lisp_val_t cc_write_byte(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t z = cc_car(args);
    lisp_val_t stream = cc_car(cc_cdr(args));
    GC_PROTECT(stream);
    GC_PROTECT(z);
    os_stream_write_char(stream_raw(stream), (char)(UINT8)os_fixnum_magnitude(z));
    lisp_val_t transfer = os_stream_take_transfer(stream);
    if (transfer != nil) {
        return transfer;
    }
    return z;
}

lisp_val_t cc_probe_file(lisp_val_t args, lisp_val_t env) {
    (void)env;
    char path[STREAM_PATH_MAX];
    os_string_to_cstr(cc_car(args), path, sizeof(path));

    char relative[STREAM_PATH_MAX];
    lisp_val_t device;
    mount_kind_t kind = os_mount_resolve(path, relative, sizeof(relative), &device);

    if (kind == MOUNT_KIND_9P) {
        os_stream_t tmp;
        char err_msg[128];
        if (!os_stream_open_9p_file(&tmp, relative, err_msg, sizeof(err_msg))) {
            return nil;
        }
        os_stream_close(&tmp);
        return g_sym_t;
    }

    if (kind == MOUNT_KIND_FAT32 || kind == MOUNT_KIND_FAT16) {
        // FAT32/FAT16のread-fileはファイル無し・空ファイルのいずれもnilを返すため、
        // 中身が空の既存ファイルは無しと誤判定される既知の制約がある
        UINT8 *data;
        UINT32 len;
        lisp_val_t transfer = nil;
        GC_PROTECT(transfer);
        int ok = os_mount_fat_read_file(kind, device, relative, &data, &len, &transfer);
        if (transfer != nil) {
            return transfer;
        }
        return ok ? g_sym_t : nil;
    }

    return nil;
}

lisp_val_t cc_file_position(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_t *raw = stream_raw(cc_car(args));
    // [ファイルI/O]#49(M6): STREAM_FAT_FILE_WRITE/IOはbuf_data/write_buf/next_offset
    // (9Pと共用のバッファリング方式)へ移行したため、9Pのstream_9p_file系と同じ
    // next_offset(最後にrefill/flushした位置)ベースの近似値を返す
    if (raw->kind == STREAM_STRING_INPUT) {
        return os_make_fixnum(raw->str_pos);
    }
    if (raw->kind == STREAM_STRING_OUTPUT) {
        return os_make_fixnum(raw->str_len);
    }
    if (raw->kind == STREAM_9P_FILE_READ || raw->kind == STREAM_9P_FILE_IO || raw->kind == STREAM_FAT_FILE_IO) {
        // 読み込み側: next_offsetはチャンク単位で先読みした位置なので、バッファの未読み分と
        // preview-char用の先読み1文字を差し引いた論理位置(次にread-byte/read-charが返す
        // バイトの位置)を返す(ISLisp仕様§28の (read-byte) 後に (file-position) => 1 の例)
        UINT64 unread = raw->buf_count - raw->buf_pos;
        UINT64 pos = raw->next_offset - unread - (raw->has_lookahead ? 1 : 0);
        return os_make_fixnum(pos);
    }
    if (raw->kind == STREAM_9P_FILE_WRITE || raw->kind == STREAM_FAT_FILE_WRITE) {
        // 書き込み側: flush済みの位置に未flushのバッファ分を足した論理位置
        return os_make_fixnum(raw->next_offset + raw->write_buf_len);
    }
    return os_make_fixnum(raw->next_offset);
}

lisp_val_t cc_set_file_position(lisp_val_t args, lisp_val_t env) {
    (void)env;
    os_stream_t *raw = stream_raw(cc_car(args));
    lisp_val_t z = cc_car(cc_cdr(args));
    UINT64 newpos = os_fixnum_magnitude(z);

    raw->has_lookahead = 0;
    raw->eof = 0;
    if (raw->kind == STREAM_STRING_INPUT) {
        raw->str_pos = (UINT32)newpos;
    } else if (raw->kind == STREAM_STRING_OUTPUT) {
        raw->str_len = (UINT32)newpos;
    } else {
        raw->next_offset = newpos;
        raw->buf_pos = 0;
        raw->buf_count = 0;
    }
    return z;
}

/*
 * [P4-4] **file-length は失敗しても signal しない。**
 * spec:6858-6859「Returns the length of the file named by filename, or **returns nil
 * if the length cannot be determined**」。signal すべきと仕様が定めているのは
 * filename が文字列でない場合(error-id. domain-error、spec:6860)だけである。
 * したがってここは EVAL-ERROR 返しを signal ではなく **nil 返し**へ直す
 * (元の EVAL-ERROR は「長さが決められなかった」の実装内部の表現でしかなかった)。
 */
lisp_val_t cc_file_length(lisp_val_t args, lisp_val_t env) {
    (void)env;
    char path[STREAM_PATH_MAX];
    os_string_to_cstr(cc_car(args), path, sizeof(path));

    char relative[STREAM_PATH_MAX];
    lisp_val_t device;
    mount_kind_t kind = os_mount_resolve(path, relative, sizeof(relative), &device);

    if (kind == MOUNT_KIND_9P) {
        os_stream_t tmp;
        char err_msg[128];
        if (!os_stream_open_9p_file(&tmp, relative, err_msg, sizeof(err_msg))) {
            return nil;     /* 長さを決められなかった(spec:6858-6859) */
        }
        UINT64 count = 0;
        char ch;
        while (os_stream_read_char(&tmp, &ch)) {
            count++;
        }
        os_stream_close(&tmp);
        return os_make_fixnum(count);
    }

    if (kind == MOUNT_KIND_FAT32 || kind == MOUNT_KIND_FAT16) {
        // [ファイルI/O]#50(M7): os_mount_fat_read_file(ファイル全体読み込み)で
        // サイズだけを得ようとすると#41と同じ性能問題を抱えるため、
        // ディレクトリエントリの解決だけで済む軽量パスを使う
        UINT32 len;
        lisp_val_t transfer = nil;
        GC_PROTECT(transfer);
        if (!os_mount_fat_file_size(kind, device, relative, &len, &transfer)) {
            if (transfer != nil) {
                return transfer;
            }
            return nil;     /* 長さを決められなかった(spec:6858-6859) */
        }
        return os_make_fixnum(len);
    }

    return nil;             /* どのマウントにも解決できなかった(spec:6858-6859) */
}

void os_register_streams(void) {
    os_set_function(os_make_symbol("OPEN-INPUT-STREAM"), os_make_native_function((lisp_addr_t)(void *)cc_open_input_stream), global_environment);
    os_set_function(os_make_symbol("OPEN-OUTPUT-STREAM"), os_make_native_function((lisp_addr_t)(void *)cc_open_output_stream), global_environment);
    os_set_function(os_make_symbol("CLOSE"), os_make_native_function((lisp_addr_t)(void *)cc_close), global_environment);
    os_set_function(os_make_symbol("READ-CHAR"), os_make_native_function((lisp_addr_t)(void *)cc_read_char), global_environment);
    os_set_function(os_make_symbol("WRITE-CHAR"), os_make_native_function((lisp_addr_t)(void *)cc_write_char), global_environment);
    os_set_function(os_make_symbol("READ"), os_make_native_function((lisp_addr_t)(void *)cc_read), global_environment);
    os_set_function(os_make_symbol("OPEN-STREAM-P"), os_make_native_function((lisp_addr_t)(void *)cc_open_stream_p), global_environment);
    os_set_function(os_make_symbol("INPUT-STREAM-P"), os_make_native_function((lisp_addr_t)(void *)cc_input_stream_p), global_environment);
    os_set_function(os_make_symbol("OUTPUT-STREAM-P"), os_make_native_function((lisp_addr_t)(void *)cc_output_stream_p), global_environment);
    os_set_function(os_make_symbol("OPEN-OUTPUT-FILE"), os_make_native_function((lisp_addr_t)(void *)cc_open_output_file), global_environment);
    os_set_function(os_make_symbol("OPEN-IO-FILE"), os_make_native_function((lisp_addr_t)(void *)cc_open_io_file), global_environment);
    os_set_function(os_make_symbol("FINISH-OUTPUT"), os_make_native_function((lisp_addr_t)(void *)cc_finish_output), global_environment);
    os_set_function(os_make_symbol("CREATE-STRING-INPUT-STREAM"), os_make_native_function((lisp_addr_t)(void *)cc_create_string_input_stream), global_environment);
    os_set_function(os_make_symbol("CREATE-STRING-OUTPUT-STREAM"), os_make_native_function((lisp_addr_t)(void *)cc_create_string_output_stream), global_environment);
    os_set_function(os_make_symbol("GET-OUTPUT-STREAM-STRING"), os_make_native_function((lisp_addr_t)(void *)cc_get_output_stream_string), global_environment);
    os_set_function(os_make_symbol("PREVIEW-CHAR"), os_make_native_function((lisp_addr_t)(void *)cc_preview_char), global_environment);
    os_set_function(os_make_symbol("READ-LINE"), os_make_native_function((lisp_addr_t)(void *)cc_read_line), global_environment);
    os_set_function(os_make_symbol("STREAM-READY-P"), os_make_native_function((lisp_addr_t)(void *)cc_stream_ready_p), global_environment);
    os_set_function(os_make_symbol("READ-BYTE"), os_make_native_function((lisp_addr_t)(void *)cc_read_byte), global_environment);
    os_set_function(os_make_symbol("WRITE-BYTE"), os_make_native_function((lisp_addr_t)(void *)cc_write_byte), global_environment);
    os_set_function(os_make_symbol("PROBE-FILE"), os_make_native_function((lisp_addr_t)(void *)cc_probe_file), global_environment);
    os_set_function(os_make_symbol("FILE-POSITION"), os_make_native_function((lisp_addr_t)(void *)cc_file_position), global_environment);
    os_set_function(os_make_symbol("SET-FILE-POSITION"), os_make_native_function((lisp_addr_t)(void *)cc_set_file_position), global_environment);
    os_set_function(os_make_symbol("FILE-LENGTH"), os_make_native_function((lisp_addr_t)(void *)cc_file_length), global_environment);
    os_set_function(os_make_symbol("%%KEYBOARD-INPUT-STREAM"), os_make_native_function((lisp_addr_t)(void *)cc_keyboard_input_stream), global_environment);
}
