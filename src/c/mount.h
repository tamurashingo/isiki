#ifndef _MOUNT_H_
#define _MOUNT_H_

#include "types.h"

/** パスの解決結果種別 */
typedef enum {
    MOUNT_KIND_NONE,
    MOUNT_KIND_9P,
    MOUNT_KIND_FAT32,
    MOUNT_KIND_FAT16
} mount_kind_t;

/**
 * pathを*mounts*(Lisp側の動的変数、未load環境ではnil=空とみなす)と、組み込みの
 * "/9p"ルールに対して解決する。"/9p"配下は*mounts*に登録が無くても常に解決できる
 * (9Pドライバでアクセスするファイルは/9p配下限定、という仕様)。
 * 最長一致(prefix境界を厳密にチェック、マウントパス"/"のみ常に一致)したエントリを
 * 採用し、一致した分を取り除いた相対パスをout_relativeへ書き出す。マウントパスが
 * "/"自身の場合は元のpathをそのままout_relativeへコピーする(例:
 * "/path/to/file.txt"を"/"にマウントした場合の相対パスは"/path/to/file.txt"のまま)。
 * @param path 解決対象の絶対パス(NUL終端)
 * @param out_relative 相対パスの格納先
 * @param relative_cap out_relativeの容量(NUL終端分を含む)
 * @param out_device 一致したエントリのdeviceシンボル(9P/未一致の場合はnil)
 * @return 一致した種別。一致しなければMOUNT_KIND_NONE
 */
mount_kind_t os_mount_resolve(const char *path, char *out_relative, UINT32 relative_cap,
                               lisp_val_t *out_device);

/**
 * (%device-handle device)でdeviceのハンドルを取り、(fat32-read-file|fat16-read-file
 * handle relative_path)を呼び出して結果(fixnumのconsリスト、またはファイルが
 * 無ければnil)をbyteバッファへ変換する。バッファはos_alloc_rawで確保する
 * (呼び出し元は解放不要、ヒープはGC管理)。
 * @param kind MOUNT_KIND_FAT32またはMOUNT_KIND_FAT16
 * @param device deviceシンボル
 * @param relative_path FATドライバへ渡す相対パス(NUL終端、例: "/DIR/FILE.TXT")
 * @param out_data 確保したバッファの格納先
 * @param out_len バッファのバイト数の格納先
 * @return 成功時1、ファイルが無い/読み込み失敗時0
 */
int os_mount_fat_read_file(mount_kind_t kind, lisp_val_t device, const char *relative_path,
                            UINT8 **out_data, UINT32 *out_len);

/**
 * data(len byte)をfixnumのconsリストに変換し、(fat32-write-file|fat16-write-file
 * handle relative_path bytes)を試み、既存ファイルが無く失敗した場合は
 * (fat32-create-file|fat16-create-file handle relative_path bytes)にフォールバック
 * する(9Pの「open失敗→create」と同じ二段構え)。
 * @param kind MOUNT_KIND_FAT32またはMOUNT_KIND_FAT16
 * @param device deviceシンボル
 * @param relative_path FATドライバへ渡す相対パス
 * @param data 書き込むバイト列
 * @param len dataのバイト数
 * @return 成功時1、失敗時0
 */
int os_mount_fat_write_file(mount_kind_t kind, lisp_val_t device, const char *relative_path,
                             const UINT8 *data, UINT32 len);

/**
 * pathを解決し、(fat32-resolve-node|fat16-resolve-node handle relative_path)で
 * 得られる<file-node>インスタンス(<fat32-file-node>/<fat16-file-node>、
 * read-into!/write-from!のディスパッチ先)を返す。[ファイルI/O]#49(M6)、
 * os_stream_tのFAT系ストリームがos_mount_fat_read_file/os_mount_fat_write_file
 * (ファイル全体の一括読み書き)ではなくこのnodeを保持し、read-into!/write-from!
 * 経由でオンデマンドにrefill/flushするために使う。
 * truncateが真の場合、既存ファイルは(fat32-write-file|fat16-write-file)へ
 * 空vectorを渡して0byteへ切り詰めてから解決し直す(OPEN-OUTPUT-FILE相当、
 * 9PのP9_OTRUNCと同じ意味論)。truncateが偽の場合は既存の内容をそのまま保持する
 * (OPEN-IO-FILE用。旧実装が持っていた「IOストリームは常に空バッファから始まり
 * 書き込み前のreadが常にEOFになる」既知の非対称性は、この分岐により解消される)。
 * create_if_missingが真でファイルが存在しない場合は
 * (fat32-create-file|fat16-create-file)で空ファイルを新規作成してから解決する。
 * @param kind MOUNT_KIND_FAT32またはMOUNT_KIND_FAT16
 * @param device deviceシンボル
 * @param relative_path FATドライバへ渡す相対パス
 * @param truncate 真なら既存ファイルを0byteへ切り詰めてから解決する
 * @param create_if_missing 真ならファイルが存在しない場合に空ファイルを新規作成する
 * @param out_node 解決できた場合に<file-node>インスタンスを格納する先
 * @return 成功時1、失敗時0(パス解決不可・作成失敗等)
 */
int os_mount_fat_resolve_file_node(mount_kind_t kind, lisp_val_t device, const char *relative_path,
                                    int truncate, int create_if_missing, lisp_val_t *out_node);

#endif /* _MOUNT_H_ */
