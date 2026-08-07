#include "pci.h"
#include "../interrupt.h"

/** CONFIG_ADDRESSポート */
#define PCI_CONFIG_ADDRESS 0xCF8
/** CONFIG_DATAポート */
#define PCI_CONFIG_DATA    0xCFC

/** VirtIOベンダーID */
#define VIRTIO_VENDOR_ID     0x1AF4
/** VirtIO legacy transitional 9pデバイスID (0x1000 + virtio device-type 9) */
#define VIRTIO_9P_DEVICE_ID  0x1009

/** PCI Commandレジスタのオフセット(config space上、16bit) */
#define PCI_COMMAND_OFFSET 0x04
/** Command: I/O Spaceアクセス許可 */
#define PCI_COMMAND_IO_SPACE  0x0001
/** Command: Bus Master許可(DMA用) */
#define PCI_COMMAND_BUS_MASTER 0x0004

/** BAR0のオフセット(config space上) */
#define PCI_BAR0_OFFSET 0x10
/** BAR0がI/O空間を指していることを示すビット */
#define PCI_BAR_IO_SPACE_FLAG 0x1

static UINT32 pci_config_address(UINT8 bus, UINT8 device, UINT8 function, UINT8 offset) {
    return 0x80000000u
        | ((UINT32)bus << 16)
        | ((UINT32)device << 11)
        | ((UINT32)function << 8)
        | (offset & 0xFCu);
}

UINT32 os_pci_config_read32(UINT8 bus, UINT8 device, UINT8 function, UINT8 offset) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, device, function, offset));
    return inl(PCI_CONFIG_DATA);
}

UINT16 os_pci_config_read16(UINT8 bus, UINT8 device, UINT8 function, UINT8 offset) {
    UINT32 value = os_pci_config_read32(bus, device, function, offset);
    UINT32 shift = (offset & 2) * 8;
    return (UINT16)((value >> shift) & 0xFFFF);
}

void os_pci_config_write16(UINT8 bus, UINT8 device, UINT8 function, UINT8 offset, UINT16 value) {
    UINT32 aligned_offset = offset & 0xFC;
    UINT32 shift = (offset & 2) * 8;
    UINT32 original = os_pci_config_read32(bus, device, function, (UINT8)aligned_offset);
    UINT32 mask = 0xFFFFu << shift;
    UINT32 merged = (original & ~mask) | ((UINT32)value << shift);
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, device, function, (UINT8)aligned_offset));
    outl(PCI_CONFIG_DATA, merged);
}

int os_pci_find_virtio_9p(os_pci_device *out) {
    for (UINT16 device = 0; device < 32; device++) {
        for (UINT8 function = 0; function < 8; function++) {
            UINT16 vendor_id = os_pci_config_read16(0, (UINT8)device, function, 0x00);
            if (vendor_id == 0xFFFF) {
                continue;
            }
            UINT16 device_id = os_pci_config_read16(0, (UINT8)device, function, 0x02);
            if (vendor_id == VIRTIO_VENDOR_ID && device_id == VIRTIO_9P_DEVICE_ID) {
                out->bus = 0;
                out->device = (UINT8)device;
                out->function = function;
                out->vendor_id = vendor_id;
                out->device_id = device_id;
                out->bar0_raw = os_pci_config_read32(0, (UINT8)device, function, PCI_BAR0_OFFSET);
                return 1;
            }
        }
    }
    return 0;
}

void os_pci_enable_device(const os_pci_device *dev) {
    UINT16 command = os_pci_config_read16(dev->bus, dev->device, dev->function, PCI_COMMAND_OFFSET);
    command |= PCI_COMMAND_IO_SPACE | PCI_COMMAND_BUS_MASTER;
    os_pci_config_write16(dev->bus, dev->device, dev->function, PCI_COMMAND_OFFSET, command);
}

UINT16 os_pci_bar0_io_base(UINT32 bar0_raw) {
    return (UINT16)(bar0_raw & ~PCI_BAR_IO_SPACE_FLAG);
}
