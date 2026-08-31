#ifndef _BLOCK_DEVICE_H_
#define _BLOCK_DEVICE_H_

#include "types.h"

typedef struct _block_device block_device_t;

/**
 * ストレージを「blockの集合」として扱う抽象インタフェース。IDE/ATA以外の
 * ストレージ(NVMe/AHCI等)を追加する際もこの構造体を実装すればよい。
 * ファイルシステムは持たない(生セクタの読み書きのみ)。
 */
struct _block_device {
    /** デバイス名(デバッグ表示用) */
    const char *name;
    /** 1セクタのバイト数 */
    UINT32 sector_size;
    /** 総セクタ数(0の場合は未初期化またはサイズ不明) */
    UINT32 total_sectors;
    /** IDENTIFY等で得たASCIIモデル名(末尾空白除去済み、NUL終端、未対応時は空文字列) */
    char model[41];
    /** デバイスの初期化/検出を行う */
    int (*init)(block_device_t *self, char *err_msg, UINT32 err_msg_cap);
    /** lbaを起点にcount個のセクタをbufへ読み込む(bufはcount*sector_size byte以上) */
    int (*read_sectors)(block_device_t *self, UINT32 lba, UINT16 count, UINT8 *buf, char *err_msg, UINT32 err_msg_cap);
    /** lbaを起点にbufの内容をcount個のセクタへ書き込む */
    int (*write_sectors)(block_device_t *self, UINT32 lba, UINT16 count, const UINT8 *buf, char *err_msg, UINT32 err_msg_cap);
    /** 書き込みをメディアへ確定させる(キャッシュフラッシュ等) */
    int (*flush)(block_device_t *self, char *err_msg, UINT32 err_msg_cap);
    /** ドライバ固有の状態(os_ide_device*等) */
    void *priv_data;
};

/**
 * 全チャネルをプローブしてblock_device_tのレジストリを構築する。GCヒープが
 * まだ存在しない起動直後、os_boot_alloc_init後・os_boot_alloc_finalize前に
 * 1回だけ呼ぶこと(検出した各デバイスの実体はos_boot_allocで確保する)。
 */
void os_block_device_probe_all(void);

/** os_block_device_probe_allで検出されたデバイス数を返す */
UINT32 os_block_device_count(void);

/**
 * 検出順(0起点)でindex番目のblock_device_t*を返す。範囲外の場合は0。
 */
block_device_t *os_block_device_at(UINT32 index);

/**
 * read_sectors/write_sectorsが読み書きに使う共有セクタバッファの先頭アドレスを
 * 返す(GCヒープ外の静的配列)。read_sectors/write_sectorsを呼ぶ前後でLisp側が
 * このアドレスを介してバイト単位に%%peek/%%pokeする想定。
 */
UINT8 *os_block_device_ide_sector_buffer(void);

#endif /* _BLOCK_DEVICE_H_ */
