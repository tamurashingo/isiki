#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "process.h"

// runtime.c/process.c/za.c/reader.c/stream.cをリンクするため、それらが参照する
// ハードウェア/REPL依存の関数のダミー実装が必要になる(runtime_test.cと同じパターン)
int os_virtio9p_open(const char *path, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path;
    (void)mode;
    (void)out_fid;
    (void)err_msg;
    (void)err_msg_cap;
    return 0;
}

int os_virtio9p_create(const char *path, UINT32 perm, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path;
    (void)perm;
    (void)mode;
    (void)out_fid;
    (void)err_msg;
    (void)err_msg_cap;
    return 0;
}

int os_virtio9p_write_chunk(UINT32 fid, UINT64 offset, const UINT8 *data, UINT32 count,
                             UINT32 *out_written, char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)offset;
    (void)data;
    (void)count;
    (void)out_written;
    (void)err_msg;
    (void)err_msg_cap;
    return 0;
}

int os_virtio9p_read_chunk(UINT32 fid, UINT64 offset, UINT32 want,
                            const UINT8 **out_data, UINT32 *out_count,
                            char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)offset;
    (void)want;
    (void)out_data;
    (void)out_count;
    (void)err_msg;
    (void)err_msg_cap;
    return 0;
}

int os_virtio9p_close(UINT32 fid, char *err_msg, UINT32 err_msg_cap) {
    (void)fid;
    (void)err_msg;
    (void)err_msg_cap;
    return 0;
}

static void dummy_write_string(struct _frame_buffer *self, const char *s) {
    (void)self;
    (void)s;
}

static frame_buffer g_frame_buffer = {
    .write_string = dummy_write_string,
};

frame_buffer* get_active_frame_buffer(void) {
    return &g_frame_buffer;
}

void switch_active_frame_buffer(UINT32 index) {
    (void)index;
}

void enable_timer_irq(void) {
}

static UINT8 g_fake_fpu_default_state[512] __attribute__((aligned(16)));

const void *get_fpu_default_state(void) {
    return g_fake_fpu_default_state;
}

void os_repl_step(process_t *proc) {
    (void)proc;
}

void os_wait_for_more_input(process_t *proc) {
    (void)proc;
}

extern lisp_val_t lisp_ll_transpile_fixture_answer(lisp_val_t args, lisp_val_t env);

static void test_transpile_fixture_answer(void) {
    lisp_val_t result = lisp_ll_transpile_fixture_answer(0, 0);
    assert((result & TAG_MASK) == TAG_FIXNUM, "transpiled function returns a fixnum");
    assert((result >> 3) == 42, "transpiled function returns 42");
}

int main(void) {
    test_transpile_fixture_answer();
    return g_test_failed;
}
