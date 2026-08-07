#include "virtqueue.h"

/** used ringのidxが進むまで待つ際のspin回数の上限(タイムアウト検出用) */
#define VIRTQ_POLL_SPIN_LIMIT 100000000u

static UINT32 align_up_4096(UINT32 value) {
    return (value + 4095u) & ~4095u;
}

static void zero_memory(UINT8 *mem, UINT32 size) {
    for (UINT32 i = 0; i < size; i++) {
        mem[i] = 0;
    }
}

int os_virtqueue_init(os_virtqueue *vq, const os_virtio_device *dev, UINT16 queue_index, void *mem, UINT32 mem_cap) {
    UINT16 queue_size = os_virtio_select_and_get_queue_size(dev, queue_index);
    if (queue_size == 0 || queue_size > VIRTQ_MAX_SIZE) {
        return 0;
    }

    UINT32 desc_bytes = (UINT32)queue_size * (UINT32)sizeof(virtq_desc);
    UINT32 avail_bytes = 6u + 2u * (UINT32)queue_size; /* flags(2)+idx(2)+ring(2*N)+used_event(2) */
    UINT32 used_ring_offset = align_up_4096(desc_bytes + avail_bytes);
    UINT32 used_bytes = 6u + 8u * (UINT32)queue_size; /* flags(2)+idx(2)+ring(8*N)+avail_event(2) */
    UINT32 total = used_ring_offset + used_bytes;

    if (total > mem_cap) {
        return 0;
    }

    zero_memory((UINT8 *)mem, total);

    UINT8 *base = (UINT8 *)mem;
    UINT8 *avail_base = base + desc_bytes;
    UINT8 *used_base = base + used_ring_offset;

    vq->dev = dev;
    vq->queue_index = queue_index;
    vq->queue_size = queue_size;
    vq->desc = (virtq_desc *)base;
    vq->avail_flags = (volatile UINT16 *)(avail_base + 0);
    vq->avail_idx = (volatile UINT16 *)(avail_base + 2);
    vq->avail_ring = (volatile UINT16 *)(avail_base + 4);
    vq->used_flags = (volatile UINT16 *)(used_base + 0);
    vq->used_idx = (volatile UINT16 *)(used_base + 2);
    vq->used_ring = (virtq_used_elem *)(used_base + 4);
    vq->last_used_idx = 0;

    os_virtio_set_queue_address(dev, queue_index, (UINT64)(lisp_addr_t)mem);

    return 1;
}

void os_virtqueue_submit(os_virtqueue *vq, UINT64 tx_addr, UINT32 tx_len, UINT64 rx_addr, UINT32 rx_cap) {
    vq->desc[0].addr = tx_addr;
    vq->desc[0].len = tx_len;
    vq->desc[0].flags = VIRTQ_DESC_F_NEXT;
    vq->desc[0].next = 1;

    vq->desc[1].addr = rx_addr;
    vq->desc[1].len = rx_cap;
    vq->desc[1].flags = VIRTQ_DESC_F_WRITE;
    vq->desc[1].next = 0;

    UINT16 avail_slot = (*vq->avail_idx) % vq->queue_size;
    vq->avail_ring[avail_slot] = 0; /* descriptor chainの先頭は常にdescriptor 0 */

    asm volatile ("" ::: "memory");
    (*vq->avail_idx)++;
    asm volatile ("" ::: "memory");

    os_virtio_notify_queue(vq->dev, vq->queue_index);
}

int os_virtqueue_poll(os_virtqueue *vq, UINT32 *out_len) {
    UINT32 spin = 0;
    while (*vq->used_idx == vq->last_used_idx) {
        spin++;
        if (spin >= VIRTQ_POLL_SPIN_LIMIT) {
            return 0;
        }
    }

    UINT16 used_slot = vq->last_used_idx % vq->queue_size;
    *out_len = vq->used_ring[used_slot].len;
    vq->last_used_idx++;

    return 1;
}
