#include "virtio9p.h"
#include "transport_virtio9p.h"
#include "p9_transport.h"
#include "p9.h"

/** 9Pセッションで使う最大メッセージサイズ */
#define P9_MSIZE 8192
/** パスの最大コンポーネント数(Twalk用) */
#define P9_MAX_PATH_COMPONENTS 8
/** パス文字列を保持するローカルバッファのサイズ */
#define P9_PATH_BUF_SIZE 256

static UINT8 g_p9_tx_buf[P9_MSIZE] __attribute__((aligned(4096)));
static UINT8 g_p9_rx_buf[P9_MSIZE] __attribute__((aligned(4096)));

/** Tversion/Tattachによる9Pプロトコルレベルのセッション確立が済んでいるか */
static int g_session_ready = 0;

/** 次にopen/createで割り当てるfid。fid=0はroot(Tattach)専用のため1から始める。
    解放済みfidの再利用は行わない(os_alloc_bytes等と同じbump方式) */
static UINT32 g_next_fid = 1;

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

/** g_p9_tx_buf/g_p9_rx_bufでリクエストを送りrecvで完了を待ち、タイムアウト/Rerrorをerr_msgへ整形する */
static int p9_rpc(UINT32 tx_len, UINT32 *out_rx_len, const char *stage,
                   char *err_msg, UINT32 err_msg_cap) {
    p9_transport_t *transport = os_transport_virtio9p_instance();
    char transport_err[96];
    transport_err[0] = '\0';

    if (!transport->send(transport, g_p9_tx_buf, tx_len, g_p9_rx_buf, sizeof(g_p9_rx_buf),
                          transport_err, sizeof(transport_err))) {
        set_err(err_msg, err_msg_cap, stage);
        str_append(err_msg, err_msg_cap, ": ");
        str_append(err_msg, err_msg_cap, transport_err);
        return 0;
    }
    if (!transport->recv(transport, out_rx_len, transport_err, sizeof(transport_err))) {
        set_err(err_msg, err_msg_cap, stage);
        str_append(err_msg, err_msg_cap, ": ");
        str_append(err_msg, err_msg_cap, transport_err);
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

int os_virtio9p_ensure_session(char *err_msg, UINT32 err_msg_cap) {
    if (g_session_ready) {
        return 1;
    }

    p9_transport_t *transport = os_transport_virtio9p_instance();
    if (!transport->ensure_ready(transport, err_msg, err_msg_cap)) {
        return 0;
    }

    UINT32 tx_len;
    UINT32 rx_len;

    /* Tversion: セッション確立前なのでtag=NOTAG(0xFFFF)を使う。
       QEMUのvirtio-9pサーバは素の"9P2000"を受け付けないため"9P2000.u"を使う */
    tx_len = os_p9_build_tversion(g_p9_tx_buf, 0xFFFF, P9_MSIZE, "9P2000.u");
    if (!p9_rpc(tx_len, &rx_len, "Tversion", err_msg, err_msg_cap)) {
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
    if (!p9_rpc(tx_len, &rx_len, "Tattach", err_msg, err_msg_cap)) {
        return 0;
    }
    if (!os_p9_parse_rattach(g_p9_rx_buf, rx_len)) {
        set_err(err_msg, err_msg_cap, "Tattach: malformed Rattach response");
        return 0;
    }

    g_session_ready = 1;
    return 1;
}

int os_virtio9p_open(const char *path, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    if (!os_virtio9p_ensure_session(err_msg, err_msg_cap)) {
        return 0;
    }

    UINT32 tx_len;
    UINT32 rx_len;
    UINT32 newfid = g_next_fid++;

    /* Twalk: fid=0からpathの各要素をnewfidへwalkする */
    char path_buf[P9_PATH_BUF_SIZE];
    str_copy(path_buf, sizeof(path_buf), path);
    const char *wnames[P9_MAX_PATH_COMPONENTS];
    UINT16 nwname = split_path(path_buf, wnames);

    tx_len = os_p9_build_twalk(g_p9_tx_buf, 0, 0, newfid, wnames, nwname);
    if (!p9_rpc(tx_len, &rx_len, "Twalk", err_msg, err_msg_cap)) {
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

    /* Topen: newfidをmodeでopenする */
    tx_len = os_p9_build_topen(g_p9_tx_buf, 0, newfid, mode);
    if (!p9_rpc(tx_len, &rx_len, "Topen", err_msg, err_msg_cap)) {
        return 0;
    }
    if (!os_p9_parse_ropen(g_p9_rx_buf, rx_len)) {
        set_err(err_msg, err_msg_cap, "Topen: malformed Ropen response");
        return 0;
    }

    *out_fid = newfid;
    return 1;
}

int os_virtio9p_create(const char *path, UINT32 perm, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    if (!os_virtio9p_ensure_session(err_msg, err_msg_cap)) {
        return 0;
    }

    UINT32 tx_len;
    UINT32 rx_len;
    UINT32 newfid = g_next_fid++;

    /* Twalk: fid=0からpathの親要素(末尾のファイル名を除く)だけをnewfidへwalkする。
       親要素が無い(ルート直下に作る)場合はnwname=0でfid=0をnewfidへクローンする */
    char path_buf[P9_PATH_BUF_SIZE];
    str_copy(path_buf, sizeof(path_buf), path);
    const char *wnames[P9_MAX_PATH_COMPONENTS];
    UINT16 ncomponents = split_path(path_buf, wnames);
    if (ncomponents == 0) {
        set_err(err_msg, err_msg_cap, "Tcreate: empty path");
        return 0;
    }
    UINT16 nparent = (UINT16)(ncomponents - 1);
    const char *filename = wnames[nparent];

    tx_len = os_p9_build_twalk(g_p9_tx_buf, 0, 0, newfid, wnames, nparent);
    if (!p9_rpc(tx_len, &rx_len, "Twalk", err_msg, err_msg_cap)) {
        return 0;
    }
    UINT16 nwqid;
    if (!os_p9_parse_rwalk(g_p9_rx_buf, rx_len, &nwqid)) {
        set_err(err_msg, err_msg_cap, "Twalk: malformed Rwalk response");
        return 0;
    }
    if (nwqid != nparent) {
        set_err(err_msg, err_msg_cap, "Twalk: parent directory not found (partial walk, check -fsdev path=)");
        return 0;
    }

    /* Tcreate: newfidが指すディレクトリにfilenameを作成する。成功後newfidは新規ファイルを指す */
    tx_len = os_p9_build_tcreate(g_p9_tx_buf, 0, newfid, filename, perm, mode);
    if (!p9_rpc(tx_len, &rx_len, "Tcreate", err_msg, err_msg_cap)) {
        return 0;
    }
    if (!os_p9_parse_rcreate(g_p9_rx_buf, rx_len)) {
        set_err(err_msg, err_msg_cap, "Tcreate: malformed Rcreate response");
        return 0;
    }

    *out_fid = newfid;
    return 1;
}

int os_virtio9p_read_chunk(UINT32 fid, UINT64 offset, UINT32 want,
                            const UINT8 **out_data, UINT32 *out_count,
                            char *err_msg, UINT32 err_msg_cap) {
    UINT32 tx_len = os_p9_build_tread(g_p9_tx_buf, 0, fid, offset, want);
    UINT32 rx_len;
    if (!p9_rpc(tx_len, &rx_len, "Tread", err_msg, err_msg_cap)) {
        return 0;
    }

    if (!os_p9_parse_rread(g_p9_rx_buf, rx_len, out_data, out_count)) {
        set_err(err_msg, err_msg_cap, "Tread: malformed Rread response");
        return 0;
    }
    return 1;
}

int os_virtio9p_write_chunk(UINT32 fid, UINT64 offset, const UINT8 *data, UINT32 count,
                             UINT32 *out_written, char *err_msg, UINT32 err_msg_cap) {
    UINT32 tx_len = os_p9_build_twrite(g_p9_tx_buf, 0, fid, offset, data, count);
    UINT32 rx_len;
    if (!p9_rpc(tx_len, &rx_len, "Twrite", err_msg, err_msg_cap)) {
        return 0;
    }

    if (!os_p9_parse_rwrite(g_p9_rx_buf, rx_len, out_written)) {
        set_err(err_msg, err_msg_cap, "Twrite: malformed Rwrite response");
        return 0;
    }
    return 1;
}

int os_virtio9p_close(UINT32 fid, char *err_msg, UINT32 err_msg_cap) {
    UINT32 tx_len = os_p9_build_tclunk(g_p9_tx_buf, 0, fid);
    UINT32 rx_len;
    if (!p9_rpc(tx_len, &rx_len, "Tclunk", err_msg, err_msg_cap)) {
        return 0;
    }
    if (!os_p9_parse_rclunk(g_p9_rx_buf, rx_len)) {
        set_err(err_msg, err_msg_cap, "Tclunk: malformed Rclunk response");
        return 0;
    }
    return 1;
}

int os_virtio9p_load_file(const char *path, UINT8 *result_buf, UINT32 result_cap,
                           UINT32 *out_len, char *err_msg, UINT32 err_msg_cap) {
    UINT32 fid;
    if (!os_virtio9p_open(path, P9_OREAD, &fid, err_msg, err_msg_cap)) {
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

        const UINT8 *data;
        UINT32 count;
        if (!os_virtio9p_read_chunk(fid, offset, want, &data, &count, err_msg, err_msg_cap)) {
            os_virtio9p_close(fid, 0, 0);
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

    if (!os_virtio9p_close(fid, err_msg, err_msg_cap)) {
        return 0;
    }

    *out_len = total;
    return 1;
}
