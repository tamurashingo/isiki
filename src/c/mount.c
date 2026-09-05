// マウントテーブル(*mounts*、Lisp側はsrc/lisp/mount.lisp)を使ったパス解決と、
// FATドライバ(src/lisp/fat32.lisp/fat16.lisp)への橋渡しを行う。

#include "mount.h"
#include "stream.h"
#include "runtime.h"
#include "process.h"
#include "eval.h"
#include "lisp.h"

/** ホスト9P経由のファイルアクセスの組み込みマウントパス。*mounts*に登録が無くても
    常にこのプレフィックスを解決できる(9Pドライバでアクセスするファイルは
    /9p配下限定、という仕様) */
static const char *MOUNT_9P_PREFIX = "/9p";

/**
 * mount_pathがpathの接頭辞として正しく一致するかを調べる。一致した直後がpathの
 * 終端か'/'であることを要求する(例: マウントパス"/mnt"は"/mnt2/x"には一致しない)。
 * ただしmount_pathが"/"自身の場合は常に一致する。
 * @param mount_path 判定するマウントパス(NUL終端)
 * @param path 解決対象のパス(NUL終端)
 * @return 一致すればmount_pathの文字数、一致しなければ-1
 */
static int mount_path_match_len(const char *mount_path, const char *path) {
    int i = 0;
    while (mount_path[i] != '\0') {
        if (path[i] != mount_path[i]) {
            return -1;
        }
        i++;
    }
    if (i == 1 && mount_path[0] == '/') {
        return i;
    }
    if (path[i] == '\0' || path[i] == '/') {
        return i;
    }
    return -1;
}

mount_kind_t os_mount_resolve(const char *path, char *out_relative, UINT32 relative_cap,
                               lisp_val_t *out_device) {
    lisp_val_t sym_fat32 = os_make_symbol(":FAT32");
    GC_PROTECT(sym_fat32);
    lisp_val_t sym_fat16 = os_make_symbol(":FAT16");
    GC_PROTECT(sym_fat16);

    lisp_val_t mounts = os_get_dynamic(os_make_symbol("*MOUNTS*"));
    GC_PROTECT(mounts);
    lisp_val_t current = mounts;
    GC_PROTECT(current);

    int best_len = -1;
    int best_is_root = 0;
    mount_kind_t best_kind = MOUNT_KIND_NONE;
    lisp_val_t best_device = nil;
    GC_PROTECT(best_device);

    while (current != nil) {
        lisp_val_t entry = cc_car(current); /* (mount-path . (device . fs-type)) */
        lisp_val_t mount_path_val = cc_car(entry);
        lisp_val_t value = cc_cdr(entry);
        lisp_val_t device = cc_car(value);
        lisp_val_t fs_type = cc_cdr(value);

        mount_kind_t kind = MOUNT_KIND_NONE;
        if (fs_type == sym_fat32) {
            kind = MOUNT_KIND_FAT32;
        } else if (fs_type == sym_fat16) {
            kind = MOUNT_KIND_FAT16;
        }

        if (kind != MOUNT_KIND_NONE) {
            char mount_path_cbuf[STREAM_PATH_MAX];
            os_string_to_cstr(mount_path_val, mount_path_cbuf, sizeof(mount_path_cbuf));
            int m = mount_path_match_len(mount_path_cbuf, path);
            if (m > best_len) {
                best_len = m;
                best_is_root = (mount_path_cbuf[0] == '/' && mount_path_cbuf[1] == '\0');
                best_kind = kind;
                best_device = device;
            }
        }

        current = cc_cdr(current);
    }

    int p9_len = mount_path_match_len(MOUNT_9P_PREFIX, path);
    if (p9_len > best_len) {
        best_len = p9_len;
        best_is_root = 0;
        best_kind = MOUNT_KIND_9P;
        best_device = nil;
    }

    if (best_kind == MOUNT_KIND_NONE) {
        return MOUNT_KIND_NONE;
    }

    const char *src = best_is_root ? path : path + best_len;
    UINT32 i = 0;
    while (src[i] != '\0' && i + 1 < relative_cap) {
        out_relative[i] = src[i];
        i++;
    }
    out_relative[i] = '\0';

    *out_device = best_device;
    return best_kind;
}

