#include <stdlib.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "lisp.h"
#include "process.h"
#include "mount.h"
#include "stream.h"

// reader.c/stream.cをruntime_test.c等と同じ理由でリンクする(os_get_dynamic経由の
// *MOUNTS*操作にruntime.cのdynamic変数機構が必要で、それを単体でリンクする既存の
// 組み合わせがreader.c+stream.cを含むもののため)。os_virtio9p_*/フレームバッファ/
// プロセススケジューラ関連はこのテストが使わないダミー実装で十分
int os_virtio9p_open(const char *path, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path; (void)mode; (void)out_fid; (void)err_msg; (void)err_msg_cap;
    return 0;
}

int os_virtio9p_create(const char *path, UINT32 perm, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path; (void)perm; (void)mode; (void)out_fid; (void)err_msg; (void)err_msg_cap;
    return 0;
}

int os_virtio9p_write_chunk(UINT32 fid, UINT64 offset, const UINT8 *data, UINT32 count,
                             UINT32 *out_written, char *err_msg, UINT32 err_msg_cap) {
    (void)fid; (void)offset; (void)data; (void)count; (void)out_written; (void)err_msg; (void)err_msg_cap;
    return 0;
}

int os_virtio9p_read_chunk(UINT32 fid, UINT64 offset, UINT32 want,
                            const UINT8 **out_data, UINT32 *out_count,
                            char *err_msg, UINT32 err_msg_cap) {
    (void)fid; (void)offset; (void)want; (void)out_data; (void)out_count; (void)err_msg; (void)err_msg_cap;
    return 0;
}

