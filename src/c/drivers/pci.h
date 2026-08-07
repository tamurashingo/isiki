#ifndef _DRIVERS_PCI_H_
#define _DRIVERS_PCI_H_

#include "../types.h"

/** PCIデバイスを一意に特定する情報 */
typedef struct {
    /** バス番号 */
    UINT8  bus;
    /** デバイス番号 */
    UINT8  device;
    /** ファンクション番号 */
    UINT8  function;
    /** ベンダーID */
    UINT16 vendor_id;
    /** デバイスID */
    UINT16 device_id;
    /** BAR0の生の値(下位ビットにI/O空間フラグ等を含む) */
    UINT32 bar0_raw;
} os_pci_device;

/**
 * PCI config spaceから4バイト読み込む
 * @param bus バス番号
 * @param device デバイス番号
 * @param function ファンクション番号
 * @param offset config space内のオフセット(4バイト境界)
 * @return 読み込んだ値
 */
UINT32 os_pci_config_read32(UINT8 bus, UINT8 device, UINT8 function, UINT8 offset);

/**
 * PCI config spaceから2バイト読み込む
 * @param bus バス番号
 * @param device デバイス番号
 * @param function ファンクション番号
 * @param offset config space内のオフセット
 * @return 読み込んだ値
 */
UINT16 os_pci_config_read16(UINT8 bus, UINT8 device, UINT8 function, UINT8 offset);

/**
 * PCI config spaceへ2バイト書き込む
 * @param bus バス番号
 * @param device デバイス番号
 * @param function ファンクション番号
 * @param offset config space内のオフセット
 * @param value 書き込む値
 */
void os_pci_config_write16(UINT8 bus, UINT8 device, UINT8 function, UINT8 offset, UINT16 value);

/**
 * bus 0を走査し、VirtIO legacy 9pデバイス(vendor=0x1AF4, device=0x1009)を探す
 * @param out 見つかった場合に情報を書き込む先
 * @return 見つかった場合1、見つからなければ0
 */
int os_pci_find_virtio_9p(os_pci_device *out);

/**
 * PCIデバイスのCommandレジスタにI/O Space + Bus Masterを立てて有効化する
 * @param dev 対象デバイス
 */
void os_pci_enable_device(const os_pci_device *dev);

/**
 * BAR0の生の値からI/O空間のベースポート番号を取り出す
 * @param bar0_raw os_pci_device.bar0_raw
 * @return ベースポート番号
 */
UINT16 os_pci_bar0_io_base(UINT32 bar0_raw);

#endif /* _DRIVERS_PCI_H_ */
