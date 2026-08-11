#include "p9.h"

/** 9Pヘッダ(size[4]+type[1]+tag[2])のバイト数 */
#define P9_HEADER_SIZE 7

static UINT32 p9_strlen(const char *s) {
    UINT32 n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void p9_put_u8(UINT8 *buf, UINT32 *pos, UINT8 v) {
    buf[*pos] = v;
    *pos += 1;
}

static void p9_put_u16(UINT8 *buf, UINT32 *pos, UINT16 v) {
    buf[*pos + 0] = (UINT8)(v & 0xFF);
    buf[*pos + 1] = (UINT8)((v >> 8) & 0xFF);
    *pos += 2;
}

static void p9_put_u32(UINT8 *buf, UINT32 *pos, UINT32 v) {
    buf[*pos + 0] = (UINT8)(v & 0xFF);
    buf[*pos + 1] = (UINT8)((v >> 8) & 0xFF);
    buf[*pos + 2] = (UINT8)((v >> 16) & 0xFF);
    buf[*pos + 3] = (UINT8)((v >> 24) & 0xFF);
    *pos += 4;
}

static void p9_put_u64(UINT8 *buf, UINT32 *pos, UINT64 v) {
    for (UINT32 i = 0; i < 8; i++) {
        buf[*pos + i] = (UINT8)((v >> (8 * i)) & 0xFF);
    }
    *pos += 8;
}

static void p9_put_string(UINT8 *buf, UINT32 *pos, const char *s) {
    UINT32 len = p9_strlen(s);
    p9_put_u16(buf, pos, (UINT16)len);
    for (UINT32 i = 0; i < len; i++) {
        buf[*pos + i] = (UINT8)s[i];
    }
    *pos += len;
}

static UINT8 p9_get_u8(const UINT8 *buf, UINT32 *pos) {
    UINT8 v = buf[*pos];
    *pos += 1;
    return v;
}

static UINT16 p9_get_u16(const UINT8 *buf, UINT32 *pos) {
    UINT16 v = (UINT16)buf[*pos] | ((UINT16)buf[*pos + 1] << 8);
    *pos += 2;
    return v;
}

static UINT32 p9_get_u32(const UINT8 *buf, UINT32 *pos) {
    UINT32 v = (UINT32)buf[*pos]
        | ((UINT32)buf[*pos + 1] << 8)
        | ((UINT32)buf[*pos + 2] << 16)
        | ((UINT32)buf[*pos + 3] << 24);
    *pos += 4;
    return v;
}

/** len[2]+bytesの文字列をout(NUL終端, out_cap込み)へコピーし、posを実際の長さ分進める */
static void p9_get_string(const UINT8 *buf, UINT32 *pos, char *out, UINT32 out_cap) {
    UINT16 len = p9_get_u16(buf, pos);
    UINT32 copy_len = len;
    if (out != 0) {
        if (copy_len > out_cap - 1) {
            copy_len = out_cap - 1;
        }
        for (UINT32 i = 0; i < copy_len; i++) {
            out[i] = (char)buf[*pos + i];
        }
        out[copy_len] = '\0';
    }
    *pos += len;
}

static void p9_write_header(UINT8 *buf, UINT32 total_size, UINT8 type, UINT16 tag) {
    UINT32 pos = 0;
    p9_put_u32(buf, &pos, total_size);
    p9_put_u8(buf, &pos, type);
    p9_put_u16(buf, &pos, tag);
}

UINT32 os_p9_build_tversion(UINT8 *buf, UINT16 tag, UINT32 msize, const char *version) {
    UINT32 pos = P9_HEADER_SIZE;
    p9_put_u32(buf, &pos, msize);
    p9_put_string(buf, &pos, version);
    p9_write_header(buf, pos, P9_TVERSION, tag);
    return pos;
}

UINT32 os_p9_build_tattach(UINT8 *buf, UINT16 tag, UINT32 fid, UINT32 afid, const char *uname, const char *aname, UINT32 n_uname) {
    UINT32 pos = P9_HEADER_SIZE;
    p9_put_u32(buf, &pos, fid);
    p9_put_u32(buf, &pos, afid);
    p9_put_string(buf, &pos, uname);
    p9_put_string(buf, &pos, aname);
    p9_put_u32(buf, &pos, n_uname); /* 9P2000.u拡張フィールド */
    p9_write_header(buf, pos, P9_TATTACH, tag);
    return pos;
}

UINT32 os_p9_build_twalk(UINT8 *buf, UINT16 tag, UINT32 fid, UINT32 newfid, const char **wnames, UINT16 nwname) {
    UINT32 pos = P9_HEADER_SIZE;
    p9_put_u32(buf, &pos, fid);
    p9_put_u32(buf, &pos, newfid);
    p9_put_u16(buf, &pos, nwname);
    for (UINT16 i = 0; i < nwname; i++) {
        p9_put_string(buf, &pos, wnames[i]);
    }
    p9_write_header(buf, pos, P9_TWALK, tag);
    return pos;
}

UINT32 os_p9_build_topen(UINT8 *buf, UINT16 tag, UINT32 fid, UINT8 mode) {
    UINT32 pos = P9_HEADER_SIZE;
    p9_put_u32(buf, &pos, fid);
    p9_put_u8(buf, &pos, mode);
    p9_write_header(buf, pos, P9_TOPEN, tag);
    return pos;
}

UINT32 os_p9_build_tread(UINT8 *buf, UINT16 tag, UINT32 fid, UINT64 offset, UINT32 count) {
    UINT32 pos = P9_HEADER_SIZE;
    p9_put_u32(buf, &pos, fid);
    p9_put_u64(buf, &pos, offset);
    p9_put_u32(buf, &pos, count);
    p9_write_header(buf, pos, P9_TREAD, tag);
    return pos;
}

UINT32 os_p9_build_tclunk(UINT8 *buf, UINT16 tag, UINT32 fid) {
    UINT32 pos = P9_HEADER_SIZE;
    p9_put_u32(buf, &pos, fid);
    p9_write_header(buf, pos, P9_TCLUNK, tag);
    return pos;
}

UINT32 os_p9_build_twrite(UINT8 *buf, UINT16 tag, UINT32 fid, UINT64 offset, const UINT8 *data, UINT32 count) {
    UINT32 pos = P9_HEADER_SIZE;
    p9_put_u32(buf, &pos, fid);
    p9_put_u64(buf, &pos, offset);
    p9_put_u32(buf, &pos, count);
    for (UINT32 i = 0; i < count; i++) {
        buf[pos + i] = data[i];
    }
    pos += count;
    p9_write_header(buf, pos, P9_TWRITE, tag);
    return pos;
}

UINT32 os_p9_build_tcreate(UINT8 *buf, UINT16 tag, UINT32 fid, const char *name, UINT32 perm, UINT8 mode) {
    UINT32 pos = P9_HEADER_SIZE;
    p9_put_u32(buf, &pos, fid);
    p9_put_string(buf, &pos, name);
    p9_put_u32(buf, &pos, perm);
    p9_put_u8(buf, &pos, mode);
    /* 9P2000.u拡張フィールド: extension[s](symlink/device等の特殊ファイル用。通常ファイル作成では空文字列。
       セッションは"9P2000.u"で確立しているため、この session実際のサーバ(QEMU)はTcreateにこのフィールドが
       付くことを前提にパースする。欠けるとsizeフィールドと実際に必要なパース長が食い違い、
       実機のvirtio-9pサーバ相手にのみ失敗する(fake transportのテストでは検出できない) */
    p9_put_string(buf, &pos, "");
    p9_write_header(buf, pos, P9_TCREATE, tag);
    return pos;
}

static int p9_check_min_len(UINT32 len, UINT32 required) {
    return len >= required;
}

int os_p9_parse_rversion(const UINT8 *buf, UINT32 len, UINT32 *out_msize, char *out_version, UINT32 version_cap) {
    if (!p9_check_min_len(len, P9_HEADER_SIZE + 4 + 2)) {
        return 0;
    }
    UINT32 pos = 4;
    UINT8 type = p9_get_u8(buf, &pos);
    if (type != P9_RVERSION) {
        return 0;
    }
    pos = P9_HEADER_SIZE;
    *out_msize = p9_get_u32(buf, &pos);
    p9_get_string(buf, &pos, out_version, version_cap);
    return 1;
}

int os_p9_parse_rattach(const UINT8 *buf, UINT32 len) {
    if (!p9_check_min_len(len, P9_HEADER_SIZE + P9_QID_SIZE)) {
        return 0;
    }
    UINT32 pos = 4;
    UINT8 type = p9_get_u8(buf, &pos);
    return type == P9_RATTACH;
}

int os_p9_parse_rwalk(const UINT8 *buf, UINT32 len, UINT16 *out_nwqid) {
    if (!p9_check_min_len(len, P9_HEADER_SIZE + 2)) {
        return 0;
    }
    UINT32 pos = 4;
    UINT8 type = p9_get_u8(buf, &pos);
    if (type != P9_RWALK) {
        return 0;
    }
    pos = P9_HEADER_SIZE;
    *out_nwqid = p9_get_u16(buf, &pos);
    return 1;
}

int os_p9_parse_ropen(const UINT8 *buf, UINT32 len) {
    if (!p9_check_min_len(len, P9_HEADER_SIZE + P9_QID_SIZE + 4)) {
        return 0;
    }
    UINT32 pos = 4;
    UINT8 type = p9_get_u8(buf, &pos);
    return type == P9_ROPEN;
}

int os_p9_parse_rread(const UINT8 *buf, UINT32 len, const UINT8 **out_data, UINT32 *out_count) {
    if (!p9_check_min_len(len, P9_HEADER_SIZE + 4)) {
        return 0;
    }
    UINT32 pos = 4;
    UINT8 type = p9_get_u8(buf, &pos);
    if (type != P9_RREAD) {
        return 0;
    }
    pos = P9_HEADER_SIZE;
    UINT32 count = p9_get_u32(buf, &pos);
    if (!p9_check_min_len(len, P9_HEADER_SIZE + 4 + count)) {
        return 0;
    }
    *out_data = buf + pos;
    *out_count = count;
    return 1;
}

int os_p9_parse_rclunk(const UINT8 *buf, UINT32 len) {
    if (!p9_check_min_len(len, P9_HEADER_SIZE)) {
        return 0;
    }
    UINT32 pos = 4;
    UINT8 type = p9_get_u8(buf, &pos);
    return type == P9_RCLUNK;
}

int os_p9_parse_rwrite(const UINT8 *buf, UINT32 len, UINT32 *out_count) {
    if (!p9_check_min_len(len, P9_HEADER_SIZE + 4)) {
        return 0;
    }
    UINT32 pos = 4;
    UINT8 type = p9_get_u8(buf, &pos);
    if (type != P9_RWRITE) {
        return 0;
    }
    pos = P9_HEADER_SIZE;
    *out_count = p9_get_u32(buf, &pos);
    return 1;
}

int os_p9_parse_rcreate(const UINT8 *buf, UINT32 len) {
    if (!p9_check_min_len(len, P9_HEADER_SIZE + P9_QID_SIZE + 4)) {
        return 0;
    }
    UINT32 pos = 4;
    UINT8 type = p9_get_u8(buf, &pos);
    return type == P9_RCREATE;
}

int os_p9_check_error(const UINT8 *buf, UINT32 len, char *errbuf, UINT32 errbuf_cap) {
    if (!p9_check_min_len(len, P9_HEADER_SIZE + 2)) {
        return 0;
    }
    UINT32 pos = 4;
    UINT8 type = p9_get_u8(buf, &pos);
    if (type != P9_RERROR) {
        return 0;
    }
    pos = P9_HEADER_SIZE;
    p9_get_string(buf, &pos, errbuf, errbuf_cap);
    return 1;
}
