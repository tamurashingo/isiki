#ifndef _P9_TRANSPORT_H_
#define _P9_TRANSPORT_H_

#include "types.h"

/**
 * 9Pクライアント(virtio9p.c)がバイト列の送受信のためだけに使う抽象インタフェース。
 * 実装ごとにVirtIO/NIC等のtransport固有の詳細をカプセル化し、クライアント側は
 * 9Pプロトコルの意味(Twalk/Tread等)にのみ専念できるようにする。
 */
typedef struct _p9_transport {
    /**
     * 送受信路を使える状態にする(未確立なら確立する)。確立済みなら即成功を返す。
     * @param self このtransport
     * @param err_msg 失敗時にエラー内容(NUL終端文字列)を格納する先
     * @param err_msg_cap err_msgの容量
     * @return 成功時1、失敗時0
     */
    int (*ensure_ready)(struct _p9_transport *self, char *err_msg, UINT32 err_msg_cap);

    /**
     * tx_buf/tx_lenを送信し、応答をrx_buf/rx_capへ書き込ませる受信要求を発行する。
     * 応答の到着待ちはrecvで行う(send/recvは常に対で呼ぶこと)。
     * @param self このtransport
     * @param tx_buf 送信するバイト列
     * @param tx_len 送信バイト数
     * @param rx_buf 応答を書き込ませる先のバッファ
     * @param rx_cap rx_bufの容量
     * @param err_msg 失敗時にエラー内容(NUL終端文字列)を格納する先
     * @param err_msg_cap err_msgの容量
     * @return 成功時1、失敗時0
     */
    int (*send)(struct _p9_transport *self, const UINT8 *tx_buf, UINT32 tx_len,
                UINT8 *rx_buf, UINT32 rx_cap, char *err_msg, UINT32 err_msg_cap);

    /**
     * 直前のsendに対する応答が届くのを待つ。応答本体は既にsendで渡したrx_bufに
     * 書き込まれている。
     * @param self このtransport
     * @param out_rx_len 実際に受信したバイト数を格納する先
     * @param err_msg 失敗時(タイムアウト等)にエラー内容(NUL終端文字列)を格納する先
     * @param err_msg_cap err_msgの容量
     * @return 成功時1、失敗時0
     */
    int (*recv)(struct _p9_transport *self, UINT32 *out_rx_len, char *err_msg, UINT32 err_msg_cap);
} p9_transport_t;

#endif /* _P9_TRANSPORT_H_ */
