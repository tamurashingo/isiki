#ifndef _DRIVERS_IDE_H_
#define _DRIVERS_IDE_H_

#include "../types.h"

/** 1セクタのバイト数(ATA PIOは常に512byte単位) */
#define IDE_SECTOR_SIZE 512

/**
 * IDE/ATAチャネル1台のATAデバイスのハンドル。Secondaryチャネル(0x170-0x177,
 * control 0x376)のmasterを対象とする(Primary channelはESP起動用vvfatドライブが
 * 占有しているため触らない。詳細はdocuments参照)。
 */
typedef struct {
    /** コマンドブロックレジスタのベースポート(Secondary: 0x170) */
    UINT16 io_base;
    /** コントロールブロックレジスタのベースポート(Secondary: 0x376) */
    UINT16 ctrl_base;
    /** drive/headレジスタに立てるdrive選択ビット(0xA0=master, 0xB0=slave) */
    UINT8 drive_select;
    /** IDENTIFY DEVICEのword60-61から得たLBA28の総セクタ数 */
    UINT32 total_sectors;
    /** IDENTIFYに成功した場合1 */
    int present;
} os_ide_device;

/**
 * io_base/ctrl_base上のdrive(0=master,1=slave)へIDENTIFY DEVICE(0xEC)を送り、
 * デバイスの有無とLBA28の総セクタ数を調べる。デバイス非搭載(floating bus、
 * status読み込みが0x00/0xFF)の場合はBSY待ちに入らず即座に0を返す(ハング防止)。
 * ATAPI/SATAブリッジのsignature(LBA_MID/LBA_HIGHが0x14/0xEBまたは0x69/0x96)を
 * 検出した場合も、PATA HDD以外はスコープ外として0を返す。
 * モデル名(word27-46)やLBA48対応(word83 bit10)は解析しない(明示的に対象外)。
 * @param dev 初期化対象。成功時にio_base/ctrl_base/drive_select/total_sectors/presentが設定される
 * @param io_base コマンドブロックレジスタのベースポート
 * @param ctrl_base コントロールブロックレジスタのベースポート
 * @param drive 0=master, 1=slave
 * @param err_msg 失敗時に理由を書き込む先(NULL可)
 * @param err_msg_cap err_msgのバイト数(NUL込み)
 * @return 成功時1、失敗時0
 */
int os_ide_identify(os_ide_device *dev, UINT16 io_base, UINT16 ctrl_base, UINT8 drive,
                     char *err_msg, UINT32 err_msg_cap);

/**
 * devからLBA28のlbaを起点にcount個(1-256、256は0として送る)のセクタをREAD
 * SECTORS(0x20)でPIO読み込みし、bufへ格納する(bufはcount*512byte以上必要)。
 * セクタ単位でBSY→ERR/DF→DRQをポーリングしてから256wordずつ転送する。
 * @return 成功時1、BSY/DRQのスピンキャップ超過やERR/DFビット検出時0
 */
int os_ide_read_sectors(os_ide_device *dev, UINT32 lba, UINT16 count, UINT8 *buf,
                         char *err_msg, UINT32 err_msg_cap);

/**
 * devのLBA28のlbaを起点にbufの内容をcount個のセクタへWRITE SECTORS(0x30)で
 * PIO書き込みし、完了後CACHE FLUSH(0xE7)を発行してBSYクリアを待つ。
 * @return 成功時1、失敗時0
 */
int os_ide_write_sectors(os_ide_device *dev, UINT32 lba, UINT16 count, const UINT8 *buf,
                          char *err_msg, UINT32 err_msg_cap);

#endif /* _DRIVERS_IDE_H_ */
