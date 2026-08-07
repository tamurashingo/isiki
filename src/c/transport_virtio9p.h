#ifndef _TRANSPORT_VIRTIO9P_H_
#define _TRANSPORT_VIRTIO9P_H_

#include "p9_transport.h"

/**
 * VirtIO-9p(legacy PCI)によるp9_transport_tのシングルトンインスタンスを返す。
 * PCI検出・virtqueue初期化はensure_ready呼び出し時に1度だけ行う。
 * @return p9_transport_tのインスタンス
 */
p9_transport_t* os_transport_virtio9p_instance(void);

#endif /* _TRANSPORT_VIRTIO9P_H_ */
