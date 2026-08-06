#include "virtio9p.h"
#include "drivers/pci.h"
#include "drivers/virtio.h"
#include "drivers/virtqueue.h"
#include "p9.h"

/** 9Pセッションで使う最大メッセージサイズ */
#define P9_MSIZE 8192
/** virtqueue用DMA領域のサイズ */
#define VIRTQ9P_MEM_SIZE 8192
/** パスの最大コンポーネント数(Twalk用) */
#define P9_MAX_PATH_COMPONENTS 8
/** パス文字列を保持するローカルバッファのサイズ */
#define P9_PATH_BUF_SIZE 256

static UINT8 g_virtq9p_mem[VIRTQ9P_MEM_SIZE] __attribute__((aligned(4096)));
static UINT8 g_p9_tx_buf[P9_MSIZE] __attribute__((aligned(4096)));
static UINT8 g_p9_rx_buf[P9_MSIZE] __attribute__((aligned(4096)));
static UINT8 g_init_lisp_buf[4096] __attribute__((aligned(4096)));

static UINT32 str_len(const char *s) {
    UINT32 n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void str_copy(char *dst, UINT32 cap, const char *src) {
    UINT32 i = 0;
    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void str_append(char *dst, UINT32 cap, const char *src) {
    UINT32 len = str_len(dst);
    UINT32 i = 0;
    while (src[i] != '\0' && len + 1 < cap) {
        dst[len] = src[i];
        len++;
        i++;
    }
    dst[len] = '\0';
}

static void set_err(char *err_msg, UINT32 err_msg_cap, const char *msg) {
    if (err_msg != 0 && err_msg_cap > 0) {
        str_copy(err_msg, err_msg_cap, msg);
    }
}

/** path_buf内の'/'をNULに置き換えながらコンポーネントへのポインタをwnames_outへ積む */
static UINT16 split_path(char *path_buf, const char *wnames_out[P9_MAX_PATH_COMPONENTS]) {
    UINT16 count = 0;
    char *p = path_buf;
    if (*p == '/') {
        p++;
    }
    wnames_out[count++] = p;
    while (*p != '\0' && count < P9_MAX_PATH_COMPONENTS) {
        if (*p == '/') {
            *p = '\0';
            p++;
            wnames_out[count++] = p;
        } else {
            p++;
        }
    }
    return count;
}

/** tx_buf/rx_bufでリクエストを送りpollで完了を待ち、タイムアウト/Rerrorをerr_msgへ整形する */
static int p9_rpc(os_virtqueue *vq, UINT32 tx_len, UINT32 *out_rx_len, const char *stage,
                   char *err_msg, UINT32 err_msg_cap) {
    os_virtqueue_submit(vq, (UINT64)(lisp_addr_t)g_p9_tx_buf, tx_len,
                         (UINT64)(lisp_addr_t)g_p9_rx_buf, sizeof(g_p9_rx_buf));
    if (!os_virtqueue_poll(vq, out_rx_len)) {
        set_err(err_msg, err_msg_cap, stage);
        str_append(err_msg, err_msg_cap, ": virtqueue poll timed out");
        return 0;
    }

    char rerror_text[128];
    if (os_p9_check_error(g_p9_rx_buf, *out_rx_len, rerror_text, sizeof(rerror_text))) {
        set_err(err_msg, err_msg_cap, stage);
        str_append(err_msg, err_msg_cap, ": Rerror: ");
        str_append(err_msg, err_msg_cap, rerror_text);
        return 0;
    }

    return 1;
}

int os_virtio9p_load_file(const char *path, UINT8 *result_buf, UINT32 result_cap,
                           UINT32 *out_len, char *err_msg, UINT32 err_msg_cap) {
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

    os_virtio_device vdev;
    vdev.io_base = os_pci_bar0_io_base(pci_dev.bar0_raw);

    os_virtio_reset_and_negotiate(&vdev);

    os_virtqueue vq;
    if (!os_virtqueue_init(&vq, &vdev, 0, g_virtq9p_mem, sizeof(g_virtq9p_mem))) {
        set_err(err_msg, err_msg_cap,
                "virtqueue init failed: device queue size exceeds VIRTQ_MAX_SIZE or reserved memory too small");
        return 0;
    }

    os_virtio_set_driver_ok(&vdev);

    UINT32 tx_len;
    UINT32 rx_len;

    /* Tversion: セッション確立前なのでtag=NOTAG(0xFFFF)を使う。
       QEMUのvirtio-9pサーバは素の"9P2000"を受け付けないため"9P2000.u"を使う */
    tx_len = os_p9_build_tversion(g_p9_tx_buf, 0xFFFF, P9_MSIZE, "9P2000.u");
    if (!p9_rpc(&vq, tx_len, &rx_len, "Tversion", err_msg, err_msg_cap)) {
        return 0;
    }
    UINT32 negotiated_msize;
    char version_str[16];
    if (!os_p9_parse_rversion(g_p9_rx_buf, rx_len, &negotiated_msize, version_str, sizeof(version_str))) {
        set_err(err_msg, err_msg_cap, "Tversion: malformed Rversion response");
        return 0;
    }

    /* Tattach: fid=0をroot(aname="")へattachする */
    tx_len = os_p9_build_tattach(g_p9_tx_buf, 0, 0, 0xFFFFFFFF, "root", "", P9_NONUNAME);
    if (!p9_rpc(&vq, tx_len, &rx_len, "Tattach", err_msg, err_msg_cap)) {
        return 0;
    }
    if (!os_p9_parse_rattach(g_p9_rx_buf, rx_len)) {
        set_err(err_msg, err_msg_cap, "Tattach: malformed Rattach response");
        return 0;
    }

    /* Twalk: fid=0からpathの各要素をnewfid=1へwalkする */
    char path_buf[P9_PATH_BUF_SIZE];
    str_copy(path_buf, sizeof(path_buf), path);
    const char *wnames[P9_MAX_PATH_COMPONENTS];
    UINT16 nwname = split_path(path_buf, wnames);

    tx_len = os_p9_build_twalk(g_p9_tx_buf, 0, 0, 1, wnames, nwname);
    if (!p9_rpc(&vq, tx_len, &rx_len, "Twalk", err_msg, err_msg_cap)) {
        return 0;
    }
    UINT16 nwqid;
    if (!os_p9_parse_rwalk(g_p9_rx_buf, rx_len, &nwqid)) {
        set_err(err_msg, err_msg_cap, "Twalk: malformed Rwalk response");
        return 0;
    }
    if (nwqid != nwname) {
        set_err(err_msg, err_msg_cap, "Twalk: path component not found (partial walk, check -fsdev path=)");
        return 0;
    }

    /* Topen: newfid=1を読み込み専用でopenする */
    tx_len = os_p9_build_topen(g_p9_tx_buf, 0, 1, P9_OREAD);
    if (!p9_rpc(&vq, tx_len, &rx_len, "Topen", err_msg, err_msg_cap)) {
        return 0;
    }
    if (!os_p9_parse_ropen(g_p9_rx_buf, rx_len)) {
        set_err(err_msg, err_msg_cap, "Topen: malformed Ropen response");
        return 0;
    }

    /* Tread: count=0(EOF)になるかresult_bufが満たされるまで読み続ける */
    UINT64 offset = 0;
    UINT32 total = 0;
    UINT32 read_chunk = P9_MSIZE - 64; /* Rreadヘッダ分の余裕 */

    for (;;) {
        UINT32 remaining_cap = result_cap - total;
        if (remaining_cap == 0) {
            break;
        }
        UINT32 want = (read_chunk < remaining_cap) ? read_chunk : remaining_cap;

        tx_len = os_p9_build_tread(g_p9_tx_buf, 0, 1, offset, want);
        if (!p9_rpc(&vq, tx_len, &rx_len, "Tread", err_msg, err_msg_cap)) {
            return 0;
        }

        const UINT8 *data;
        UINT32 count;
        if (!os_p9_parse_rread(g_p9_rx_buf, rx_len, &data, &count)) {
            set_err(err_msg, err_msg_cap, "Tread: malformed Rread response");
            return 0;
        }
        if (count == 0) {
            break;
        }

        for (UINT32 i = 0; i < count; i++) {
            result_buf[total + i] = data[i];
        }
        total += count;
        offset += count;
    }

    *out_len = total;
    return 1;
}

static void write_u32_decimal(frame_buffer *fb, UINT32 value) {
    char digits[11];
    UINT32 i = 0;

    if (value == 0) {
        fb->write_char(fb, '0');
        return;
    }

    while (value > 0) {
        digits[i] = (char)('0' + (value % 10));
        i++;
        value /= 10;
    }
    while (i > 0) {
        i--;
        fb->write_char(fb, digits[i]);
    }
}

void os_virtio9p_test_run(frame_buffer *fb) {
    fb->write_string(fb, "virtio9p: loading src/lisp/init.lisp ...\n");

    UINT32 out_len = 0;
    char err_msg[160];
    err_msg[0] = '\0';

    int ok = os_virtio9p_load_file("src/lisp/init.lisp", g_init_lisp_buf, sizeof(g_init_lisp_buf),
                                    &out_len, err_msg, sizeof(err_msg));

    if (!ok) {
        fb->write_string(fb, "virtio9p: FAILED: ");
        fb->write_string(fb, err_msg);
        fb->write_char(fb, '\n');
        for (;;) {
        }
    }

    fb->write_string(fb, "virtio9p: OK, ");
    write_u32_decimal(fb, out_len);
    fb->write_string(fb, " bytes:\n---\n");
    for (UINT32 i = 0; i < out_len; i++) {
        fb->write_char(fb, g_init_lisp_buf[i]);
    }
    fb->write_string(fb, "\n---\n");
}
