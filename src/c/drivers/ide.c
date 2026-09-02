#include "ide.h"
#include "../interrupt.h"

/* コマンドブロックレジスタ(io_baseからのオフセット) */
#define IDE_REG_DATA        0
#define IDE_REG_ERROR       1
#define IDE_REG_SECCOUNT    2
#define IDE_REG_LBA_LOW     3
#define IDE_REG_LBA_MID     4
#define IDE_REG_LBA_HIGH    5
#define IDE_REG_DRIVE_HEAD  6
#define IDE_REG_STATUS      7
#define IDE_REG_COMMAND     7

/* コントロールブロックレジスタ(ctrl_baseからのオフセット) */
#define IDE_REG_CONTROL     0

/* status/altstatusレジスタのビット */
#define IDE_STATUS_ERR 0x01u
#define IDE_STATUS_DF  0x20u
#define IDE_STATUS_DRQ 0x08u
#define IDE_STATUS_BSY 0x80u

/* control(nIEN)レジスタのビット。bit1=1で割込み出力を無効化する */
#define IDE_CONTROL_NIEN 0x02u

/* drive/headレジスタの固定ビット(bit7,5=1) + LBAモード(bit6=1) + drive選択(bit4)。
 * bit6(LBA)を立てないとCHSアドレッシングと解釈され、LBA_LOW/MID/HIGHへ設定した
 * 値がシリンダ/セクタ番号として誤読される(READ/WRITE SECTORSが誤ったセクタを
 * 指してしまう。IDENTIFYはアドレッシング方式を問わないため、このビットが無くても
 * 見かけ上は成功してしまう点に注意) */
#define IDE_DRIVE_HEAD_BASE   0xE0u
#define IDE_DRIVE_HEAD_SLAVE  0x10u

#define IDE_CMD_IDENTIFY     0xECu
#define IDE_CMD_READ_SECTORS 0x20u
#define IDE_CMD_WRITE_SECTORS 0x30u
#define IDE_CMD_CACHE_FLUSH  0xE7u

/* BSY/DRQポーリングのスピンキャップ。virtqueue.cのVIRTQ_POLL_SPIN_LIMITと同じ考え方 */
#define IDE_POLL_SPIN_LIMIT 100000000u

static void set_err(char *err_msg, UINT32 err_msg_cap, const char *msg) {
    if (err_msg == 0 || err_msg_cap == 0) {
        return;
    }
    UINT32 i = 0;
    while (msg[i] != '\0' && i + 1 < err_msg_cap) {
        err_msg[i] = msg[i];
        i++;
    }
    err_msg[i] = '\0';
}

/** statusレジスタのBSYビットが下がるまで待つ。超過時0 */
static int ide_wait_not_busy(UINT16 io_base) {
    UINT32 spin = 0;
    while (inb(io_base + IDE_REG_STATUS) & IDE_STATUS_BSY) {
        spin++;
        if (spin >= IDE_POLL_SPIN_LIMIT) {
            return 0;
        }
    }
    return 1;
}

/** BSYが下がりDRQが立つまで待つ。ERR/DFが立っていれば即失敗。超過時も失敗 */
static int ide_wait_drq(UINT16 io_base) {
    UINT32 spin = 0;
    for (;;) {
        UINT8 status = inb(io_base + IDE_REG_STATUS);
        if (status & (IDE_STATUS_ERR | IDE_STATUS_DF)) {
            return 0;
        }
        if (!(status & IDE_STATUS_BSY) && (status & IDE_STATUS_DRQ)) {
            return 1;
        }
        spin++;
        if (spin >= IDE_POLL_SPIN_LIMIT) {
            return 0;
        }
    }
}

/** コマンドレジスタへoutbした直後、statusが実際に更新されるまでの実機ドライバ定石の
 * 遅延。Alternate Status(ctrl_base)を4回捨て読みする(いわゆる400ns delay、OSDev
 * wikiのIDEドライバでも標準的に使われる手法)。これが無いと、コマンド発行直後の
 * 一瞬だけ前回転送のBSY/DRQが残っていて、そのままDRQ=1と誤認して前回の転送結果を
 * 読んでしまう競合が起こり得る */
