#include "transport_virtio9p.h"
#include "drivers/pci.h"
#include "drivers/virtio.h"
#include "drivers/virtqueue.h"

/** virtqueue用DMA領域のサイズ */
#define VIRTQ9P_MEM_SIZE 8192

static UINT8 g_virtq9p_mem[VIRTQ9P_MEM_SIZE] __attribute__((aligned(4096)));

static os_virtio_device g_vdev;
static os_virtqueue g_vq;
static int g_ready = 0;

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

static int transport_ensure_ready(p9_transport_t *self, char *err_msg, UINT32 err_msg_cap) {
    (void)self;

    if (g_ready) {
        return 1;
    }

    os_pci_device pci_dev;
    if (!os_pci_find_virtio_9p(&pci_dev)) {
        set_err(err_msg, err_msg_cap, "virtio-9p PCI device not found (check -device virtio-9p-pci in Makefile)");
        return 0;
    }

    os_pci_enable_device(&pci_dev);

    if ((pci_dev.bar0_raw & 0x1) == 0) {
        set_err(err_msg, err_msg_cap, "virtio-9p BAR0 is not an I/O space BAR");
        return 0;
    }

    g_vdev.io_base = os_pci_bar0_io_base(pci_dev.bar0_raw);

    os_virtio_reset_and_negotiate(&g_vdev);

    if (!os_virtqueue_init(&g_vq, &g_vdev, 0, g_virtq9p_mem, sizeof(g_virtq9p_mem))) {
        set_err(err_msg, err_msg_cap,
                "virtqueue init failed: device queue size exceeds VIRTQ_MAX_SIZE or reserved memory too small");
        return 0;
    }

    os_virtio_set_driver_ok(&g_vdev);

    g_ready = 1;
    return 1;
}

static int transport_send(p9_transport_t *self, const UINT8 *tx_buf, UINT32 tx_len,
                           UINT8 *rx_buf, UINT32 rx_cap, char *err_msg, UINT32 err_msg_cap) {
    (void)self;
    (void)err_msg;
    (void)err_msg_cap;

#ifndef ISIKIOS_UNIT_TEST
    asm volatile("cli");
#endif
    os_virtqueue_submit(&g_vq, (UINT64)(lisp_addr_t)tx_buf, tx_len, (UINT64)(lisp_addr_t)rx_buf, rx_cap);
    return 1;
}

static int transport_recv(p9_transport_t *self, UINT32 *out_rx_len, char *err_msg, UINT32 err_msg_cap) {
    (void)self;

    int polled = os_virtqueue_poll(&g_vq, out_rx_len);
#ifndef ISIKIOS_UNIT_TEST
    asm volatile("sti");
#endif
    if (!polled) {
        set_err(err_msg, err_msg_cap, "virtqueue poll timed out");
        return 0;
    }
    return 1;
}

static p9_transport_t g_transport = {
    .ensure_ready = transport_ensure_ready,
    .send = transport_send,
    .recv = transport_recv,
};

p9_transport_t* os_transport_virtio9p_instance(void) {
    return &g_transport;
}
