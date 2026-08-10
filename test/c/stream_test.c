#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "test_assert.h"
#include "types.h"
#include "stream.h"

// stream.cはos_alloc_raw(runtime.c)を参照するが、このテストはruntime.cをリンクしない
// (stream.cのみを対象とするため)。mallocへの薄い委譲で置き換える
lisp_addr_t os_alloc_raw(UINT64 n) {
    return (lisp_addr_t)malloc(n);
}

// virtio9p.c/p9.c/drivers/*.c はリンクしない(9Pプロトコルの実通信は
// stream.c の責務ではないため)。os_virtio9p_open/read_chunk/close を
// フェイク実装に差し替え、インメモリのバイト列を不揃いなチャンクサイズで
// 返すことで、os_stream_read_char のバッファリング/EOF/エラー処理を検証する。

#define FAKE_DATA_MAX 256
#define FAKE_CHUNK_SIZES_MAX 16

static UINT8 g_fake_data[FAKE_DATA_MAX];
static UINT32 g_fake_data_len = 0;
static UINT32 g_fake_chunk_sizes[FAKE_CHUNK_SIZES_MAX];
static UINT32 g_fake_chunk_count = 0;
static UINT32 g_fake_chunk_index = 0;
static UINT32 g_fake_read_calls = 0;
static int g_fake_open_fail = 0;
static int g_fake_read_fail_after = -1; // -1: 失敗させない。N: N回目の呼び出しから失敗させる

static void set_fake_data(const char *s) {
    UINT32 n = 0;
    while (s[n] != '\0' && n < FAKE_DATA_MAX) {
        g_fake_data[n] = (UINT8)s[n];
        n++;
    }
    g_fake_data_len = n;
}

static void set_fake_chunks(const UINT32 *sizes, UINT32 count) {
    for (UINT32 i = 0; i < count; i++) {
        g_fake_chunk_sizes[i] = sizes[i];
    }
    g_fake_chunk_count = count;
    g_fake_chunk_index = 0;
}

static void reset_fake_state(void) {
    g_fake_chunk_index = 0;
    g_fake_read_calls = 0;
    g_fake_open_fail = 0;
    g_fake_read_fail_after = -1;
}

int os_virtio9p_open(const char *path, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path;
    (void)mode;
    if (g_fake_open_fail) {
        if (err_msg != 0 && err_msg_cap > 0) {
            snprintf(err_msg, err_msg_cap, "fake open failure");
        }
        return 0;
    }
    *out_fid = 1;
    return 1;
}

int os_virtio9p_create(const char *path, UINT32 perm, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path;
    (void)perm;
    (void)mode;
    (void)err_msg;
    (void)err_msg_cap;
    *out_fid = 1;
    return 1;
}

int os_virtio9p_write_chunk(UINT32 fid, UINT64 offset, const UINT8 *data, UINT32 count,
                             UINT32 *out_written, char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)offset;
    (void)data;
    (void)err_msg;
    (void)err_msg_cap;
    *out_written = count;
    return 1;
}

int os_virtio9p_read_chunk(UINT32 fid, UINT64 offset, UINT32 want,
                            const UINT8 **out_data, UINT32 *out_count,
                            char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)want;
    g_fake_read_calls++;

    if (g_fake_read_fail_after >= 0 && (UINT32)g_fake_read_fail_after < g_fake_read_calls) {
        if (err_msg != 0 && err_msg_cap > 0) {
            snprintf(err_msg, err_msg_cap, "fake read failure");
        }
        return 0;
    }

    UINT32 off = (UINT32)offset;
    UINT32 remaining = g_fake_data_len - off;
    UINT32 count = remaining;
    if (g_fake_chunk_index < g_fake_chunk_count) {
        UINT32 sz = g_fake_chunk_sizes[g_fake_chunk_index++];
        if (sz < count) {
            count = sz;
        }
    }

    *out_data = g_fake_data + off;
    *out_count = count;
    return 1;
}

int os_virtio9p_close(UINT32 fid, char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)err_msg;
    (void)err_msg_cap;
    return 1;
}