int os_virtio9p_close(UINT32 fid, char *err_msg, UINT32 err_msg_cap) {
    (void)fid; (void)err_msg; (void)err_msg_cap;
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

#define HEAP_SIZE (1024 * 1024)

// os_make_cons/symbol/string等はヒープ確保とnilの初期化が前提なので、
// 各テスト実行前にheap_initとbootを済ませておく(他のテストと同じパターン)
static void setup_heap(void) {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
}

/** *MOUNTS*へ(path device fs-type)のエントリを1件追加する */
static void add_mount(const char *path, const char *device_name, const char *fs_type_name) {
    lisp_val_t path_val = os_make_string(path);
    lisp_val_t device = os_make_symbol(device_name);
    lisp_val_t fs_type = os_make_symbol(fs_type_name);
    lisp_val_t entry = os_make_cons(path_val, os_make_cons(device, fs_type));
    lisp_val_t sym = os_make_symbol("*MOUNTS*");
    lisp_val_t existing = os_get_dynamic(sym);
    os_set_dynamic(sym, os_make_cons(entry, existing));
}

static void reset_mounts(void) {
    os_set_dynamic(os_make_symbol("*MOUNTS*"), nil);
}

void test_resolve_9p_default_without_any_mounts(void) {
    reset_mounts();
    char relative[STREAM_PATH_MAX];
    lisp_val_t device;
    mount_kind_t kind = os_mount_resolve("/9p/foo/bar.txt", relative, sizeof(relative), &device);
    assert(kind == MOUNT_KIND_9P, "*mounts*が空でも/9p配下は組み込みで解決できる");
    // "/9p/foo/bar.txt"から"/9p"を取り除いた"/foo/bar.txt"になっているはず
    int ok = relative[0] == '/' && relative[1] == 'f' && relative[2] == 'o' && relative[3] == 'o' && relative[4] == '/';
    assert(ok, "相対パスは/9pを取り除いた残り('/foo/bar.txt')");
}

void test_resolve_non_9p_path_without_mounts_is_none(void) {
    reset_mounts();
    char relative[STREAM_PATH_MAX];
    lisp_val_t device;
    mount_kind_t kind = os_mount_resolve("/file.txt", relative, sizeof(relative), &device);
    assert(kind == MOUNT_KIND_NONE, "*mounts*が空で/9p配下でもないパスは解決できない");
}

void test_resolve_root_mount_keeps_full_path_as_relative(void) {
    reset_mounts();
    add_mount("/", "BLK0S0", ":FAT32");
    char relative[STREAM_PATH_MAX];
    lisp_val_t device;
    mount_kind_t kind = os_mount_resolve("/path/to/file.txt", relative, sizeof(relative), &device);
    assert(kind == MOUNT_KIND_FAT32, "'/'にmountしたfs-typeがFAT32として解決される");
    assert(device == os_make_symbol("BLK0S0"), "resolveしたdeviceシンボルが一致する");
    int ok = relative[0] == '/' && relative[1] == 'p' && relative[2] == 'a' && relative[3] == 't' && relative[4] == 'h';
    assert(ok, "'/'マウント時の相対パスは元のパスそのまま(先頭の'/'を保持)");
}

void test_resolve_non_root_mount_strips_prefix(void) {
    reset_mounts();
    add_mount("/mnt", "BLK1", ":FAT16");
    char relative[STREAM_PATH_MAX];
    lisp_val_t device;
    mount_kind_t kind = os_mount_resolve("/mnt/file.txt", relative, sizeof(relative), &device);
    assert(kind == MOUNT_KIND_FAT16, "'/mnt'にmountしたfs-typeがFAT16として解決される");
    int ok = relative[0] == '/' && relative[1] == 'f' && relative[2] == 'i' && relative[3] == 'l' && relative[4] == 'e';
    assert(ok, "'/mnt'マウント時の相対パスはプレフィックスを取り除いた残り('/file.txt')");
}

void test_resolve_prefix_boundary_does_not_match_similar_name(void) {
    reset_mounts();
    add_mount("/mnt", "BLK1", ":FAT16");
    char relative[STREAM_PATH_MAX];
    lisp_val_t device;
    mount_kind_t kind = os_mount_resolve("/mnt2/x", relative, sizeof(relative), &device);
    assert(kind == MOUNT_KIND_NONE, "'/mnt'は'/mnt2/x'にはマッチしない(境界チェック)");
}

void test_resolve_prefers_longest_match(void) {
    reset_mounts();
    add_mount("/", "BLK0S0", ":FAT32");
    add_mount("/mnt", "BLK1", ":FAT16");
    char relative[STREAM_PATH_MAX];
    lisp_val_t device;

    mount_kind_t kind_mnt = os_mount_resolve("/mnt/file.txt", relative, sizeof(relative), &device);
    assert(kind_mnt == MOUNT_KIND_FAT16, "'/'と'/mnt'両方登録時、'/mnt/file.txt'はより長い'/mnt'にマッチする");

    mount_kind_t kind_root = os_mount_resolve("/other/file.txt", relative, sizeof(relative), &device);
    assert(kind_root == MOUNT_KIND_FAT32, "'/mnt'にマッチしないパスは'/'にフォールバックする");
}

void test_resolve_ties_prefer_mounts_over_builtin_9p(void) {
    reset_mounts();
    add_mount("/9p", "BLK0S0", ":FAT32");
    char relative[STREAM_PATH_MAX];
    lisp_val_t device;
    mount_kind_t kind = os_mount_resolve("/9p/file.txt", relative, sizeof(relative), &device);
    assert(kind == MOUNT_KIND_FAT32, "同じ長さの一致では*mounts*側(明示的な/9p mount)が組み込みルールより優先される");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    setup_heap();
    test_resolve_9p_default_without_any_mounts();
    test_resolve_non_9p_path_without_mounts_is_none();
    test_resolve_root_mount_keeps_full_path_as_relative();
    test_resolve_non_root_mount_strips_prefix();
    test_resolve_prefix_boundary_does_not_match_similar_name();
    test_resolve_prefers_longest_match();
    test_resolve_ties_prefer_mounts_over_builtin_9p();
    return g_test_failed ? 1 : 0;
}
