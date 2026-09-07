#include "stream.h"
#include "virtio9p.h"
#include "p9.h"
#include "runtime.h"
#include "mount.h"
#include "eval.h"

/** 1回のTreadで要求するバイト数(P9_MSIZE未満であれば十分) */
#define STREAM_READ_CHUNK 1024
/** デフォルトの作成パーミッション(ISLisp側にperm指定は無いため固定) */
#define STREAM_CREATE_PERM 0644

/**
 * [ファイルI/O]#49(M6): MAGIC_STREAMインスタンス(handle)からos_stream_t*を
 * 取り出す。stream_lisp.cのstream_raw/os_stream_from_lispと全く同じロジックだが、
 * stream_lisp.cをリンクしない一部のテストターゲット(test/c/stream_test.c等、
 * stream.cのみをリンクする)でも使えるよう、あえて重複させてここに直接持つ。
 * flush_write_buf_fat/refill_read_buf_fatが、Lisp呼び出し(GCを誘発しうる)の後に
 * (gc_relocate_stream(runtime.c)で再配置されたかもしれない)os_stream_t*を
 * 取り直すために使う。
 */
static os_stream_t *stream_from_handle(lisp_val_t handle) {
    UINT64 *obj = (UINT64 *)(lisp_addr_t)(handle & ~TAG_MASK);
    return (os_stream_t *)(lisp_addr_t)obj[1];
}

static void stream_init_common(os_stream_t *stream, stream_kind_t kind) {
    stream->kind = kind;
    stream->fid = 0;
    stream->next_offset = 0;
    stream->buf_count = 0;
    stream->buf_pos = 0;
    stream->write_buf_len = 0;
    stream->eof = 0;
    stream->error = 0;
    stream->out_fb = 0;
    stream->str_buf = 0;
    stream->str_cap = 0;
    stream->str_len = 0;
    stream->str_pos = 0;
    stream->has_lookahead = 0;
    stream->lookahead = 0;
    stream->column = 0;
    stream->closed = 0;
    stream->mount_file_node = nil;
    stream->self_handle = nil;
}

/** write_bufに溜まっている内容をTwriteで送出し、成功したらnext_offsetを進める */
static void flush_write_buf_9p(os_stream_t *stream) {
    if (stream->write_buf_len == 0) {
        return;
    }
    char err_msg[128];
    UINT32 written;
    if (!os_virtio9p_write_chunk(stream->fid, stream->next_offset, stream->write_buf,
                                  stream->write_buf_len, &written, err_msg, sizeof(err_msg))) {
        stream->error = 1;
        stream->write_buf_len = 0;
        return;
    }
    stream->next_offset += written;
    stream->write_buf_len = 0;
}

static int is_9p_write_kind(stream_kind_t kind) {
    return kind == STREAM_9P_FILE_WRITE || kind == STREAM_9P_FILE_IO;
}

static int is_9p_read_kind(stream_kind_t kind) {
    return kind == STREAM_9P_FILE_READ || kind == STREAM_9P_FILE_IO;
}

static int is_fat_write_kind(stream_kind_t kind) {
    return kind == STREAM_FAT_FILE_WRITE || kind == STREAM_FAT_FILE_IO;
}

static int is_fat_read_kind(stream_kind_t kind) {
    return kind == STREAM_FAT_FILE_IO;
}

/**
 * [ファイルI/O]#49(M6): write_bufに溜まっている内容をwrite-from!でnodeへflushし、
 * 成功したらnext_offsetを進める(flush_write_buf_9pのFAT版)。write-from!は
 * バイト列を評価済みvector引数として要求するため、write_buf(生のUINT8配列)から
 * 一時vectorへ変換する(この一時vectorはこの呼び出しの間だけ生存すればよく、
 * #41のような生存ヒープ肥大化の問題は生じない)。
 */
