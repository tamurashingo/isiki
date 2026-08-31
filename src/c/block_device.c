#include "block_device.h"
#include "drivers/ide.h"
#include "runtime.h"

/* Secondaryチャネル(0x170-0x177, control 0x376)のみ扱う。Primaryチャネル
 * (0x1F0-0x1F7, control 0x3F6)はQEMUのデフォルトESP起動ドライブ(vvfat)が
 * 占有しているため、意図的にプローブしない(UEFI起動用に予約、サポート対象外)。 */
#define IDE_SECONDARY_IO_BASE   0x170
#define IDE_SECONDARY_CTRL_BASE 0x376

#define IDE_SECTOR_BUFFER_SIZE 512
static UINT8 g_ide_sector_buffer[IDE_SECTOR_BUFFER_SIZE] __attribute__((aligned(8)));

/** Secondaryチャネル上の1drive(master/slave)分の設定+検出状態。os_boot_allocで
 * 検出成功時のみ確保する(block_device_t.priv_dataが指す) */
typedef struct {
    UINT16 io_base;
    UINT16 ctrl_base;
    UINT8 drive;
    os_ide_device dev;
} ide_bd_slot_t;

/* Secondary master+slaveの最大2台のみ実際には検出されるが、将来他コントローラ
 * (NVMe/AHCI等)を追加する余地として少し余裕を持たせる。テーブル自体は固定長の
 * 静的配列だが、各デバイスの実体(ide_bd_slot_t/block_device_t)はos_boot_allocで
 * 動的に確保するため、"デバイス1台固定"にはならない */
#define MAX_BLOCK_DEVICES 8
static block_device_t *g_block_devices[MAX_BLOCK_DEVICES];
static UINT32 g_block_device_count = 0;

static int ide_bd_init(block_device_t *self, char *err_msg, UINT32 err_msg_cap) {
    ide_bd_slot_t *slot = (ide_bd_slot_t *)self->priv_data;
    if (!os_ide_identify(&slot->dev, slot->io_base, slot->ctrl_base, slot->drive,
                          err_msg, err_msg_cap)) {
        return 0;
    }
    self->total_sectors = slot->dev.total_sectors;

    int i = 0;
    while (slot->dev.model[i] != '\0' && i + 1 < (int)sizeof(self->model)) {
        self->model[i] = slot->dev.model[i];
        i++;
    }
    self->model[i] = '\0';
    return 1;
}

static int ide_bd_read_sectors(block_device_t *self, UINT32 lba, UINT16 count, UINT8 *buf,
                                char *err_msg, UINT32 err_msg_cap) {
    ide_bd_slot_t *slot = (ide_bd_slot_t *)self->priv_data;
    return os_ide_read_sectors(&slot->dev, lba, count, buf, err_msg, err_msg_cap);
}

static int ide_bd_write_sectors(block_device_t *self, UINT32 lba, UINT16 count, const UINT8 *buf,
                                 char *err_msg, UINT32 err_msg_cap) {
    ide_bd_slot_t *slot = (ide_bd_slot_t *)self->priv_data;
    return os_ide_write_sectors(&slot->dev, lba, count, buf, err_msg, err_msg_cap);
}

static int ide_bd_flush(block_device_t *self, char *err_msg, UINT32 err_msg_cap) {
    /* os_ide_write_sectorsが書き込み完了時に毎回CACHE FLUSHまで実行するため、
     * 明示的なflush呼び出しでは何もする必要がない */
    (void)self;
    (void)err_msg;
    (void)err_msg_cap;
    return 1;
}

/** io_base/ctrl_base上のdrive(0=master,1=slave)をプローブし、検出できた場合のみ
 * os_boot_allocでios_bd_slot_t/block_device_tを確保してg_block_devicesへ登録する */
static void ide_probe_one(UINT16 io_base, UINT16 ctrl_base, UINT8 drive, const char *name) {
    if (g_block_device_count >= MAX_BLOCK_DEVICES) {
        return;
    }

    ide_bd_slot_t *slot = (ide_bd_slot_t *)os_boot_alloc(sizeof(ide_bd_slot_t), 8);
    slot->io_base = io_base;
    slot->ctrl_base = ctrl_base;
    slot->drive = drive;

    block_device_t *bd = (block_device_t *)os_boot_alloc(sizeof(block_device_t), 8);
    bd->name = name;
    bd->sector_size = IDE_SECTOR_BUFFER_SIZE;
    bd->total_sectors = 0;
    bd->model[0] = '\0';
    bd->init = ide_bd_init;
    bd->read_sectors = ide_bd_read_sectors;
    bd->write_sectors = ide_bd_write_sectors;
    bd->flush = ide_bd_flush;
    bd->priv_data = slot;

    char err_msg[128];
    err_msg[0] = '\0';
    if (bd->init(bd, err_msg, sizeof(err_msg))) {
        g_block_devices[g_block_device_count++] = bd;
    }
    /* 検出失敗時、slot/bdに確保したboot_alloc領域はそのまま無駄になる
     * (os_boot_allocに解放が無いため)。1台あたり100byte未満で、失敗するのは
     * 現状最大でもslave 1台のみなので許容する */
}

void os_block_device_probe_all(void) {
    ide_probe_one(IDE_SECONDARY_IO_BASE, IDE_SECONDARY_CTRL_BASE, 0, "ide-secondary-master");
    ide_probe_one(IDE_SECONDARY_IO_BASE, IDE_SECONDARY_CTRL_BASE, 1, "ide-secondary-slave");
}

UINT32 os_block_device_count(void) {
    return g_block_device_count;
}

block_device_t *os_block_device_at(UINT32 index) {
    if (index >= g_block_device_count) {
        return 0;
    }
    return g_block_devices[index];
}

UINT8 *os_block_device_ide_sector_buffer(void) {
    return g_ide_sector_buffer;
}
