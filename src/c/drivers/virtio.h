#ifndef _DRIVERS_VIRTIO_H_
#define _DRIVERS_VIRTIO_H_

#include "../types.h"

/** legacy virtio-pci BAR0レジスタのオフセット */
#define VIRTIO_REG_HOST_FEATURES   0x00 /* R32 */
#define VIRTIO_REG_GUEST_FEATURES  0x04 /* W32 */
#define VIRTIO_REG_QUEUE_ADDRESS   0x08 /* W32, 書き込む値はアドレス>>12 (PFN) */
#define VIRTIO_REG_QUEUE_SIZE      0x0C /* R16, デバイス側が決める読み取り専用値 */
#define VIRTIO_REG_QUEUE_SELECT    0x0E /* W16 */
#define VIRTIO_REG_QUEUE_NOTIFY    0x10 /* W16 */
#define VIRTIO_REG_DEVICE_STATUS   0x12 /* RW8 */
#define VIRTIO_REG_ISR_STATUS      0x13 /* R8 */

/** デバイスステータスビット(legacy) */
#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER      0x02
#define VIRTIO_STATUS_DRIVER_OK   0x04
#define VIRTIO_STATUS_FAILED      0x80

/** legacy virtio-pciデバイスのBAR0 I/Oベースを保持するハンドル */
typedef struct {
    /** BAR0のI/O空間ベースポート番号 */
    UINT16 io_base;
} os_virtio_device;

/**
 * デバイスをリセットし、ACKNOWLEDGE|DRIVERまでステータスを進め、
 * ホストのfeatureビットをすべて無効(0)として返答する。
 * QUEUE設定とDRIVER_OKは呼び出し側が別途行う。
 * @param dev 初期化対象。io_baseは呼び出し前に設定しておくこと
 */
void os_virtio_reset_and_negotiate(os_virtio_device *dev);

/**
 * 指定したキューをQUEUE_SELECTで選択し、QUEUE_SIZEレジスタの値(デバイス決定、read-only)を返す
 * @param dev 対象デバイス
 * @param queue_index キュー番号
 * @return デバイスが報告したキューサイズ
 */
UINT16 os_virtio_select_and_get_queue_size(const os_virtio_device *dev, UINT16 queue_index);

/**
 * 指定したキューにdescriptor table+avail/used ringの物理アドレスを設定する。
 * 事前にos_virtio_select_and_get_queue_sizeで同じqueue_indexを選択しておくこと。
 * @param dev 対象デバイス
 * @param queue_index キュー番号
 * @param phys_addr desc table先頭の物理アドレス(4096バイトアライン)
 */
void os_virtio_set_queue_address(const os_virtio_device *dev, UINT16 queue_index, UINT64 phys_addr);

/**
 * 指定したキューへ新しいavailエントリが積まれたことをデバイスへ通知する
 * @param dev 対象デバイス
 * @param queue_index キュー番号
 */
void os_virtio_notify_queue(const os_virtio_device *dev, UINT16 queue_index);

/** デバイスステータスにDRIVER_OKを立てて初期化を完了させる */
void os_virtio_set_driver_ok(const os_virtio_device *dev);

#endif /* _DRIVERS_VIRTIO_H_ */
