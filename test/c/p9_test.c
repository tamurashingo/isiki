#include <stdio.h>
#include <string.h>
#include "test_assert.h"
#include "types.h"
#include "p9.h"

/* p9.cのput系/get系のヘルパーはstaticのため、testからは直接使えない。
   ワイヤーフォーマット(header: size[4]+type[1]+tag[2])の契約を独立に確認するため、
   テスト側でも同じ書式の最小限のread/writeヘルパーを用意する。 */

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

static void wr_header(UINT8 *b, UINT32 total_size, UINT8 type, UINT16 tag) {
    wr_u32(b, 0, total_size);
    b[4] = type;
    b[5] = (UINT8)(tag & 0xFF);
    b[6] = (UINT8)((tag >> 8) & 0xFF);
}

void test_build_twrite_encodes_header_and_payload() {
    UINT8 buf[128];
    UINT8 data[4] = { 'A', 'B', 'C', 'D' };
    UINT32 len = os_p9_build_twrite(buf, 0x1234, 42, 100, data, 4);

    assert(rd_u32(buf, 0) == len, "sizeフィールドが実際に書き込んだバイト数と一致する");
    assert(buf[4] == P9_TWRITE, "typeがTwriteになっている");
    assert(rd_u16(buf, 5) == 0x1234, "tagが渡した値になっている");
    assert(rd_u32(buf, 7) == 42, "fidが渡した値になっている");
    assert(rd_u32(buf, 11) == 100, "offsetの下位32bitが渡した値になっている(offset<2^32のため上位は0)");
    assert(rd_u32(buf, 15) == 0, "offsetの上位32bitが0になっている");
    assert(rd_u32(buf, 19) == 4, "countが渡したバイト数になっている");
    assert(memcmp(buf + 23, data, 4) == 0, "書き込むデータがそのままpayloadに入っている");
}

void test_parse_rwrite_extracts_count() {
    UINT8 buf[32];
    wr_u32(buf, 7, 999);
    wr_header(buf, 11, P9_RWRITE, 5);

    UINT32 count = 0;
    int ok = os_p9_parse_rwrite(buf, 11, &count);
    assert(ok == 1, "正しいRwriteは解析に成功する");
    assert(count == 999, "countが送られてきた値と一致する");
}

void test_parse_rwrite_rejects_wrong_type_and_short_buffer() {
    UINT8 buf[32];
    wr_u32(buf, 7, 999);
    wr_header(buf, 11, P9_RREAD, 5); /* typeが違う */
    UINT32 count = 0;
    assert(os_p9_parse_rwrite(buf, 11, &count) == 0, "typeがRwriteでなければ失敗する");

    wr_header(buf, 11, P9_RWRITE, 5);
    assert(os_p9_parse_rwrite(buf, 8, &count) == 0, "ヘッダ+countに満たない短いバッファは失敗する");
}

void test_build_tcreate_encodes_name_perm_mode() {
    UINT8 buf[128];
    UINT32 len = os_p9_build_tcreate(buf, 7, 3, "foo.txt", 0644, P9_OWRITE);

    assert(buf[4] == P9_TCREATE, "typeがTcreateになっている");
    assert(rd_u16(buf, 5) == 7, "tagが渡した値になっている");
    assert(rd_u32(buf, 7) == 3, "fidが渡した値になっている");

    UINT16 namelen = rd_u16(buf, 11);
    assert(namelen == 7, "nameの長さが渡した文字列の長さと一致する");
    assert(memcmp(buf + 13, "foo.txt", 7) == 0, "nameの内容が渡した文字列と一致する");

    UINT32 perm = rd_u32(buf, 13 + 7);
    assert(perm == 0644, "permが渡した値になっている");
    UINT8 mode = buf[13 + 7 + 4];
    assert(mode == P9_OWRITE, "modeが渡した値になっている");

    /* 9P2000.uではTcreateにmodeの後にextension[s]が付く(QEMUのvirtio-9pサーバはこれを前提にパースする)。
       欠けるとsizeフィールドと実パース長が食い違い、実機相手にのみTcreateが失敗する回帰があったため検証する */
    UINT16 extlen = rd_u16(buf, 13 + 7 + 4 + 1);
    assert(extlen == 0, "9P2000.u拡張のextensionフィールドが空文字列(長さ0)で付与されている");
    assert(len == 13 + 7 + 4 + 1 + 2, "sizeフィールドがextension分まで含めた実際の長さと一致する");
    assert(rd_u32(buf, 0) == len, "sizeフィールドが実際に書き込んだバイト数と一致する");
}

void test_parse_rcreate_accepts_valid_and_rejects_short() {
    UINT8 buf[64];
    memset(buf + 7, 0, P9_QID_SIZE); /* qidの内容は使わないのでゼロ埋めでよい */
    wr_u32(buf, 7 + P9_QID_SIZE, 8192); /* iounit */
    UINT32 total = 7 + P9_QID_SIZE + 4;
    wr_header(buf, total, P9_RCREATE, 9);

    assert(os_p9_parse_rcreate(buf, total) == 1, "正しいRcreateは解析に成功する");
    assert(os_p9_parse_rcreate(buf, total - 1) == 0, "qid+iounitに満たない短いバッファは失敗する");

    wr_header(buf, total, P9_ROPEN, 9); /* typeが違う */
    assert(os_p9_parse_rcreate(buf, total) == 0, "typeがRcreateでなければ失敗する");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    test_build_twrite_encodes_header_and_payload();
    test_parse_rwrite_extracts_count();
    test_parse_rwrite_rejects_wrong_type_and_short_buffer();
    test_build_tcreate_encodes_name_perm_mode();
    test_parse_rcreate_accepts_valid_and_rejects_short();

    return g_test_failed ? 1 : 0;
}
