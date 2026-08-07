#ifndef _DRIVERS_VIRTQUEUE_H_
#define _DRIVERS_VIRTQUEUE_H_

#include "../types.h"
#include "virtio.h"

/** legacy virtqueueで想定する最大キューサイズ。実際の値はos_virtqueue_initで検証する */
#define VIRTQ_MAX_SIZE 128

/** descriptor: 次のdescriptorが続く */
#define VIRTQ_DESC_F_NEXT  0x1
/** descriptor: デバイスが書き込む(受信)バッファである */
#define VIRTQ_DESC_F_WRITE 0x2

/** virtqueue descriptor 1エントリ */
typedef struct {
    /** バッファの物理アドレス */
    UINT64 addr;
    /** バッファの長さ(バイト) */
    UINT32 len;
    /** VIRTQ_DESC_F_*のビットOR */
    UINT16 flags;
    /** flagsにNEXTが立っている場合の次のdescriptorインデックス */
    UINT16 next;
} __attribute__((packed)) virtq_desc;

/** used ring 1エントリ */
typedef struct {
    /** 処理済みdescriptor chainの先頭インデックス */
    UINT32 id;
    /** デバイスが書き込んだ合計バイト数 */
    UINT32 len;
} __attribute__((packed)) virtq_used_elem;

/** 1つのvirtqueueの状態を保持するハンドル */
typedef struct {
    /** 紐づくvirtioデバイス(notify発行に使う) */
    const os_virtio_device *dev;
    /** キュー番号(QUEUE_SELECT/QUEUE_NOTIFYに使う) */
    UINT16 queue_index;
    /** デバイスが報告した実際のキューサイズ */
    UINT16 queue_size;
    /** descriptor table先頭 */
    virtq_desc *desc;
    /** avail ring: flags */
    volatile UINT16 *avail_flags;
    /** avail ring: idx */
    volatile UINT16 *avail_idx;
    /** avail ring: ring本体(queue_size個) */
    volatile UINT16 *avail_ring;
    /** used ring: flags */
    volatile UINT16 *used_flags;
    /** used ring: idx */
    volatile UINT16 *used_idx;
    /** used ring: ring本体(queue_size個) */
    virtq_used_elem *used_ring;
    /** 最後にpollで確認したused_idxの値 */
    UINT16 last_used_idx;
} os_virtqueue;

/**
 * memに指定したメモリ領域上にvirtqueueのレイアウト(desc table + avail ring を4096境界に
 * 切り上げ、その直後にused ring)を構築し、デバイスへQUEUE_ADDRESSを設定して初期化する。
 * @param vq 初期化対象
 * @param dev 対象のvirtioデバイス(io_baseが設定済みであること)
 * @param queue_index キュー番号(このデバイスでは0のみ使用)
 * @param mem virtqueue用に確保した4096バイトアラインのメモリ領域先頭
 * @param mem_cap memの確保済みサイズ(バイト)。実際のqueue_sizeがVIRTQ_MAX_SIZEを
 *                超えている場合や、レイアウトがmem_capに収まらない場合は0を返して失敗させる
 * @return 成功時1、失敗時(queue_size不正/mem_cap不足)0
 */
int os_virtqueue_init(os_virtqueue *vq, const os_virtio_device *dev, UINT16 queue_index, void *mem, UINT32 mem_cap);

/**
 * tx_addr(送信データ)→rx_addr(受信バッファ)の2要素descriptor chainを1つだけ
 * avail ringに積み、デバイスへ通知する。要求は常に1つずつ、完了をpollしてから次を積む。
 * @param vq 対象virtqueue
 * @param tx_addr 送信バッファの物理アドレス
 * @param tx_len 送信バッファの長さ
 * @param rx_addr 受信バッファの物理アドレス
 * @param rx_cap 受信バッファの容量
 */
void os_virtqueue_submit(os_virtqueue *vq, UINT64 tx_addr, UINT32 tx_len, UINT64 rx_addr, UINT32 rx_cap);

/**
 * used ringのidxが進むまでbusy-waitし、直近のリクエストの完了を待つ。
 * @param vq 対象virtqueue
 * @param out_len 完了時、デバイスが書き込んだ合計バイト数(rx_len)を格納する先
 * @return 完了した場合1、spin上限に達してタイムアウトした場合0
 */
int os_virtqueue_poll(os_virtqueue *vq, UINT32 *out_len);

#endif /* _DRIVERS_VIRTQUEUE_H_ */
