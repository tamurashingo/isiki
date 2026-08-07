#include "stream.h"
#include "virtio9p.h"

/** 1回のTreadで要求するバイト数(P9_MSIZE未満であれば十分) */
#define STREAM_READ_CHUNK 1024

int os_stream_open_9p_file(os_stream_t *stream, const char *path, char *err_msg, UINT32 err_msg_cap) {
    stream->kind = STREAM_9P_FILE;
    stream->next_offset = 0;
    stream->buf_data = 0;
    stream->buf_count = 0;
    stream->buf_pos = 0;
    stream->eof = 0;
    stream->error = 0;
    stream->out_fb = 0;
    stream->closed = 0;

    if (!os_virtio9p_open(path, &stream->fid, err_msg, err_msg_cap)) {
        stream->error = 1;
        return 0;
    }
    return 1;
}

void os_stream_open_screen_output(os_stream_t *stream, frame_buffer *fb) {
    stream->kind = STREAM_OUTPUT_SCREEN;
    stream->next_offset = 0;
    stream->buf_data = 0;
    stream->buf_count = 0;
    stream->buf_pos = 0;
    stream->eof = 0;
    stream->error = 0;
    stream->out_fb = fb;
    stream->closed = 0;
}

int os_stream_read_char(os_stream_t *stream, char *out_ch) {
    if (stream->closed || stream->eof || stream->error) {
        return 0;
    }

    if (stream->buf_pos == stream->buf_count) {
        char err_msg[128];
        if (!os_virtio9p_read_chunk(stream->fid, stream->next_offset, STREAM_READ_CHUNK,
                                     &stream->buf_data, &stream->buf_count,
                                     err_msg, sizeof(err_msg))) {
            stream->error = 1;
            return 0;
        }
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

int os_stream_write_char(os_stream_t *stream, char ch) {
    if (stream->closed || stream->kind != STREAM_OUTPUT_SCREEN) {
        return 0;
    }
    stream->out_fb->write_char(stream->out_fb, (UINT8)ch);
    return 1;
}

void os_stream_close(os_stream_t *stream) {
    if (stream->kind == STREAM_9P_FILE) {
        char err_msg[128];
        os_virtio9p_close(stream->fid, err_msg, sizeof(err_msg));
    }
    stream->closed = 1;
}
