#include "virtio.h"
#include "../interrupt.h"

/** ページサイズ(PFN計算用) */
#define VIRTIO_PAGE_SHIFT 12

void os_virtio_reset_and_negotiate(os_virtio_device *dev) {
    /* reset: status=0 */
    outb((UINT16)(dev->io_base + VIRTIO_REG_DEVICE_STATUS), 0x00);

    outb((UINT16)(dev->io_base + VIRTIO_REG_DEVICE_STATUS), VIRTIO_STATUS_ACKNOWLEDGE);
    outb((UINT16)(dev->io_base + VIRTIO_REG_DEVICE_STATUS), VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* feature negotiation: 高度な機能は使わないのでguest featuresは常に0を返す */
    inl((UINT16)(dev->io_base + VIRTIO_REG_HOST_FEATURES));
    outl((UINT16)(dev->io_base + VIRTIO_REG_GUEST_FEATURES), 0x00000000u);
}

UINT16 os_virtio_select_and_get_queue_size(const os_virtio_device *dev, UINT16 queue_index) {
    outw((UINT16)(dev->io_base + VIRTIO_REG_QUEUE_SELECT), queue_index);
    return inw((UINT16)(dev->io_base + VIRTIO_REG_QUEUE_SIZE));
}

void os_virtio_set_queue_address(const os_virtio_device *dev, UINT16 queue_index, UINT64 phys_addr) {
    outw((UINT16)(dev->io_base + VIRTIO_REG_QUEUE_SELECT), queue_index);
    outl((UINT16)(dev->io_base + VIRTIO_REG_QUEUE_ADDRESS), (UINT32)(phys_addr >> VIRTIO_PAGE_SHIFT));
}

void os_virtio_notify_queue(const os_virtio_device *dev, UINT16 queue_index) {
    outw((UINT16)(dev->io_base + VIRTIO_REG_QUEUE_NOTIFY), queue_index);
}

void os_virtio_set_driver_ok(const os_virtio_device *dev) {
    UINT8 status = inb((UINT16)(dev->io_base + VIRTIO_REG_DEVICE_STATUS));
    outb((UINT16)(dev->io_base + VIRTIO_REG_DEVICE_STATUS), status | VIRTIO_STATUS_DRIVER_OK);
}
