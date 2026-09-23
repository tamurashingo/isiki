// マウントテーブル(*mounts*、Lisp側はsrc/lisp/mount.lisp)を使ったパス解決と、
// FATドライバ(src/lisp/fat32.lisp/fat16.lisp)への橋渡しを行う。

#include "mount.h"
#include "stream.h"
#include "runtime.h"
#include "process.h"
#include "eval.h"
#include "lisp.h"
#include "block_device.h"

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

/**
 * [P1] FATドライバ(Lisp)の呼び戻し結果が非局所脱出シグナルなら、out_transferへ
 * 預けて1を返す。
 *
 * **C→Lispの呼び戻しは、Cの境界で脱出を値に化けさせる。** 例えば
 * os_mount_fat_file_sizeは戻り値をos_fixnum_magnitudeへ通すので、脱出シグナルの
 * ヒープアドレスを「ファイルサイズ」として返してしまっていた
 * (documents/error-unwind-survey.md §A-3 / §F-6。このファイルには
 * os_apply_functionが14箇所あり、検査は1つも無かった)。
 *
 * 脱出は「失敗(戻り値0)」として扱う。呼び出し元は0が返ったときにout_transferを
 * 見て、非nilならそれをそのままLispへ返す。
 */
static int mount_transfer_escaped(lisp_val_t result, lisp_val_t *out_transfer) {
    if (!os_is_control_transfer(result)) {
        return 0;
    }
    if (out_transfer != 0) {
        *out_transfer = result;
    }
    return 1;
}

int os_mount_fat_read_file(mount_kind_t kind, lisp_val_t device, const char *relative_path,
                            UINT8 **out_data, UINT32 *out_len, lisp_val_t *out_transfer) {
    GC_PROTECT(device);

    lisp_val_t handle_fn = os_get_function(os_make_symbol("%DEVICE-HANDLE"), global_environment);
    if (handle_fn == nil) {
        return 0;
    }
    GC_PROTECT(handle_fn);
    lisp_val_t handle = os_apply_function(handle_fn, os_make_cons(device, nil), global_environment);
    GC_PROTECT(handle);
    if (mount_transfer_escaped(handle, out_transfer)) {
        return 0;
    }

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
    // **os_vector_headerへ通す前に見ること。** 脱出シグナルはTAG_INSTANCEなので、
    // そのまま通すと無関係なワードを「要素数」として読み、そのぶんループする
    if (mount_transfer_escaped(result, out_transfer)) {
        return 0;
    }

    if (result == nil) {
        // FAT32/FAT16のread-fileはファイル無し・空ファイルのいずれもnilを返すため
        // ここでは区別できない(既知の制約)
        return 0;
    }

    // [ファイルI/O]#46(M3): FAT32-READ-FILE/FAT16-READ-FILEの戻り値がconsリストから
    // general-vectorへ変更されたため(#41、コピーGCの生存ヒープサイズに比例した
    // コスト対策)、cc_car/cc_cdrによるリスト走査ではなくos_vector_header経由で
    // 直接データ部を読む(rank1のgeneral-vector前提、header[0]=rank(1)、
    // header[1]=要素数、header[2..]=データ)
    lisp_val_t *header = os_vector_header(result);
    UINT32 len = (UINT32)header[1];
    lisp_val_t *data = (lisp_val_t *)((lisp_addr_t)header + 16);

    UINT8 *buf = (UINT8 *)os_alloc_raw(len);
    for (UINT32 i = 0; i < len; i++) {
        buf[i] = (UINT8)os_fixnum_magnitude(data[i]);
    }

    *out_data = buf;
    *out_len = len;
    return 1;
}