static void test_read_all_uneven_chunks(void) {
    reset_fake_state();
    set_fake_data("ABCDEFGHIJ");
    UINT32 chunks[] = {3, 4, 3};
    set_fake_chunks(chunks, 3);

    os_stream_t stream;
    char err_msg[128];
    int opened = os_stream_open_9p_file(&stream, "fake/path", err_msg, sizeof(err_msg));
    assert(opened, "不揃いなチャンクサイズでもストリームをopenできる");

    char collected[FAKE_DATA_MAX];
    UINT32 n = 0;
    char ch;
    while (os_stream_read_char(&stream, &ch)) {
        collected[n++] = ch;
    }
    collected[n] = '\0';

    assert(n == g_fake_data_len, "読み込んだ文字数が元データと一致する");
    assert(memcmp(collected, "ABCDEFGHIJ", 10) == 0, "読み込んだ内容が元データと一致する");
    assert(stream.eof == 1, "全部読み終えたらeofが立つ");
    assert(stream.error == 0, "正常終了時はerrorが立たない");

    os_stream_close(&stream);
}

static void test_read_all_single_byte_chunks(void) {
    reset_fake_state();
    set_fake_data("XY");
    UINT32 chunks[] = {1, 1};
    set_fake_chunks(chunks, 2);

    os_stream_t stream;
    char err_msg[128];
    assert(os_stream_open_9p_file(&stream, "fake/path", err_msg, sizeof(err_msg)),
           "1バイトずつのチャンクでもopenできる");

    char c1, c2, c3;
    assert(os_stream_read_char(&stream, &c1) && c1 == 'X', "1文字目を読める");
    assert(os_stream_read_char(&stream, &c2) && c2 == 'Y', "2文字目を読める(再充填が発生する)");
    assert(!os_stream_read_char(&stream, &c3), "3文字目はEOFで読めない");
    assert(stream.eof == 1, "EOFフラグが立つ");

    os_stream_close(&stream);
}

static void test_open_failure_sets_error(void) {
    reset_fake_state();
    g_fake_open_fail = 1;

    os_stream_t stream;
    char err_msg[128];
    int opened = os_stream_open_9p_file(&stream, "fake/missing", err_msg, sizeof(err_msg));
    assert(!opened, "openが失敗したら0を返す");
    assert(stream.error == 1, "openの失敗はstream->errorに反映される");
}

static void test_read_failure_mid_stream(void) {
    reset_fake_state();
    set_fake_data("ABCDEFGHIJ");
    UINT32 chunks[] = {4};
    set_fake_chunks(chunks, 1);
    g_fake_read_fail_after = 1; // 1回目のTreadは成功、2回目から失敗させる

    os_stream_t stream;
    char err_msg[128];
    assert(os_stream_open_9p_file(&stream, "fake/path", err_msg, sizeof(err_msg)),
           "openは成功する");

    char collected[FAKE_DATA_MAX];
    UINT32 n = 0;
    char ch;
    while (os_stream_read_char(&stream, &ch)) {
        collected[n++] = ch;
    }

    assert(n == 4, "1回目のチャンク分だけ読めたところで停止する");
    assert(memcmp(collected, "ABCD", 4) == 0, "停止前に読めた内容は正しい");
    assert(stream.error == 1, "I/Oエラーはerrorフラグで示される(EOFとは区別される)");
    assert(stream.eof == 0, "I/Oエラーはeofフラグを立てない");

    os_stream_close(&stream);
}

static void test_empty_file_is_immediate_eof(void) {
    reset_fake_state();
    set_fake_data("");
    UINT32 chunks[] = {0};
    (void)chunks;

    os_stream_t stream;
    char err_msg[128];
    assert(os_stream_open_9p_file(&stream, "fake/empty", err_msg, sizeof(err_msg)),
           "空ファイルでもopenは成功する");

    char ch;
    assert(!os_stream_read_char(&stream, &ch), "空ファイルは最初の読み込みでEOFになる");
    assert(stream.eof == 1, "空ファイルはeofフラグが立つ");
    assert(stream.error == 0, "空ファイルはerrorフラグが立たない");

    os_stream_close(&stream);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    test_read_all_uneven_chunks();
    test_read_all_single_byte_chunks();
    test_open_failure_sets_error();
    test_read_failure_mid_stream();
    test_empty_file_is_immediate_eof();

    return g_test_failed ? 1 : 0;
}
