#include "block_device.h"
#include "drivers/ide.h"

/* Secondary IDEチャネル(0x170-0x177, control 0x376)。Primaryは
 * -drive format=raw,file=fat:rw:./esp_dir(ESP起動ドライブ)がQEMUのデフォルトで
 * 占有しているため触らない */
#define IDE_SECONDARY_IO_BASE   0x170
#define IDE_SECONDARY_CTRL_BASE 0x376
#define IDE_SECONDARY_DRIVE_MASTER 0

#define IDE_SECTOR_BUFFER_SIZE 512
static UINT8 g_ide_sector_buffer[IDE_SECTOR_BUFFER_SIZE] __attribute__((aligned(8)));

static os_ide_device g_ide_dev;
static block_device_t g_ide_block_device;
static int g_ide_ready = 0;

static int ide_bd_init(block_device_t *self, char *err_msg, UINT32 err_msg_cap) {
    os_ide_device *dev = (os_ide_device *)self->priv_data;
    if (!os_ide_identify(dev, IDE_SECONDARY_IO_BASE, IDE_SECONDARY_CTRL_BASE,
                          IDE_SECONDARY_DRIVE_MASTER, err_msg, err_msg_cap)) {
        return 0;
    }
    self->total_sectors = dev->total_sectors;
    return 1;
}

static int ide_bd_read_sectors(block_device_t *self, UINT32 lba, UINT16 count, UINT8 *buf,
                                char *err_msg, UINT32 err_msg_cap) {
    return os_ide_read_sectors((os_ide_device *)self->priv_data, lba, count, buf, err_msg, err_msg_cap);
}

static int ide_bd_write_sectors(block_device_t *self, UINT32 lba, UINT16 count, const UINT8 *buf,
                                 char *err_msg, UINT32 err_msg_cap) {
    return os_ide_write_sectors((os_ide_device *)self->priv_data, lba, count, buf, err_msg, err_msg_cap);
}

static int ide_bd_flush(block_device_t *self, char *err_msg, UINT32 err_msg_cap) {
    /* os_ide_write_sectorsが書き込み完了時に毎回CACHE FLUSHまで実行するため、
     * 明示的なflush呼び出しでは何もする必要がない */
    (void)self;
    (void)err_msg;
    (void)err_msg_cap;
    return 1;
}

block_device_t *os_block_device_ide_instance(void) {
    if (g_ide_ready) {
        return g_ide_dev.present ? &g_ide_block_device : 0;
    }

    g_ide_block_device.name = "ide-secondary-master";
    g_ide_block_device.sector_size = IDE_SECTOR_BUFFER_SIZE;
    g_ide_block_device.total_sectors = 0;
    g_ide_block_device.init = ide_bd_init;
    g_ide_block_device.read_sectors = ide_bd_read_sectors;
    g_ide_block_device.write_sectors = ide_bd_write_sectors;
    g_ide_block_device.flush = ide_bd_flush;
    g_ide_block_device.priv_data = &g_ide_dev;

    char err_msg[128];
    err_msg[0] = '\0';
    g_ide_block_device.init(&g_ide_block_device, err_msg, sizeof(err_msg));

    g_ide_ready = 1;
    return g_ide_dev.present ? &g_ide_block_device : 0;
}

UINT8 *os_block_device_ide_sector_buffer(void) {
    return g_ide_sector_buffer;
}
