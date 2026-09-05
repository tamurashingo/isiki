#include "stream.h"
#include "virtio9p.h"
#include "p9.h"
#include "runtime.h"
#include "mount.h"

/** 1回のTreadで要求するバイト数(P9_MSIZE未満であれば十分) */
#define STREAM_READ_CHUNK 1024
/** デフォルトの作成パーミッション(ISLisp側にperm指定は無いため固定) */
#define STREAM_CREATE_PERM 0644

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
    stream->mount_fs_kind = MOUNT_KIND_NONE;
    stream->mount_device = nil;
    stream->mount_relative_path[0] = '\0';
}

/** relative_pathをstream->mount_relative_pathへコピーする(STREAM_PATH_MAX境界内) */
static void set_mount_relative_path(os_stream_t *stream, const char *relative_path) {
    UINT32 i = 0;
    while (relative_path[i] != '\0' && i + 1 < STREAM_PATH_MAX) {
        stream->mount_relative_path[i] = relative_path[i];
        i++;
    }
    stream->mount_relative_path[i] = '\0';
}

/** write_bufに溜まっている内容をTwriteで送出し、成功したらnext_offsetを進める */
static void flush_write_buf(os_stream_t *stream) {
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

void os_stream_open_string_input(os_stream_t *stream, const char *data, UINT32 len) {
    stream_init_common(stream, STREAM_STRING_INPUT);
    stream->str_buf = (UINT8 *)os_alloc_raw(len);
    for (UINT32 i = 0; i < len; i++) {
        stream->str_buf[i] = (UINT8)data[i];
    }
    stream->str_cap = len;
    stream->str_len = len;
    stream->str_pos = 0;
}

void os_stream_open_string_output(os_stream_t *stream) {
    stream_init_common(stream, STREAM_STRING_OUTPUT);
    stream->str_buf = (UINT8 *)os_alloc_raw(STREAM_STRING_OUTPUT_CAP);
    stream->str_cap = STREAM_STRING_OUTPUT_CAP;
    stream->str_len = 0;
}

/**
 * mount_deviceはos_alloc_raw確保のos_stream_t(GCの走査対象外の生メモリ)内に置く
 * lisp_val_tフィールドなので、GCが動くたびに追跡・更新されるようos_gc_register_root
 * で明示的にrootへ加える(process.cの各プロセスenvと同じ理由。os_stream_close側で
 * os_gc_unregister_rootする)。
 */
void os_stream_open_fat_file_write(os_stream_t *stream, mount_kind_t kind, lisp_val_t device, const char *relative_path) {
    stream_init_common(stream, STREAM_FAT_FILE_WRITE);
    stream->mount_fs_kind = kind;
    stream->mount_device = device;
    os_gc_register_root(&stream->mount_device);
    set_mount_relative_path(stream, relative_path);
    stream->str_buf = (UINT8 *)os_alloc_raw(STREAM_FAT_FILE_CAP);
    stream->str_cap = STREAM_FAT_FILE_CAP;
    stream->str_len = 0;
}

void os_stream_open_fat_file_io(os_stream_t *stream, mount_kind_t kind, lisp_val_t device, const char *relative_path) {
    stream_init_common(stream, STREAM_FAT_FILE_IO);
    stream->mount_fs_kind = kind;
    stream->mount_device = device;
    os_gc_register_root(&stream->mount_device);
    set_mount_relative_path(stream, relative_path);
    stream->str_buf = (UINT8 *)os_alloc_raw(STREAM_FAT_FILE_CAP);
    stream->str_cap = STREAM_FAT_FILE_CAP;
    stream->str_len = 0;
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
                flush_write_buf(stream);
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

    if (stream->kind == STREAM_STRING_INPUT || stream->kind == STREAM_FAT_FILE_IO) {
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
            flush_write_buf(stream);
        }
        advance_column(stream, ch);
        return !stream->error;
    }

    if (stream->kind == STREAM_STRING_OUTPUT || stream->kind == STREAM_FAT_FILE_WRITE || stream->kind == STREAM_FAT_FILE_IO) {
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
        flush_write_buf(stream);
    }
}

void os_stream_close(os_stream_t *stream) {
    if (is_9p_write_kind(stream->kind)) {
        flush_write_buf(stream);
    }
    if (is_9p_read_kind(stream->kind) || is_9p_write_kind(stream->kind)) {
        char err_msg[128];
        os_virtio9p_close(stream->fid, err_msg, sizeof(err_msg));
    }
    if (stream->kind == STREAM_FAT_FILE_WRITE || stream->kind == STREAM_FAT_FILE_IO) {
        os_mount_fat_write_file(stream->mount_fs_kind, stream->mount_device,
                                 stream->mount_relative_path, stream->str_buf, stream->str_len);
        os_gc_unregister_root(&stream->mount_device);
    }
    stream->closed = 1;
}
