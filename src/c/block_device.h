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
 * Secondary IDEチャネルのblock_device_tシングルトンを返す。初回呼び出し時のみ
 * initを実行してIDENTIFYをプローブし、以後は結果をキャッシュして返す
 * (transport_virtio9pのensure_readyパターンと同じ)。
 * @return デバイス検出に成功していればblock_device_t*、失敗していれば0
 */
block_device_t *os_block_device_ide_instance(void);

/**
 * os_block_device_ide_instanceが読み書きに使う共有セクタバッファの先頭アドレスを
 * 返す(GCヒープ外の静的配列)。read_sectors/write_sectorsを呼ぶ前後でLisp側が
 * このアドレスを介してバイト単位に%%peek/%%pokeする想定。
 */
UINT8 *os_block_device_ide_sector_buffer(void);

#endif /* _BLOCK_DEVICE_H_ */