int os_mount_fat_write_file(mount_kind_t kind, lisp_val_t device, const char *relative_path,
                             const UINT8 *data, UINT32 len, lisp_val_t *out_transfer) {
    GC_PROTECT(device);

    lisp_val_t handle_fn = os_get_function(os_make_symbol("%DEVICE-HANDLE"), global_environment);
    if (handle_fn == nil) {
        return 0;
    }
    GC_PROTECT(handle_fn);
    lisp_val_t handle = os_apply_function(handle_fn, os_make_cons(device, nil), global_environment);
    GC_PROTECT(handle);
    if (mount_transfer_escaped(handle, out_transfer)) {
        return 0;
    }

    // [ファイルI/O]#46(M3): FAT32-WRITE-FILE/FAT16-WRITE-FILE/*-CREATE-FILEが
    // 受け取るbytesの契約がconsリストからgeneral-vectorへ変更されたため、
    // 一旦consリストを組み立ててからos_make_vector_from_list(reader.cの
    // #(...)リテラル/組み込み関数VECTORと共通のコンストラクタ)でvectorへ
    // 変換する。この一時リストはmount.c境界だけで完結し、FAT層内部で保持され
    // 続けるわけではないため#41のような生存ヒープ肥大化の問題は生じない
    lisp_val_t bytes_list = nil;
    GC_PROTECT(bytes_list);
    for (UINT32 i = len; i > 0; i--) {
        bytes_list = os_make_cons(os_make_fixnum((UINT64)data[i - 1]), bytes_list);
    }
    lisp_val_t bytes = os_make_vector_from_list(bytes_list);
    GC_PROTECT(bytes);

    lisp_val_t path_str = os_make_string(relative_path);
    GC_PROTECT(path_str);

    const char *write_name = (kind == MOUNT_KIND_FAT32) ? "FAT32-WRITE-FILE" : "FAT16-WRITE-FILE";
    lisp_val_t write_fn = os_get_function(os_make_symbol(write_name), global_environment);
    if (write_fn != nil) {
        lisp_val_t write_args = os_make_cons(handle, os_make_cons(path_str, os_make_cons(bytes, nil)));
        GC_PROTECT(write_args);
        lisp_val_t result = os_apply_function(write_fn, write_args, global_environment);
        GC_PROTECT(result);
        // **脱出は「非nil」なので、検査しないと「書き込み成功」と誤判定する**
        if (mount_transfer_escaped(result, out_transfer)) {
            return 0;
        }
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
    GC_PROTECT(result2);
    if (mount_transfer_escaped(result2, out_transfer)) {
        return 0;
    }
    return result2 != nil;
}

int os_mount_fat_resolve_file_node(mount_kind_t kind, lisp_val_t device, const char *relative_path,
                                    int truncate, int create_if_missing, lisp_val_t *out_node,
                                    lisp_val_t *out_transfer) {
    GC_PROTECT(device);

    lisp_val_t handle_fn = os_get_function(os_make_symbol("%DEVICE-HANDLE"), global_environment);
    if (handle_fn == nil) {
        return 0;
    }
    GC_PROTECT(handle_fn);
    lisp_val_t handle = os_apply_function(handle_fn, os_make_cons(device, nil), global_environment);
    GC_PROTECT(handle);
    if (mount_transfer_escaped(handle, out_transfer)) {
        return 0;
    }

    const char *resolve_name = (kind == MOUNT_KIND_FAT32) ? "FAT32-RESOLVE-NODE" : "FAT16-RESOLVE-NODE";
    lisp_val_t resolve_fn = os_get_function(os_make_symbol(resolve_name), global_environment);
    if (resolve_fn == nil) {
        return 0;
    }
    GC_PROTECT(resolve_fn);

    lisp_val_t path_str = os_make_string(relative_path);
    GC_PROTECT(path_str);
    lisp_val_t resolve_args = os_make_cons(handle, os_make_cons(path_str, nil));
    GC_PROTECT(resolve_args);

    lisp_val_t node = os_apply_function(resolve_fn, resolve_args, global_environment);
    GC_PROTECT(node);
    // **脱出は非nilなので、検査しないと「解決できた」と誤判定して
    // out_nodeへ脱出シグナルを入れ、以後ストリームのmount_file_nodeになる**
    if (mount_transfer_escaped(node, out_transfer)) {
        return 0;
    }

    if (node != nil && truncate) {
        const char *write_name = (kind == MOUNT_KIND_FAT32) ? "FAT32-WRITE-FILE" : "FAT16-WRITE-FILE";
        lisp_val_t write_fn = os_get_function(os_make_symbol(write_name), global_environment);
        if (write_fn == nil) {
            return 0;
        }
        GC_PROTECT(write_fn);
        lisp_val_t empty_vec = os_make_vector_from_list(nil);
        GC_PROTECT(empty_vec);
        lisp_val_t write_args = os_make_cons(handle, os_make_cons(path_str, os_make_cons(empty_vec, nil)));
        GC_PROTECT(write_args);
        lisp_val_t truncated = os_apply_function(write_fn, write_args, global_environment);
        GC_PROTECT(truncated);
        if (mount_transfer_escaped(truncated, out_transfer)) {
            return 0;
        }
        if (truncated == nil) {
            return 0;
        }
        // 切り詰め後はdir-lba/start-cluster等が変わりうるため、nodeを解決し直す
        node = os_apply_function(resolve_fn, resolve_args, global_environment);
        GC_PROTECT(node);
        if (mount_transfer_escaped(node, out_transfer)) {
            return 0;
        }
    }

    if (node == nil && create_if_missing) {
        const char *create_name = (kind == MOUNT_KIND_FAT32) ? "FAT32-CREATE-FILE" : "FAT16-CREATE-FILE";
        lisp_val_t create_fn = os_get_function(os_make_symbol(create_name), global_environment);
        if (create_fn == nil) {
            return 0;
        }
        GC_PROTECT(create_fn);
        lisp_val_t empty_vec = os_make_vector_from_list(nil);
        GC_PROTECT(empty_vec);
        lisp_val_t create_args = os_make_cons(handle, os_make_cons(path_str, os_make_cons(empty_vec, nil)));
        GC_PROTECT(create_args);
        lisp_val_t created = os_apply_function(create_fn, create_args, global_environment);
        GC_PROTECT(created);
        if (mount_transfer_escaped(created, out_transfer)) {
            return 0;
        }
        if (created == nil) {
            return 0;
        }
        node = os_apply_function(resolve_fn, resolve_args, global_environment);
        GC_PROTECT(node);
        if (mount_transfer_escaped(node, out_transfer)) {
            return 0;
        }
    }

    if (node == nil) {
        return 0;
    }
    *out_node = node;
    return 1;
}

int os_mount_fat_file_size(mount_kind_t kind, lisp_val_t device, const char *relative_path,
                            UINT32 *out_len, lisp_val_t *out_transfer) {
    GC_PROTECT(device);

    lisp_val_t handle_fn = os_get_function(os_make_symbol("%DEVICE-HANDLE"), global_environment);
    if (handle_fn == nil) {
        return 0;
    }
    GC_PROTECT(handle_fn);
    lisp_val_t handle = os_apply_function(handle_fn, os_make_cons(device, nil), global_environment);
    GC_PROTECT(handle);
    if (mount_transfer_escaped(handle, out_transfer)) {
        return 0;
    }

    const char *size_name = (kind == MOUNT_KIND_FAT32) ? "FAT32-FILE-SIZE" : "FAT16-FILE-SIZE";
    lisp_val_t size_fn = os_get_function(os_make_symbol(size_name), global_environment);
    if (size_fn == nil) {
        return 0;
    }
    GC_PROTECT(size_fn);

    lisp_val_t path_str = os_make_string(relative_path);
    GC_PROTECT(path_str);
    lisp_val_t size_args = os_make_cons(handle, os_make_cons(path_str, nil));
    GC_PROTECT(size_args);
    lisp_val_t result = os_apply_function(size_fn, size_args, global_environment);
    GC_PROTECT(result);
    // **os_fixnum_magnitudeへ通す前に見ること。** 脱出シグナルのヒープアドレスを
    // ファイルサイズとして返してしまう
    if (mount_transfer_escaped(result, out_transfer)) {
        return 0;
    }

    if (result == nil) {
        return 0;
    }
    *out_len = (UINT32)os_fixnum_magnitude(result);
    return 1;
}

// ---------------------------------------------------------------------------
// [性能測定] READ-FILE-INTO-VECTOR-NATIVE(documents/performance-measurement.md
// 「read-file-into-vector-native」参照)
//
// read-file-into-vector(file-cmd.lisp)は、内部でopen-input-stream(FATパスなら
// os_mount_fat_read_file経由でファイル全体を一括でCバッファへ読み込み済み)の後、
// read-byte/set-eltを1byteずつ呼ぶAOTのwhileループでLisp vectorへ詰め直す。この
// 「詰め直し」1回ごとに、呼び出し規約・GC_PROTECT・primitive_*ディスパッチの
// オーバーヘッドが乗る。さらにos_mount_fat_read_file自体も、内部で呼ぶAOT
// fat16-read-file(fat16.lisp、%fat16-read-lba-list)がクラスタ→セクタの
// バイト列をelt/set-eltで1byteずつコピーしており、同種のオーバーヘッドが
// もう一段ある。
//
// この「Lisp呼び出し規約を完全に経由しない場合の理論的な下限(フロア)」を
// 計測するため、fat16.lispのBPBパース・ディレクトリエントリ走査・クラスタ
// チェイン追跡・セクタ読み込みのロジックを、Lisp呼び出し規約を一切経由しない
// 素のC関数として再実装する。IDEへのセクタ読み込みはblock_device_t::
// read_sectorsを直接呼び(1クラスタ分をまとめて1回のPIO転送にできる、
// read-sectorのLisp版が1セクタずつしか読めないのと異なる点)、読み込んだ
// バイト列は最終的なLisp vectorのデータ部へ直接os_make_fixnum(単なる左シフト、
// ヒープ確保を伴わない)で書き込む。
//
// スコープ: このベンチマーク用途に限定し、対象はFAT16のみ・ルート直下の
// ファイル(サブディレクトリ非対応)のみとする(実際の計測対象である
// tmp/perf_read_fat16.imgのREAD100K.BIN等がすべてルート直下にあるため)。
// 正規のfat16-read-file/read-file-into-vectorはサブディレクトリ・FAT32にも
// 対応済みであり、本関数はそれらを置き換えるものではなく、命令数計測実験
// 専用の別実装として追加する。

typedef struct {
    UINT16 bytes_per_sector;
    UINT8 sectors_per_cluster;
    UINT16 reserved_sectors;
    UINT8 num_fats;
    UINT16 root_entry_count;
    UINT16 sectors_per_fat;
} fat16n_bpb_t;

/** FAT16の1クラスタの最大サイズ(仕様上の上限、32KB)。これを超える構成は非対応としnilを返す */
#define FAT16N_MAX_CLUSTER_BYTES 32768

static UINT16 fat16n_u16(const UINT8 *b, UINT32 off) {
    return (UINT16)(b[off] | (b[off + 1] << 8));
}

static UINT32 fat16n_u32(const UINT8 *b, UINT32 off) {
    return (UINT32)(fat16n_u16(b, off) | ((UINT32)fat16n_u16(b, off + 2) << 16));
}

/**
 * deviceハンドル(%DEVICE-HANDLEの戻り値、生のblock_device_t*、またはIDE
 * パーティションハンドル(:ide-partition base-handle base-lba)のconsリスト)を
 * block_device_t*と、その上でのLBAに足すべきオフセットへ分解する
 * (read-sector/ide.lispの%ide-partition-handle-p相当)。
 */
static int fat16n_resolve_device(lisp_val_t handle, block_device_t **out_dev, UINT32 *out_base_lba) {
    /* [原則4] os_make_symbolは未internのシンボルに対しては確保する。
       ":IDE-PARTITION" は実際には他所で先にinternされているはずだが、それは
       初期化順に依存した前提で、順序が変わった瞬間に破れる。踏み抜くと
       直後の cc_cdr(handle) が旧From空間を読む。1行で閉じられるので閉じておく。 */
    GC_PROTECT(handle);
    if ((handle & TAG_MASK) == TAG_CONS) {
        lisp_val_t head = cc_car(handle);
        if (head != os_make_symbol(":IDE-PARTITION")) {
            return 0;
        }
        lisp_val_t rest = cc_cdr(handle);
        lisp_val_t base_handle = cc_car(rest);
        lisp_val_t base_lba_val = cc_car(cc_cdr(rest));
        if ((base_handle & TAG_MASK) != TAG_RAW_POINTER) {
            return 0;
        }
        *out_dev = (block_device_t *)(lisp_addr_t)(base_handle & ~TAG_MASK);
        *out_base_lba = (UINT32)os_fixnum_magnitude(base_lba_val);
        return 1;
    }
    if ((handle & TAG_MASK) != TAG_RAW_POINTER) {
        return 0;
    }
    *out_dev = (block_device_t *)(lisp_addr_t)(handle & ~TAG_MASK);
    *out_base_lba = 0;
    return 1;
}

static int fat16n_read_sectors(block_device_t *dev, UINT32 base_lba, UINT32 lba, UINT16 count, UINT8 *buf) {
    char err_msg[128];
    err_msg[0] = '\0';
    return dev->read_sectors(dev, base_lba + lba, count, buf, err_msg, sizeof(err_msg));
}

static int fat16n_read_bpb(block_device_t *dev, UINT32 base_lba, fat16n_bpb_t *out_bpb) {
    UINT8 sector[512];
    if (!fat16n_read_sectors(dev, base_lba, 0, 1, sector)) {
        return 0;
    }
    // total-sectors自体はクラスタ位置計算に不要なため読み取らない
    out_bpb->bytes_per_sector = fat16n_u16(sector, 11);
    out_bpb->sectors_per_cluster = sector[13];
    out_bpb->reserved_sectors = fat16n_u16(sector, 14);
    out_bpb->num_fats = sector[16];
    out_bpb->root_entry_count = fat16n_u16(sector, 17);
    out_bpb->sectors_per_fat = fat16n_u16(sector, 22);
    if (out_bpb->bytes_per_sector != 512) {
        // 本関数の固定512byteスタックバッファ前提が崩れるため非対応
        return 0;
    }
    return 1;
}

static UINT32 fat16n_root_dir_lba(const fat16n_bpb_t *bpb) {
    return bpb->reserved_sectors + (UINT32)bpb->num_fats * bpb->sectors_per_fat;
}

static UINT32 fat16n_root_dir_sector_count(const fat16n_bpb_t *bpb) {
    return ((UINT32)bpb->root_entry_count * 32) / bpb->bytes_per_sector;
}

static UINT32 fat16n_data_start_lba(const fat16n_bpb_t *bpb) {
    return fat16n_root_dir_lba(bpb) + fat16n_root_dir_sector_count(bpb);
}

static UINT32 fat16n_cluster_to_lba(const fat16n_bpb_t *bpb, UINT32 cluster_no) {
    return fat16n_data_start_lba(bpb) + (cluster_no - 2) * bpb->sectors_per_cluster;
}

/** cluster-noに対応するFATエントリ(16bit)を返す。読み込み失敗時は0xFFFF(終端扱い) */
static UINT16 fat16n_fat_entry(block_device_t *dev, UINT32 base_lba, const fat16n_bpb_t *bpb, UINT32 cluster_no) {
    UINT32 byte_offset = cluster_no * 2;
    UINT32 sector_offset = byte_offset / bpb->bytes_per_sector;
    UINT32 offset_in_sector = byte_offset % bpb->bytes_per_sector;
    UINT8 sector[512];
    if (!fat16n_read_sectors(dev, base_lba, bpb->reserved_sectors + sector_offset, 1, sector)) {
        return 0xFFFF;
    }
    return fat16n_u16(sector, offset_in_sector);
}

/**
 * ルートディレクトリを先頭から走査し、8.3名(拡張子ドット込み、大文字)が
 * nameと一致するエントリのstart-cluster/sizeを取り出す。見つからなければ0を返す。
 * name中に'/'が含まれる(サブディレクトリを含むパス)場合は非対応として0を返す。
 */
static int fat16n_find_root_entry(block_device_t *dev, UINT32 base_lba, const fat16n_bpb_t *bpb,
                                   const char *name, UINT32 *out_start_cluster, UINT32 *out_size) {
    UINT32 root_lba = fat16n_root_dir_lba(bpb);
    UINT32 root_sectors = fat16n_root_dir_sector_count(bpb);
    char entry_name[13];

    for (UINT32 s = 0; s < root_sectors; s++) {
        UINT8 sector[512];
        if (!fat16n_read_sectors(dev, base_lba, root_lba + s, 1, sector)) {
            return 0;
        }
        for (UINT32 off = 0; off < 512; off += 32) {
            UINT8 first = sector[off];
            if (first == 0x00) {
                return 0; // ルートディレクトリ終端
            }
            if (first == 0xE5) {
                continue; // 削除済みエントリ
            }
            // 8.3名を"NAME.EXT"(拡張子が空ならNAMEのみ)へrtrim+結合する
            UINT32 p = 0;
            UINT32 name_end = 8;
            while (name_end > 0 && sector[off + name_end - 1] == ' ') name_end--;
            for (UINT32 i = 0; i < name_end; i++) entry_name[p++] = (char)sector[off + i];
            UINT32 ext_end = 3;
            while (ext_end > 0 && sector[off + 8 + ext_end - 1] == ' ') ext_end--;
            if (ext_end > 0) {
                entry_name[p++] = '.';
                for (UINT32 i = 0; i < ext_end; i++) entry_name[p++] = (char)sector[off + 8 + i];
            }
            entry_name[p] = '\0';

            // name(呼び出し元が渡す相対パス、大文字の8.3名)とentry_nameの完全一致を
            // 判定する(%fat16-find-dir-entryのstring=と同じ、大文字小文字は
            // 変換しない厳密比較)
            int matches = 1;
            UINT32 j = 0;
            for (; entry_name[j] != '\0' && name[j] != '\0'; j++) {
                if (entry_name[j] != name[j]) {
                    matches = 0;
                    break;
                }
            }
            if (matches && (entry_name[j] != '\0' || name[j] != '\0')) {
                matches = 0;
            }
            if (matches) {
                *out_start_cluster = fat16n_u16(sector, off + 26);
                *out_size = fat16n_u32(sector, off + 28);
                return 1;
            }
        }
    }
    return 0;
}

/**
 * (read-file-into-vector-native path) : read-file-into-vectorと同じ契約
 * (成功時general-vector、マウント解決失敗/非対応時nil)だが、Lisp呼び出し
 * 規約を一切経由しない素のC実装で読み込む(上のコメント参照)。FAT16の
 * ルート直下のファイルのみ対応。
 */
lisp_val_t cc_read_file_into_vector_native(lisp_val_t args, lisp_val_t env) {
    (void)env;
    char path[STREAM_PATH_MAX];
    os_string_to_cstr(cc_car(args), path, sizeof(path));

    char relative[STREAM_PATH_MAX];
    lisp_val_t device_sym;
    mount_kind_t kind = os_mount_resolve(path, relative, sizeof(relative), &device_sym);
    if (kind != MOUNT_KIND_FAT16) {
        return nil;
    }
    // ルート直下限定(サブディレクトリパス"/DIR/FILE.TXT"は非対応)
    if (relative[0] != '/') {
        return nil;
    }
    for (UINT32 i = 1; relative[i] != '\0'; i++) {
        if (relative[i] == '/') {
            return nil;
        }
    }

    GC_PROTECT(device_sym);
    lisp_val_t handle_fn = os_get_function(os_make_symbol("%DEVICE-HANDLE"), global_environment);
    if (handle_fn == nil) {
        return nil;
    }
    GC_PROTECT(handle_fn);
    lisp_val_t handle = os_apply_function(handle_fn, os_make_cons(device_sym, nil), global_environment);
    // %DEVICE-HANDLEが非局所脱出した場合はそのまま返す(この関数はlisp_val_tを
    // 返すので、預ける先は要らない)
    if (os_is_control_transfer(handle)) {
        return handle;
    }
    // [性能測定] handle自体はGCに再配置されうるが、ここから先(fat16n_*)は
    // 生のblock_device_t*しか使わない(handleを再度参照しない)ため、
    // これ以降GC_PROTECTは不要
    block_device_t *dev;
    UINT32 base_lba;
    if (!fat16n_resolve_device(handle, &dev, &base_lba)) {
        return nil;
    }

    fat16n_bpb_t bpb;
    if (!fat16n_read_bpb(dev, base_lba, &bpb)) {
        return nil;
    }

    UINT32 start_cluster, size;
    if (!fat16n_find_root_entry(dev, base_lba, &bpb, relative + 1, &start_cluster, &size)) {
        return nil;
    }
    if (size == 0) {
        // read-file-into-vector(file-cmd.lisp)はos_mount_fat_read_file経由で
        // 0byteファイルをnilとして扱う(fat16-read-fileの既知の制約、
        // 「ファイル無し」と区別できない)。比較対象と挙動を一致させるため
        // ここでも同じ契約(0byteファイルはnil)に合わせる
        return nil;
    }

    lisp_val_t *data;
    lisp_val_t vec = os_make_vector_raw(size, &data);
    // [性能測定] os_make_vector_raw確保後はvecがGCルートから到達可能な正規の
    // VECTORなので、以降の(FATエントリ読み込み等の)処理でGCが発火しても
    // vec/dataの安全性に問題は無い(VECTORの内部ブロックはgc_relocateが
    // 正しく再配置する、既存のVECTOR実装と同じ)
    GC_PROTECT(vec);

    UINT32 cluster_bytes = (UINT32)bpb.sectors_per_cluster * bpb.bytes_per_sector;
    if (cluster_bytes == 0 || cluster_bytes > FAT16N_MAX_CLUSTER_BYTES) {
        return nil;
    }
    UINT8 cluster_buf[FAT16N_MAX_CLUSTER_BYTES];

    UINT32 cluster = start_cluster;
    UINT32 written = 0;
    UINT32 safety_limit = (size + cluster_bytes - 1) / cluster_bytes + 2; // 想定クラスタ数+安全マージン
    for (UINT32 iter = 0; iter < safety_limit && written < size; iter++) {
        if (!fat16n_read_sectors(dev, base_lba, fat16n_cluster_to_lba(&bpb, cluster),
                                  bpb.sectors_per_cluster, cluster_buf)) {
            return nil;
        }
        UINT32 chunk = cluster_bytes;
        if (written + chunk > size) {
            chunk = size - written;
        }
        for (UINT32 i = 0; i < chunk; i++) {
            data[written + i] = os_make_fixnum(cluster_buf[i]);
        }
        written += chunk;
        if (written >= size) {
            break;
        }
        UINT16 next = fat16n_fat_entry(dev, base_lba, &bpb, cluster);
        if (next >= 0xFFF8) {
            return nil; // チェインがsizeに満たないまま終端(破損)
        }
        cluster = next;
    }
    if (written < size) {
        return nil;
    }
    return vec;
}

void os_register_mount_native_subprimitives(void) {
    os_set_function(os_make_symbol("READ-FILE-INTO-VECTOR-NATIVE"),
                     os_make_native_function((lisp_addr_t)(void *)cc_read_file_into_vector_native), global_environment);
}