static void ide_delay400ns(UINT16 ctrl_base) {
    inb(ctrl_base + IDE_REG_CONTROL);
    inb(ctrl_base + IDE_REG_CONTROL);
    inb(ctrl_base + IDE_REG_CONTROL);
    inb(ctrl_base + IDE_REG_CONTROL);
}

/** lba(28bit)/countをコマンドブロックレジスタへセットし、drive選択も行う */
static void ide_setup_lba(os_ide_device *dev, UINT32 lba, UINT16 count) {
    UINT8 lba_top = (UINT8)((lba >> 24) & 0x0Fu);
    outb(dev->io_base + IDE_REG_DRIVE_HEAD, (UINT8)(dev->drive_select | lba_top));
    outb(dev->io_base + IDE_REG_SECCOUNT, (UINT8)count);
    outb(dev->io_base + IDE_REG_LBA_LOW, (UINT8)(lba & 0xFFu));
    outb(dev->io_base + IDE_REG_LBA_MID, (UINT8)((lba >> 8) & 0xFFu));
    outb(dev->io_base + IDE_REG_LBA_HIGH, (UINT8)((lba >> 16) & 0xFFu));
}

int os_ide_identify(os_ide_device *dev, UINT16 io_base, UINT16 ctrl_base, UINT8 drive,
                     char *err_msg, UINT32 err_msg_cap) {
    dev->io_base = io_base;
    dev->ctrl_base = ctrl_base;
    dev->drive_select = (UINT8)(IDE_DRIVE_HEAD_BASE | (drive ? IDE_DRIVE_HEAD_SLAVE : 0));
    dev->total_sectors = 0;
    dev->model[0] = '\0';
    dev->present = 0;

    /* 割込みを電気的にも出させない(PIC側は元々マスク済みだが二重の安全策) */
    outb(ctrl_base + IDE_REG_CONTROL, IDE_CONTROL_NIEN);

    outb(io_base + IDE_REG_DRIVE_HEAD, dev->drive_select);
    outb(io_base + IDE_REG_SECCOUNT, 0);
    outb(io_base + IDE_REG_LBA_LOW, 0);
    outb(io_base + IDE_REG_LBA_MID, 0);
    outb(io_base + IDE_REG_LBA_HIGH, 0);
    outb(io_base + IDE_REG_COMMAND, IDE_CMD_IDENTIFY);
    ide_delay400ns(ctrl_base);

    UINT8 status = inb(io_base + IDE_REG_STATUS);
    if (status == 0x00u || status == 0xFFu) {
        set_err(err_msg, err_msg_cap, "ide: no device (floating bus)");
        return 0;
    }

    UINT8 lba_mid = inb(io_base + IDE_REG_LBA_MID);
    UINT8 lba_high = inb(io_base + IDE_REG_LBA_HIGH);
    if ((lba_mid == 0x14u && lba_high == 0xEBu) || (lba_mid == 0x69u && lba_high == 0x96u)) {
        set_err(err_msg, err_msg_cap, "ide: non-PATA device (ATAPI/SATA bridge), out of scope");
        return 0;
    }

    if (!ide_wait_drq(io_base)) {
        set_err(err_msg, err_msg_cap, "ide: identify timed out or reported error");
        return 0;
    }

    UINT16 identify_data[256];
    for (int i = 0; i < 256; i++) {
        identify_data[i] = inw(io_base + IDE_REG_DATA);
    }

    UINT32 lba28 = ((UINT32)identify_data[61] << 16) | (UINT32)identify_data[60];
    dev->total_sectors = lba28;

    /* word27-46(モデル名)は1wordに2文字、上位byteが先に来る(byte-swapped) */
    for (int i = 0; i < 20; i++) {
        UINT16 word = identify_data[27 + i];
        dev->model[2 * i] = (char)((word >> 8) & 0xFFu);
        dev->model[2 * i + 1] = (char)(word & 0xFFu);
    }
    dev->model[40] = '\0';
    int end = 40;
    while (end > 0 && (dev->model[end - 1] == ' ' || dev->model[end - 1] == '\0')) {
        end--;
    }
    dev->model[end] = '\0';

    dev->present = 1;
    return 1;
}

