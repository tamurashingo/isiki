#include <stdio.h>
#include <string.h>
#include "test_assert.h"
#include "types.h"
#include "p9.h"
#include "p9_transport.h"
#include "transport_virtio9p.h"
#include "virtio9p.h"

/*
 * virtio9p.cはos_transport_virtio9p_instance()を直接呼ぶ実装になっているため、
 * 実機のVirtIOデバイスを使わずにos_virtio9p_*のRPCシーケンスを検証するには、
 * このシングルトンをテスト側のfake実装で置き換える(リンク時に本物のtransport_virtio9p.c
 * を含めず、この関数をここで定義することで差し替える)。
 *
 * fakeはTwalk/Topen/Tcreate/Tread/Twrite/Tclunk等を「送られてきたリクエストの
 * パラメータをそのまま反映した成功応答」で返す汎用サーバとして振る舞う。
 * 加えて、送られてきた各リクエストの生バイト列をg_call_bufに記録し、
 * テストからそのリクエストの内容(fid/offset/count/nameなど)を検証できるようにする。
 */

#define MAX_CALLS 32
static UINT8 g_call_buf[MAX_CALLS][512];
static UINT32 g_call_len[MAX_CALLS];
static int g_call_count = 0;

static const UINT8 *g_fake_read_data = 0;
static UINT32 g_fake_read_data_len = 0;

static void reset_calls(void) {
    g_call_count = 0;
}

static UINT32 rd_u32(const UINT8 *b, UINT32 off) {
    return (UINT32)b[off] | ((UINT32)b[off + 1] << 8) | ((UINT32)b[off + 2] << 16) | ((UINT32)b[off + 3] << 24);
}

static UINT16 rd_u16(const UINT8 *b, UINT32 off) {
    return (UINT16)b[off] | ((UINT16)b[off + 1] << 8);
}

static void wr_u32(UINT8 *b, UINT32 off, UINT32 v) {
    b[off + 0] = (UINT8)(v & 0xFF);
    b[off + 1] = (UINT8)((v >> 8) & 0xFF);
    b[off + 2] = (UINT8)((v >> 16) & 0xFF);
    b[off + 3] = (UINT8)((v >> 24) & 0xFF);
}

static void wr_u16(UINT8 *b, UINT32 off, UINT16 v) {
    b[off + 0] = (UINT8)(v & 0xFF);
    b[off + 1] = (UINT8)((v >> 8) & 0xFF);
}

static void wr_header(UINT8 *b, UINT32 total_size, UINT8 type, UINT16 tag) {
    wr_u32(b, 0, total_size);
    b[4] = type;
    wr_u16(b, 5, tag);
}

static UINT32 build_qid_zero(UINT8 *buf, UINT32 pos) {
    memset(buf + pos, 0, P9_QID_SIZE);
    return pos + P9_QID_SIZE;
}

static UINT32 build_rversion(UINT8 *buf, UINT16 tag) {
    UINT32 pos = 7;
    wr_u32(buf, pos, 8192);
    pos += 4;
    const char *ver = "9P2000";
    UINT16 vlen = (UINT16)strlen(ver);
    wr_u16(buf, pos, vlen);
    pos += 2;
    memcpy(buf + pos, ver, vlen);
    pos += vlen;
    wr_header(buf, pos, P9_RVERSION, tag);
    return pos;
}

static UINT32 build_rattach(UINT8 *buf, UINT16 tag) {
    UINT32 pos = build_qid_zero(buf, 7);
    wr_header(buf, pos, P9_RATTACH, tag);
    return pos;
}

static UINT32 build_rwalk(UINT8 *buf, UINT16 tag, UINT16 nwqid) {
    UINT32 pos = 7;
    wr_u16(buf, pos, nwqid);
    pos += 2;
    for (UINT16 i = 0; i < nwqid; i++) {
        pos = build_qid_zero(buf, pos);
    }
    wr_header(buf, pos, P9_RWALK, tag);
    return pos;
}

static UINT32 build_ropen(UINT8 *buf, UINT16 tag) {
    UINT32 pos = build_qid_zero(buf, 7);
    wr_u32(buf, pos, 8192);
    pos += 4;
    wr_header(buf, pos, P9_ROPEN, tag);
    return pos;
}

static UINT32 build_rcreate(UINT8 *buf, UINT16 tag) {
    UINT32 pos = build_qid_zero(buf, 7);
    wr_u32(buf, pos, 8192);
    pos += 4;
    wr_header(buf, pos, P9_RCREATE, tag);
    return pos;
}