static os_stream_t *flush_write_buf_fat(os_stream_t *stream) {
    if (stream->write_buf_len == 0) {
        return stream;
    }
    lisp_val_t write_fn = os_get_function(os_make_symbol("WRITE-FROM!"), global_environment);
    if (write_fn == nil) {
        stream->error = 1;
        stream->write_buf_len = 0;
        return stream;
    }
    GC_PROTECT(write_fn);

    // [ファイルI/O]#49(M6)で発見・修正: 以下はどれもLisp側の割り当てを伴い、streamの
    // GC_PROTECT済みの生ポインタでも、streamのラッパー(MAGIC_STREAM)自体は
    // 生きているぶん構わずgc_relocate_stream(runtime.c)がos_stream_t本体を
    // 別アドレスへ再配置してしまう(mount_file_nodeと同様、他プロセスのアロケーション
    // 経由ではなく自分自身のアロケーションでも起こりうる)。streamから読み出す値は
    // 一切アロケーションを挟む前にすべてローカル変数(node/offset/len/local_buf)へ
    // 複製しておき、以後は生ポインタstreamを直接読まない。self_handle(GC_PROTECT
    // 済み)を使い、Lisp呼び出し後にos_stream_from_lispで生ポインタを取り直してから
    // 書き込む。
    lisp_val_t self_handle = stream->self_handle;
    GC_PROTECT(self_handle);
    lisp_val_t node = stream->mount_file_node;
    GC_PROTECT(node);
    UINT32 len = stream->write_buf_len;
    UINT64 offset = stream->next_offset;
    UINT8 local_buf[sizeof(stream->write_buf)];
    for (UINT32 i = 0; i < len; i++) {
        local_buf[i] = stream->write_buf[i];
    }

#ifndef ISIKIOS_UNIT_TEST
    asm volatile ("cli");
#endif
    lisp_val_t buf = primitive_create_vector(os_make_cons(os_make_fixnum(len), nil), nil);
    GC_PROTECT(buf);
    lisp_val_t *buf_data = (lisp_val_t *)((lisp_addr_t)os_vector_header(buf) + 16);
    for (UINT32 i = 0; i < len; i++) {
        buf_data[i] = os_make_fixnum((UINT64)local_buf[i]);
    }
#ifndef ISIKIOS_UNIT_TEST
    asm volatile ("sti");
#endif

    lisp_val_t args = os_make_cons(node,
                       os_make_cons(buf,
                       os_make_cons(os_make_fixnum(0),
                       os_make_cons(os_make_fixnum(offset),
                       os_make_cons(os_make_fixnum(len), nil)))));
    GC_PROTECT(args);
    lisp_val_t result = os_apply_function(write_fn, args, global_environment);

    // ここまでのいずれかのアロケーションでstreamが再配置されている可能性があるため、
    // GC_PROTECT済みのself_handle経由で必ず生ポインタを取り直す
    if (self_handle != nil) {
        stream = stream_from_handle(self_handle);
    }

    if (result == nil) {
        stream->error = 1;
        stream->write_buf_len = 0;
        return stream;
    }
    stream->next_offset = offset + len;
    stream->write_buf_len = 0;
    return stream;
}

/**
 * [ファイルI/O]#49(M6): stream->buf_data/buf_countをread-into!でrefillする
 * (9PのTread相当)。read-into!は評価済みvectorへ書き込む契約のため、
 * STREAM_READ_CHUNK分の一時vectorを渡してから内容をbuf_dataへコピーする。
 */