int os_ide_read_sectors(os_ide_device *dev, UINT32 lba, UINT16 count, UINT8 *buf,
                         char *err_msg, UINT32 err_msg_cap) {
    if (!dev->present) {
        set_err(err_msg, err_msg_cap, "ide: device not present");
        return 0;
    }

    if (!ide_wait_not_busy(dev->io_base)) {
        set_err(err_msg, err_msg_cap, "ide: device busy, timed out");
        return 0;
    }

    ide_setup_lba(dev, lba, count);
    outb(dev->io_base + IDE_REG_COMMAND, IDE_CMD_READ_SECTORS);
    ide_delay400ns(dev->ctrl_base);

    UINT16 sectors = count == 0 ? 256 : count;
    UINT16 *words = (UINT16 *)buf;
    for (UINT16 s = 0; s < sectors; s++) {
        if (!ide_wait_drq(dev->io_base)) {
            set_err(err_msg, err_msg_cap, "ide: read failed or timed out");
            return 0;
        }
        for (int i = 0; i < 256; i++) {
            words[s * 256 + i] = inw(dev->io_base + IDE_REG_DATA);
        }
    }
    return 1;
}

int os_ide_write_sectors(os_ide_device *dev, UINT32 lba, UINT16 count, const UINT8 *buf,
                          char *err_msg, UINT32 err_msg_cap) {
    if (!dev->present) {
        set_err(err_msg, err_msg_cap, "ide: device not present");
        return 0;
    }

    /* コマンド発行〜CACHE FLUSH完了まで割込みを禁止する(ネイティブホストでの
     * ユニットテストではcli/sti自体が無効な命令のため、実機ビルドのみで有効にする) */
#ifndef ISIKIOS_UNIT_TEST
    __asm__ __volatile__ ("cli");
#endif

    if (!ide_wait_not_busy(dev->io_base)) {
        set_err(err_msg, err_msg_cap, "ide: device busy, timed out");
#ifndef ISIKIOS_UNIT_TEST
        __asm__ __volatile__ ("sti");
#endif
        return 0;
    }

    ide_setup_lba(dev, lba, count);
    outb(dev->io_base + IDE_REG_COMMAND, IDE_CMD_WRITE_SECTORS);
    ide_delay400ns(dev->ctrl_base);

    UINT16 sectors = count == 0 ? 256 : count;
    const UINT16 *words = (const UINT16 *)buf;
    for (UINT16 s = 0; s < sectors; s++) {
        if (!ide_wait_drq(dev->io_base)) {
            set_err(err_msg, err_msg_cap, "ide: write failed or timed out");
#ifndef ISIKIOS_UNIT_TEST
            __asm__ __volatile__ ("sti");
#endif
            return 0;
        }
        for (int i = 0; i < 256; i++) {
            outw(dev->io_base + IDE_REG_DATA, words[s * 256 + i]);
        }
    }

    outb(dev->io_base + IDE_REG_COMMAND, IDE_CMD_CACHE_FLUSH);
    ide_delay400ns(dev->ctrl_base);
    if (!ide_wait_not_busy(dev->io_base)) {
        set_err(err_msg, err_msg_cap, "ide: cache flush timed out");
#ifndef ISIKIOS_UNIT_TEST
        __asm__ __volatile__ ("sti");
#endif
        return 0;
    }
#ifndef ISIKIOS_UNIT_TEST
    __asm__ __volatile__ ("sti");
#endif
    return 1;
}