static UINT32 build_rread(UINT8 *buf, UINT16 tag, const UINT8 *data, UINT32 count) {
    UINT32 pos = 7;
    wr_u32(buf, pos, count);
    pos += 4;
    memcpy(buf + pos, data, count);
    pos += count;
    wr_header(buf, pos, P9_RREAD, tag);
    return pos;
}

static UINT32 build_rwrite(UINT8 *buf, UINT16 tag, UINT32 count) {
    UINT32 pos = 7;
    wr_u32(buf, pos, count);
    pos += 4;
    wr_header(buf, pos, P9_RWRITE, tag);
    return pos;
}

static UINT32 build_rclunk(UINT8 *buf, UINT16 tag) {
    wr_header(buf, 7, P9_RCLUNK, tag);
    return 7;
}

static int fake_ensure_ready(p9_transport_t *self, char *err_msg, UINT32 err_msg_cap) {
    (void)self;
    (void)err_msg;
    (void)err_msg_cap;
    return 1;
}

static UINT32 g_fake_rx_len = 0;

static int fake_send(p9_transport_t *self, const UINT8 *tx_buf, UINT32 tx_len,
                      UINT8 *rx_buf, UINT32 rx_cap, char *err_msg, UINT32 err_msg_cap) {
    (void)self;
    (void)rx_cap;
    (void)err_msg;
    (void)err_msg_cap;

    if (g_call_count < MAX_CALLS) {
        memcpy(g_call_buf[g_call_count], tx_buf, tx_len);
        g_call_len[g_call_count] = tx_len;
        g_call_count++;
    }

    UINT8 type = tx_buf[4];
    UINT16 tag = rd_u16(tx_buf, 5);
    UINT32 out_len = 0;
    switch (type) {
        case P9_TVERSION:
            out_len = build_rversion(rx_buf, tag);
            break;
        case P9_TATTACH:
            out_len = build_rattach(rx_buf, tag);
            break;
        case P9_TWALK: {
            UINT16 nwname = rd_u16(tx_buf, 15);
            out_len = build_rwalk(rx_buf, tag, nwname);
            break;
        }
        case P9_TOPEN:
            out_len = build_ropen(rx_buf, tag);
            break;
        case P9_TCREATE:
            out_len = build_rcreate(rx_buf, tag);
            break;
        case P9_TREAD: {
            UINT32 want = rd_u32(tx_buf, 19);
            UINT32 count = want < g_fake_read_data_len ? want : g_fake_read_data_len;
            out_len = build_rread(rx_buf, tag, g_fake_read_data, count);
            break;
        }
        case P9_TWRITE: {
            UINT32 count = rd_u32(tx_buf, 19);
            out_len = build_rwrite(rx_buf, tag, count);
            break;
        }
        case P9_TCLUNK:
            out_len = build_rclunk(rx_buf, tag);
            break;
        default:
            return 0;
    }
    g_fake_rx_len = out_len;
    return 1;
}

static int fake_recv(p9_transport_t *self, UINT32 *out_rx_len, char *err_msg, UINT32 err_msg_cap) {
    (void)self;
    (void)err_msg;
    (void)err_msg_cap;
    *out_rx_len = g_fake_rx_len;
    return 1;
}

static p9_transport_t g_fake_transport = { fake_ensure_ready, fake_send, fake_recv };

p9_transport_t *os_transport_virtio9p_instance(void) {
    return &g_fake_transport;
}

/** g_call_buf[0..g_call_count)の中から指定typeの最初のリクエストを探す。無ければ0 */
static const UINT8 *find_call(UINT8 type) {
    for (int i = 0; i < g_call_count; i++) {
        if (g_call_buf[i][4] == type) {
            return g_call_buf[i];
        }
    }
    return 0;
}

void test_open_allocates_increasing_fids_across_calls() {
    char err[128];
    UINT32 fid1 = 0, fid2 = 0;

    reset_calls();
    assert(os_virtio9p_open("a.txt", P9_OREAD, &fid1, err, sizeof(err)), "1回目のopenが成功する");
    reset_calls();
    assert(os_virtio9p_open("b.txt", P9_OREAD, &fid2, err, sizeof(err)), "2回目のopenが成功する");

    assert(fid1 != fid2, "2回のopenで異なるfidが割り当てられる(fid固定だった旧実装のバグ修正)");
    assert(fid2 == fid1 + 1, "fidはbump方式で単調増加する");
}