static os_stream_t *refill_read_buf_fat(os_stream_t *stream) {
    lisp_val_t read_fn = os_get_function(os_make_symbol("READ-INTO!"), global_environment);
    if (read_fn == nil) {
        stream->error = 1;
        return stream;
    }
    GC_PROTECT(read_fn);

    // flush_write_buf_fatと同じ理由([ファイルI/O]#49(M6)参照): アロケーションを
    // 挟む前にstreamから必要な値をローカル変数へ複製し、self_handleをGC_PROTECT
    // した上でLisp呼び出し後に生ポインタを取り直す。
    lisp_val_t self_handle = stream->self_handle;
    GC_PROTECT(self_handle);
    lisp_val_t node = stream->mount_file_node;
    GC_PROTECT(node);
    UINT64 offset = stream->next_offset;

#ifndef ISIKIOS_UNIT_TEST
    asm volatile ("cli");
#endif
    lisp_val_t buf = primitive_create_vector(os_make_cons(os_make_fixnum(STREAM_READ_CHUNK), nil), nil);
    GC_PROTECT(buf);
#ifndef ISIKIOS_UNIT_TEST
    asm volatile ("sti");
#endif

    lisp_val_t args = os_make_cons(node,
                       os_make_cons(buf,
                       os_make_cons(os_make_fixnum(0),
                       os_make_cons(os_make_fixnum(offset),
                       os_make_cons(os_make_fixnum(STREAM_READ_CHUNK), nil)))));
    GC_PROTECT(args);
    lisp_val_t result = os_apply_function(read_fn, args, global_environment);
    UINT64 got = (result == nil) ? 0 : os_fixnum_magnitude(result);
    if (got > STREAM_READ_CHUNK) {
        got = STREAM_READ_CHUNK;
    }

    // ここまでのいずれかのアロケーションでstreamが再配置されている可能性があるため、
    // GC_PROTECT済みのself_handle経由で必ず生ポインタを取り直す
    if (self_handle != nil) {
        stream = stream_from_handle(self_handle);
    }

#ifndef ISIKIOS_UNIT_TEST
    asm volatile ("cli");
#endif
    lisp_val_t *buf_data = (lisp_val_t *)((lisp_addr_t)os_vector_header(buf) + 16);
    for (UINT32 i = 0; i < got; i++) {
        stream->buf_data[i] = (UINT8)os_fixnum_magnitude(buf_data[i]);
    }
#ifndef ISIKIOS_UNIT_TEST
    asm volatile ("sti");
#endif
    stream->buf_count = (UINT32)got;
    stream->buf_pos = 0;
    stream->next_offset = offset + got;
    if (got == 0) {
        stream->eof = 1;
    }
    return stream;
}

int os_stream_open_9p_file(os_stream_t *stream, const char *path, char *err_msg, UINT32 err_msg_cap) {
    stream_init_common(stream, STREAM_9P_FILE_READ);
    if (!os_virtio9p_open(path, P9_OREAD, &stream->fid, err_msg, err_msg_cap)) {
        stream->error = 1;
        return 0;
    }
    return 1;
}

int os_stream_open_9p_file_write(os_stream_t *stream, const char *path, int create_if_missing, char *err_msg, UINT32 err_msg_cap) {
    stream_init_common(stream, STREAM_9P_FILE_WRITE);
    if (os_virtio9p_open(path, (UINT8)(P9_OWRITE | P9_OTRUNC), &stream->fid, err_msg, err_msg_cap)) {
        return 1;
    }
    if (create_if_missing &&
        os_virtio9p_create(path, STREAM_CREATE_PERM, P9_OWRITE, &stream->fid, err_msg, err_msg_cap)) {
        return 1;
    }
    stream->error = 1;
    return 0;
}

int os_stream_open_9p_file_io(os_stream_t *stream, const char *path, int create_if_missing, char *err_msg, UINT32 err_msg_cap) {
    stream_init_common(stream, STREAM_9P_FILE_IO);
    if (os_virtio9p_open(path, (UINT8)(P9_ORDWR | P9_OTRUNC), &stream->fid, err_msg, err_msg_cap)) {
        return 1;
    }
    if (create_if_missing &&
        os_virtio9p_create(path, STREAM_CREATE_PERM, P9_ORDWR, &stream->fid, err_msg, err_msg_cap)) {
        return 1;
    }
    stream->error = 1;
    return 0;
}

void os_stream_open_screen_output(os_stream_t *stream, frame_buffer *fb) {
    stream_init_common(stream, STREAM_OUTPUT_SCREEN);
    stream->out_fb = fb;
}