int os_mount_fat_read_file(mount_kind_t kind, lisp_val_t device, const char *relative_path,
                            UINT8 **out_data, UINT32 *out_len) {
    GC_PROTECT(device);

    lisp_val_t handle_fn = os_get_function(os_make_symbol("%DEVICE-HANDLE"), global_environment);
    if (handle_fn == nil) {
        return 0;
    }
    GC_PROTECT(handle_fn);
    lisp_val_t handle = os_apply_function(handle_fn, os_make_cons(device, nil), global_environment);
    GC_PROTECT(handle);

    const char *read_name = (kind == MOUNT_KIND_FAT32) ? "FAT32-READ-FILE" : "FAT16-READ-FILE";
    lisp_val_t read_fn = os_get_function(os_make_symbol(read_name), global_environment);
    if (read_fn == nil) {
        return 0;
    }
    GC_PROTECT(read_fn);

    lisp_val_t path_str = os_make_string(relative_path);
    GC_PROTECT(path_str);
    lisp_val_t read_args = os_make_cons(handle, os_make_cons(path_str, nil));
    GC_PROTECT(read_args);
    lisp_val_t result = os_apply_function(read_fn, read_args, global_environment);
    GC_PROTECT(result);

    if (result == nil) {
        // FAT32/FAT16のread-fileはファイル無し・空ファイルのいずれもnilを返すため
        // ここでは区別できない(既知の制約)
        return 0;
    }

    UINT32 len = 0;
    lisp_val_t cursor = result;
    GC_PROTECT(cursor);
    while (cursor != nil) {
        len++;
        cursor = cc_cdr(cursor);
    }

    UINT8 *buf = (UINT8 *)os_alloc_raw(len);
    cursor = result;
    for (UINT32 i = 0; i < len; i++) {
        buf[i] = (UINT8)os_fixnum_magnitude(cc_car(cursor));
        cursor = cc_cdr(cursor);
    }

    *out_data = buf;
    *out_len = len;
    return 1;
}

int os_mount_fat_write_file(mount_kind_t kind, lisp_val_t device, const char *relative_path,
                             const UINT8 *data, UINT32 len) {
    GC_PROTECT(device);

    lisp_val_t handle_fn = os_get_function(os_make_symbol("%DEVICE-HANDLE"), global_environment);
    if (handle_fn == nil) {
        return 0;
    }
    GC_PROTECT(handle_fn);
    lisp_val_t handle = os_apply_function(handle_fn, os_make_cons(device, nil), global_environment);
    GC_PROTECT(handle);

    lisp_val_t bytes = nil;
    GC_PROTECT(bytes);
    for (UINT32 i = len; i > 0; i--) {
        bytes = os_make_cons(os_make_fixnum((UINT64)data[i - 1]), bytes);
    }

    lisp_val_t path_str = os_make_string(relative_path);
    GC_PROTECT(path_str);

    const char *write_name = (kind == MOUNT_KIND_FAT32) ? "FAT32-WRITE-FILE" : "FAT16-WRITE-FILE";
    lisp_val_t write_fn = os_get_function(os_make_symbol(write_name), global_environment);
    if (write_fn != nil) {
        lisp_val_t write_args = os_make_cons(handle, os_make_cons(path_str, os_make_cons(bytes, nil)));
        GC_PROTECT(write_args);
        lisp_val_t result = os_apply_function(write_fn, write_args, global_environment);
        if (result != nil) {
            return 1;
        }
    }

    const char *create_name = (kind == MOUNT_KIND_FAT32) ? "FAT32-CREATE-FILE" : "FAT16-CREATE-FILE";
    lisp_val_t create_fn = os_get_function(os_make_symbol(create_name), global_environment);
    if (create_fn == nil) {
        return 0;
    }
    lisp_val_t create_args = os_make_cons(handle, os_make_cons(path_str, os_make_cons(bytes, nil)));
    GC_PROTECT(create_args);
    lisp_val_t result2 = os_apply_function(create_fn, create_args, global_environment);
    return result2 != nil;
}