void test_write_chunk_sends_correct_twrite_and_returns_written_count() {
    char err[128];
    UINT8 data[4] = { 'A', 'B', 'C', 'D' };
    UINT32 written = 0;

    reset_calls();
    assert(os_virtio9p_write_chunk(42, 100, data, 4, &written, err, sizeof(err)), "write_chunkが成功する");
    assert(written == 4, "書き込んだバイト数がRwriteのcountと一致する");

    const UINT8 *twrite = find_call(P9_TWRITE);
    assert(twrite != 0, "Twriteが送信されている");
    assert(rd_u32(twrite, 7) == 42, "送信されたTwriteのfidが渡した値になっている");
    assert(rd_u32(twrite, 11) == 100, "送信されたTwriteのoffsetが渡した値になっている");
    assert(rd_u32(twrite, 19) == 4, "送信されたTwriteのcountが渡したバイト数になっている");
    assert(memcmp(twrite + 23, data, 4) == 0, "送信されたTwriteのpayloadが渡したデータと一致する");
}

void test_create_with_parent_walks_parent_and_sends_correct_tcreate() {
    char err[128];
    UINT32 fid = 0;

    reset_calls();
    assert(os_virtio9p_create("dir/new.txt", 0644, P9_OWRITE, &fid, err, sizeof(err)), "親ディレクトリありのcreateが成功する");

    const UINT8 *twalk = find_call(P9_TWALK);
    assert(twalk != 0, "Twalkが送信されている");
    UINT16 nwname = rd_u16(twalk, 15);
    assert(nwname == 1, "親ディレクトリ1要素分だけwalkする");
    UINT16 namelen = rd_u16(twalk, 17);
    assert(namelen == 3, "walkする要素名の長さがdirの長さと一致する");
    assert(memcmp(twalk + 19, "dir", 3) == 0, "walkする要素名がdirになっている");

    const UINT8 *tcreate = find_call(P9_TCREATE);
    assert(tcreate != 0, "Tcreateが送信されている");
    UINT16 fnamelen = rd_u16(tcreate, 11);
    assert(fnamelen == 7, "作成するファイル名の長さがnew.txtの長さと一致する");
    assert(memcmp(tcreate + 13, "new.txt", 7) == 0, "作成するファイル名がnew.txtになっている");
    UINT32 perm = rd_u32(tcreate, 13 + 7);
    assert(perm == 0644, "送信されたTcreateのpermが渡した値になっている");
    UINT8 mode = tcreate[13 + 7 + 4];
    assert(mode == P9_OWRITE, "送信されたTcreateのmodeが渡した値になっている");
}

void test_create_without_parent_walks_zero_elements() {
    char err[128];
    UINT32 fid = 0;

    reset_calls();
    assert(os_virtio9p_create("root.txt", 0644, P9_OWRITE, &fid, err, sizeof(err)), "親ディレクトリなしのcreateが成功する");

    const UINT8 *twalk = find_call(P9_TWALK);
    assert(twalk != 0, "親ディレクトリが無くてもfidをクローンするためTwalkが送信されている");
    UINT16 nwname = rd_u16(twalk, 15);
    assert(nwname == 0, "パスに親ディレクトリが無い場合はwalk要素数0(クローンのみ)になる");

    const UINT8 *tcreate = find_call(P9_TCREATE);
    assert(tcreate != 0, "Tcreateが送信されている");
    UINT16 fnamelen = rd_u16(tcreate, 11);
    assert(fnamelen == 8, "作成するファイル名の長さがroot.txtの長さと一致する");
    assert(memcmp(tcreate + 13, "root.txt", 8) == 0, "作成するファイル名がroot.txtになっている");
}

void test_read_chunk_and_close_round_trip() {
    char err[128];
    static const UINT8 fake_data[5] = { 'h', 'e', 'l', 'l', 'o' };
    g_fake_read_data = fake_data;
    g_fake_read_data_len = 5;

    const UINT8 *out_data = 0;
    UINT32 out_count = 0;
    reset_calls();
    assert(os_virtio9p_read_chunk(7, 0, 10, &out_data, &out_count, err, sizeof(err)), "read_chunkが成功する");
    assert(out_count == 5, "実際に読み込んだバイト数がfakeサーバの保持データ長になる");
    assert(memcmp(out_data, "hello", 5) == 0, "読み込んだデータの内容が一致する");

    assert(os_virtio9p_close(7, err, sizeof(err)), "closeが成功する");
    const UINT8 *tclunk = find_call(P9_TCLUNK);
    assert(tclunk != 0, "Tclunkが送信されている");
    assert(rd_u32(tclunk, 7) == 7, "送信されたTclunkのfidが渡した値になっている");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    test_open_allocates_increasing_fids_across_calls();
    test_write_chunk_sends_correct_twrite_and_returns_written_count();
    test_create_with_parent_walks_parent_and_sends_correct_tcreate();
    test_create_without_parent_walks_zero_elements();
    test_read_chunk_and_close_round_trip();

    return g_test_failed ? 1 : 0;
}