void os_stream_open_string_input(os_stream_t *stream, UINT8 *buf, UINT32 len) {
    stream_init_common(stream, STREAM_STRING_INPUT);
    stream->str_buf = buf;
    stream->str_cap = len;
    stream->str_len = len;
    stream->str_pos = 0;
}

void os_stream_open_string_output(os_stream_t *stream, UINT8 *buf, UINT32 cap) {
    stream_init_common(stream, STREAM_STRING_OUTPUT);
    stream->str_buf = buf;
    stream->str_cap = cap;
    stream->str_len = 0;
}

/**
 * mount_file_nodeはos_stream_t(MAGIC_STREAM)本体の一部としてgc_relocate_stream
 * (runtime.c)がcons/vectorの要素と同じ要領でgc_copy_valueし直すため、streamの
 * open/close時に個別のGCルート登録は不要([ファイルI/O]#49で修正: 以前は
 * os_gc_register_root/os_gc_unregister_rootしていたが、os_stream_t自体がGCの
 * たびに別アドレスへ再配置される構造体であるため、固定アドレスを覚えるだけの
 * os_gc_register_rootでは1回目のGC後にアドレスが無効化し、2回目以降のGCで
 * 無関係なメモリを破壊していた)。
 */
void os_stream_open_fat_file_write(os_stream_t *stream, lisp_val_t file_node) {
    stream_init_common(stream, STREAM_FAT_FILE_WRITE);
    stream->mount_file_node = file_node;
}

void os_stream_open_fat_file_io(os_stream_t *stream, lisp_val_t file_node) {
    stream_init_common(stream, STREAM_FAT_FILE_IO);
    stream->mount_file_node = file_node;
}

int os_stream_read_char(os_stream_t *stream, char *out_ch) {
    if (stream->closed || stream->eof || stream->error) {
        return 0;
    }

    if (stream->has_lookahead) {
        *out_ch = stream->lookahead;
        stream->has_lookahead = 0;
        return 1;
    }

    if (is_9p_read_kind(stream->kind)) {
        if (stream->buf_pos == stream->buf_count) {
            /* IOストリームで書き込みバッファが残っていれば、読み込み前に反映させる */
            if (stream->kind == STREAM_9P_FILE_IO) {
                flush_write_buf_9p(stream);
                if (stream->error) {
                    return 0;
                }
            }
            char err_msg[128];
            const UINT8 *chunk_data;
            UINT32 chunk_count;
            if (!os_virtio9p_read_chunk(stream->fid, stream->next_offset, STREAM_READ_CHUNK,
                                         &chunk_data, &chunk_count,
                                         err_msg, sizeof(err_msg))) {
                stream->error = 1;
                return 0;
            }
            /* chunk_dataは全9pストリームで共有される受信バッファへの参照なので、
               他のストリームのTreadで上書きされる前にstream自身へコピーしておく */
            for (UINT32 i = 0; i < chunk_count; i++) {
                stream->buf_data[i] = chunk_data[i];
            }
            stream->buf_count = chunk_count;
            stream->buf_pos = 0;
            stream->next_offset += stream->buf_count;
            if (stream->buf_count == 0) {
                stream->eof = 1;
                return 0;
            }
        }
        *out_ch = (char)stream->buf_data[stream->buf_pos];
        stream->buf_pos++;
        return 1;
    }

    if (is_fat_read_kind(stream->kind)) {
        if (stream->buf_pos == stream->buf_count) {
            /* IOストリームで書き込みバッファが残っていれば、読み込み前に反映させる
               (9PのSTREAM_9P_FILE_IOと同じ理由)。flush/refillはstreamを再配置
               しうるため、必ず戻り値で呼び出し元のポインタを更新する
               ([ファイルI/O]#49(M6)参照) */
            stream = flush_write_buf_fat(stream);
            if (stream->error) {
                return 0;
            }
            stream = refill_read_buf_fat(stream);
            if (stream->error || stream->buf_count == 0) {
                return 0;
            }
        }
        *out_ch = (char)stream->buf_data[stream->buf_pos];
        stream->buf_pos++;
        return 1;
    }

    if (stream->kind == STREAM_STRING_INPUT) {
        if (stream->str_pos >= stream->str_len) {
            stream->eof = 1;
            return 0;
        }
        *out_ch = (char)stream->str_buf[stream->str_pos];
        stream->str_pos++;
        return 1;
    }

    return 0;
}

int os_stream_preview_char(os_stream_t *stream, char *out_ch) {
    if (stream->has_lookahead) {
        *out_ch = stream->lookahead;
        return 1;
    }
    char ch;
    if (!os_stream_read_char(stream, &ch)) {
        return 0;
    }
    stream->has_lookahead = 1;
    stream->lookahead = ch;
    *out_ch = ch;
    return 1;
}

/** column(format ~T用)を1文字分更新する */
static void advance_column(os_stream_t *stream, char ch) {
    if (ch == '\n') {
        stream->column = 0;
    } else {
        stream->column++;
    }
}

int os_stream_write_char(os_stream_t *stream, char ch) {
    if (stream->closed) {
        return 0;
    }

    if (stream->kind == STREAM_OUTPUT_SCREEN) {
        stream->out_fb->write_char(stream->out_fb, (UINT8)ch);
        advance_column(stream, ch);
        return 1;
    }

    if (is_9p_write_kind(stream->kind)) {
        if (stream->kind == STREAM_9P_FILE_IO) {
            /* 書き込み位置がずれるため、読み込み済みバッファは無効化する */
            stream->buf_pos = 0;
            stream->buf_count = 0;
        }
        stream->write_buf[stream->write_buf_len] = (UINT8)ch;
        stream->write_buf_len++;
        if (stream->write_buf_len == sizeof(stream->write_buf)) {
            flush_write_buf_9p(stream);
        }
        advance_column(stream, ch);
        return !stream->error;
    }

    if (is_fat_write_kind(stream->kind)) {
        if (stream->kind == STREAM_FAT_FILE_IO) {
            /* 書き込み位置がずれるため、読み込み済みバッファは無効化する
               (9PのSTREAM_9P_FILE_IOと同じ理由) */
            stream->buf_pos = 0;
            stream->buf_count = 0;
        }
        stream->write_buf[stream->write_buf_len] = (UINT8)ch;
        stream->write_buf_len++;
        if (stream->write_buf_len == sizeof(stream->write_buf)) {
            // flushはstreamを再配置しうるため、必ず戻り値で呼び出し元のポインタを
            // 更新する([ファイルI/O]#49(M6)参照)
            stream = flush_write_buf_fat(stream);
        }
        advance_column(stream, ch);
        return !stream->error;
    }

    if (stream->kind == STREAM_STRING_OUTPUT) {
        if (stream->str_len < stream->str_cap) {
            stream->str_buf[stream->str_len] = (UINT8)ch;
            stream->str_len++;
        }
        advance_column(stream, ch);
        return 1;
    }

    return 0;
}

void os_stream_finish_output(os_stream_t *stream) {
    if (is_9p_write_kind(stream->kind)) {
        flush_write_buf_9p(stream);
    }
    if (is_fat_write_kind(stream->kind)) {
        flush_write_buf_fat(stream);
    }
}

void os_stream_close(os_stream_t *stream) {
    if (is_9p_write_kind(stream->kind)) {
        flush_write_buf_9p(stream);
    }
    if (is_9p_read_kind(stream->kind) || is_9p_write_kind(stream->kind)) {
        char err_msg[128];
        os_virtio9p_close(stream->fid, err_msg, sizeof(err_msg));
    }
    if (is_fat_write_kind(stream->kind)) {
        // flushはstreamを再配置しうるため、必ず戻り値で呼び出し元のポインタを
        // 更新してからclosedフィールドへ書き込む([ファイルI/O]#49(M6)参照)
        stream = flush_write_buf_fat(stream);
    }
    stream->closed = 1;
}
