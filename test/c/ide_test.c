#include <stdint.h>
#include <stdlib.h>
#include "test_assert.h"
#include "types.h"
#include "runtime.h"
#include "framebuffer.h"
#include "lisp.h"
#include "process.h"
#include "reader.h"
#include "drivers/ide.h"
#include "block_device.h"
#include "ide_subprimitive.h"

// reader.c/stream.c/process.cのリンクに必要なダミー実装群。
// subprimitive_test.cと同じ方針: このテストが実際に呼ばない経路は中身を使わない
int os_virtio9p_open(const char *path, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path; (void)mode; (void)out_fid; (void)err_msg; (void)err_msg_cap;
    return 0;
}

int os_virtio9p_create(const char *path, UINT32 perm, UINT8 mode, UINT32 *out_fid, char *err_msg, UINT32 err_msg_cap) {
    (void)path; (void)perm; (void)mode; (void)out_fid; (void)err_msg; (void)err_msg_cap;
    return 0;
}

int os_virtio9p_write_chunk(UINT32 fid, UINT64 offset, const UINT8 *data, UINT32 count,
                             UINT32 *out_written, char *err_msg, UINT32 err_msg_cap) {
    (void)fid; (void)offset; (void)data; (void)count; (void)out_written; (void)err_msg; (void)err_msg_cap;
    return 0;
}

int os_virtio9p_read_chunk(UINT32 fid, UINT64 offset, UINT32 want,
                            const UINT8 **out_data, UINT32 *out_count,
                            char *err_msg, UINT32 err_msg_cap) {
    (void)fid; (void)offset; (void)want; (void)out_data; (void)out_count; (void)err_msg; (void)err_msg_cap;
    return 0;
}

int os_virtio9p_close(UINT32 fid, char *err_msg, UINT32 err_msg_cap) {
    (void)fid; (void)err_msg; (void)err_msg_cap;
    return 0;
}

void os_wait_for_more_input(process_t *proc) {
    (void)proc;
}

static void dummy_write_string(struct _frame_buffer *self, const char *s) {
    (void)self; (void)s;
}

static frame_buffer g_frame_buffer = {
    .write_string = dummy_write_string,
};

frame_buffer* get_active_frame_buffer(void) {
    return &g_frame_buffer;
}

void switch_active_frame_buffer(UINT32 index) {
    (void)index;
}

void enable_timer_irq(void) {
}

static UINT8 g_fake_fpu_default_state[512] __attribute__((aligned(16)));

const void *get_fpu_default_state(void) {
    return g_fake_fpu_default_state;
}

void os_repl_step(process_t *proc) {
    (void)proc;
}

// interrupt.cはリンクしない(実I/O命令はユーザ空間で実行できない)。このテスト
// ファイルがSecondary IDEチャネル相当のfakeレジスタを提供し、outb/inb/outw/inwへの
// 呼び出し順序・値を記録する。block_device.cが使うポート定数と揃える必要があるため
// FAKE_IO_BASE/FAKE_CTRL_BASEは0x170/0x376に固定する
#define FAKE_IO_BASE   0x170
#define FAKE_CTRL_BASE 0x376

static UINT8 g_status_queue[64];
static UINT32 g_status_queue_len = 0;
static UINT32 g_status_queue_pos = 0;

static UINT8 g_reg_lba_mid_in = 0;
static UINT8 g_reg_lba_high_in = 0;

static UINT16 g_data_in[4096];
static UINT32 g_data_in_pos = 0;
static UINT16 g_data_out[4096];
static UINT32 g_data_out_pos = 0;

static UINT8 g_out_drive_head = 0;
static UINT8 g_out_seccount = 0;
static UINT8 g_out_lba_low = 0;
static UINT8 g_out_lba_mid = 0;
static UINT8 g_out_lba_high = 0;
static UINT8 g_out_command = 0;
static UINT8 g_ctrl_value = 0;

static void fake_reset(void) {
    g_status_queue_len = 0;
    g_status_queue_pos = 0;
    g_reg_lba_mid_in = 0;
    g_reg_lba_high_in = 0;
    g_data_in_pos = 0;
    g_data_out_pos = 0;
    for (UINT32 i = 0; i < 4096; i++) {
        g_data_in[i] = 0;
        g_data_out[i] = 0;
    }
}

static void fake_push_status(UINT8 s) {
    g_status_queue[g_status_queue_len++] = s;
}

void outb(uint16_t port, uint8_t val) {
    if (port == FAKE_CTRL_BASE) {
        g_ctrl_value = val;
        return;
    }
    switch (port - FAKE_IO_BASE) {
        case 2: g_out_seccount = val; break;
        case 3: g_out_lba_low = val; break;
        case 4: g_out_lba_mid = val; break;
        case 5: g_out_lba_high = val; break;
        case 6: g_out_drive_head = val; break;
        case 7: g_out_command = val; break;
        default: break;
    }
}

uint8_t inb(uint16_t port) {
    UINT32 offset = port - FAKE_IO_BASE;
    if (offset == 7) {
        if (g_status_queue_pos < g_status_queue_len) {
            return g_status_queue[g_status_queue_pos++];
        }
        return g_status_queue_len > 0 ? g_status_queue[g_status_queue_len - 1] : 0;
    }
    if (offset == 4) {
        return g_reg_lba_mid_in;
    }
    if (offset == 5) {
        return g_reg_lba_high_in;
    }
    return 0;
}

void outw(uint16_t port, uint16_t val) {
    (void)port;
    g_data_out[g_data_out_pos++] = val;
}

uint16_t inw(uint16_t port) {
    (void)port;
    return g_data_in[g_data_in_pos++];
}

#define HEAP_SIZE (1024 * 1024)

static void setup_heap() {
    void *heap = malloc(HEAP_SIZE);
    assert(heap != NULL, "1MBのヒープ用メモリをmallocで確保できる");
    os_heap_init((UINT64)heap, HEAP_SIZE);
    os_bootstrap();
}

static lisp_val_t make_args(int argc, lisp_val_t *args) {
    lisp_val_t list = nil;
    for (int i = argc - 1; i >= 0; i--) {
        list = os_make_cons(args[i], list);
    }
    return list;
}

void test_os_ide_identify_success() {
    fake_reset();
    fake_push_status(0x80); // 初回チェック: floating busではない
    fake_push_status(0x80); // wait_drq 1周目: BSYがまだ立っている
    fake_push_status(0x08); // wait_drq 2周目: BSYクリア+DRQセット
    g_data_in[60] = 100;
    g_data_in[61] = 0;

    os_ide_device dev;
    char err[128];
    int ok = os_ide_identify(&dev, FAKE_IO_BASE, FAKE_CTRL_BASE, 0, err, sizeof(err));

    assert(ok == 1, "os_ide_identifyはPATA HDD検出時に1を返す");
    assert(dev.present == 1, "os_ide_identify成功時はpresentが1になる");
    assert(dev.total_sectors == 100, "os_ide_identifyはword60-61からLBA28の総セクタ数を得る");
    assert(dev.io_base == FAKE_IO_BASE, "os_ide_identifyはio_baseを保持する");
    assert(dev.drive_select == 0xE0, "os_ide_identifyはmaster選択時drive_selectを0xE0にする");
    assert(g_out_drive_head == 0xE0, "os_ide_identifyはdrive/headレジスタへ0xE0を書く");
    assert(g_out_command == 0xEC, "os_ide_identifyはIDENTIFY DEVICE(0xEC)を発行する");
    assert((g_ctrl_value & 0x02) == 0x02, "os_ide_identifyはcontrolレジスタのnIENビットをセットする");
}

void test_os_ide_identify_no_device() {
    fake_reset();
    fake_push_status(0x00); // floating bus

    os_ide_device dev;
    char err[128];
    int ok = os_ide_identify(&dev, FAKE_IO_BASE, FAKE_CTRL_BASE, 0, err, sizeof(err));

    assert(ok == 0, "os_ide_identifyはfloating bus(status=0x00)時に0を返す");
    assert(dev.present == 0, "デバイス非搭載時はpresentが0のままになる");
}

void test_os_ide_identify_atapi_out_of_scope() {
    fake_reset();
    fake_push_status(0x80); // floating busではない
    g_reg_lba_mid_in = 0x14;
    g_reg_lba_high_in = 0xEB; // ATAPI signature

    os_ide_device dev;
    char err[128];
    int ok = os_ide_identify(&dev, FAKE_IO_BASE, FAKE_CTRL_BASE, 0, err, sizeof(err));

    assert(ok == 0, "os_ide_identifyはATAPI signature検出時に0を返す(スコープ外)");
}

void test_os_ide_read_sectors_success() {
    fake_reset();
    fake_push_status(0x00); // wait_not_busy: 最初からBSYクリア
    fake_push_status(0x08); // wait_drq: BSYクリア+DRQセット
    for (int i = 0; i < 256; i++) {
        g_data_in[i] = (UINT16)(i + 1);
    }

    os_ide_device dev = { .io_base = FAKE_IO_BASE, .ctrl_base = FAKE_CTRL_BASE, .drive_select = 0xE0, .total_sectors = 100, .present = 1 };
    UINT8 buf[512];
    char err[128];
    int ok = os_ide_read_sectors(&dev, 5, 1, buf, err, sizeof(err));

    assert(ok == 1, "os_ide_read_sectorsは成功時1を返す");
    assert(g_out_lba_low == 5, "os_ide_read_sectorsはLBA_LOWへlbaの下位8bitを書く");
    assert(g_out_lba_mid == 0, "os_ide_read_sectorsはLBA_MIDへlbaの次の8bitを書く");
    assert(g_out_lba_high == 0, "os_ide_read_sectorsはLBA_HIGHへlbaの次の8bitを書く");
    assert(g_out_seccount == 1, "os_ide_read_sectorsはSECCOUNTへ転送セクタ数を書く");
    assert(g_out_command == 0x20, "os_ide_read_sectorsはREAD SECTORS(0x20)を発行する");

    UINT16 *words = (UINT16 *)buf;
    int mismatch = 0;
    for (int i = 0; i < 256; i++) {
        if (words[i] != (UINT16)(i + 1)) {
            mismatch = 1;
        }
    }
    assert(mismatch == 0, "os_ide_read_sectorsはDATAレジスタから読んだ256wordをbufへ順に格納する");
}

void test_os_ide_read_sectors_error_bit() {
    fake_reset();
    fake_push_status(0x00); // wait_not_busy: pass
    fake_push_status(0x21); // wait_drq: ERRビットが立っている(BSYクリア)

    os_ide_device dev = { .io_base = FAKE_IO_BASE, .ctrl_base = FAKE_CTRL_BASE, .drive_select = 0xE0, .total_sectors = 100, .present = 1 };
    UINT8 buf[512];
    char err[128];
    int ok = os_ide_read_sectors(&dev, 0, 1, buf, err, sizeof(err));

    assert(ok == 0, "os_ide_read_sectorsはERRビット検出時に0を返す");
}

void test_os_ide_read_sectors_not_present() {
    fake_reset();
    os_ide_device dev = { .io_base = FAKE_IO_BASE, .ctrl_base = FAKE_CTRL_BASE, .drive_select = 0xE0, .total_sectors = 0, .present = 0 };
    UINT8 buf[512];
    char err[128];
    int ok = os_ide_read_sectors(&dev, 0, 1, buf, err, sizeof(err));

    assert(ok == 0, "os_ide_read_sectorsはpresent=0のデバイスに対し0を返す");
}

void test_os_ide_write_sectors_success() {
    fake_reset();
    fake_push_status(0x00); // wait_not_busy(書き込み開始前): pass
    fake_push_status(0x08); // wait_drq: BSYクリア+DRQセット
    fake_push_status(0x00); // wait_not_busy(CACHE FLUSH後): pass

    os_ide_device dev = { .io_base = FAKE_IO_BASE, .ctrl_base = FAKE_CTRL_BASE, .drive_select = 0xE0, .total_sectors = 100, .present = 1 };
    UINT8 buf[512];
    UINT16 *words = (UINT16 *)buf;
    for (int i = 0; i < 256; i++) {
        words[i] = (UINT16)(0x1000 + i);
    }
    char err[128];
    int ok = os_ide_write_sectors(&dev, 9, 1, buf, err, sizeof(err));

    assert(ok == 1, "os_ide_write_sectorsは成功時1を返す");
    assert(g_out_lba_low == 9, "os_ide_write_sectorsはLBA_LOWへlbaの下位8bitを書く");
    assert(g_out_command == 0xE7, "os_ide_write_sectorsは最後にCACHE FLUSH(0xE7)を発行する");

    int mismatch = 0;
    for (int i = 0; i < 256; i++) {
        if (g_data_out[i] != (UINT16)(0x1000 + i)) {
            mismatch = 1;
        }
    }
    assert(mismatch == 0, "os_ide_write_sectorsはbufの256wordをDATAレジスタへ順に書き出す");
}

void test_os_ide_write_sectors_busy_timeout() {
    fake_reset();
    fake_push_status(0x80); // wait_not_busyがBSYのまま返り続け、スピンキャップに達する

    os_ide_device dev = { .io_base = FAKE_IO_BASE, .ctrl_base = FAKE_CTRL_BASE, .drive_select = 0xE0, .total_sectors = 100, .present = 1 };
    UINT8 buf[512];
    char err[128];
    int ok = os_ide_write_sectors(&dev, 0, 1, buf, err, sizeof(err));

    assert(ok == 0, "os_ide_write_sectorsはBSYスピンキャップ超過時に0を返す(ハングしない)");
}

void test_cc_ide_init_and_singleton() {
    // block_device.cのos_block_device_ide_instanceはSecondaryチャネル
    // (0x170/0x376)を対象にした遅延初期化シングルトンで、初回呼び出し時にのみ
    // IDENTIFYを実行する。以後このプロセス内では結果がキャッシュされ続けるため、
    // 他のcc_ide_*テストより先に一度だけ成功パターンで初期化させる
    fake_reset();
    fake_push_status(0x80);
    fake_push_status(0x08);
    g_data_in[60] = 200;
    g_data_in[61] = 0;

    lisp_val_t result = cc_ide_init(nil, nil);

    assert(result != nil, "cc_ide_initはIDENTIFY成功時にnil以外を返す");
    assert((result & TAG_MASK) == TAG_RAW_POINTER, "cc_ide_initはblock_device_t*をTAG_RAW_POINTER付きで返す");

    block_device_t *dev = (block_device_t *)(lisp_addr_t)(result & ~TAG_MASK);
    assert(dev->total_sectors == 200, "cc_ide_initが返すblock_device_tのtotal_sectorsはIDENTIFYの結果と一致する");

    lisp_val_t args[1] = { result };
    lisp_val_t total = cc_ide_total_sectors(make_args(1, args), nil);
    assert(total == os_make_fixnum(200), "cc_ide_total_sectorsはtotal_sectorsをfixnumで返す");

    lisp_val_t addr = cc_ide_sector_buffer_address(make_args(1, args), nil);
    assert((addr & TAG_MASK) == TAG_FIXNUM, "cc_ide_sector_buffer_addressは素のFIXNUMを返す(TAG_RAW_POINTERではない)");
    assert((UINT8 *)(lisp_addr_t)(addr >> 3) == os_block_device_ide_sector_buffer(), "cc_ide_sector_buffer_addressは共有バッファの先頭アドレスを返す");

    fake_reset();
    fake_push_status(0x00);
    fake_push_status(0x08);
    for (int i = 0; i < 256; i++) {
        g_data_in[i] = (UINT16)(0x2000 + i);
    }
    lisp_val_t read_args[2] = { result, os_make_fixnum(3) };
    lisp_val_t read_ok = cc_ide_read_sector(make_args(2, read_args), nil);
    assert(read_ok == g_sym_t, "cc_ide_read_sectorは成功時tを返す");

    UINT16 *buf_words = (UINT16 *)os_block_device_ide_sector_buffer();
    int mismatch = 0;
    for (int i = 0; i < 256; i++) {
        if (buf_words[i] != (UINT16)(0x2000 + i)) {
            mismatch = 1;
        }
    }
    assert(mismatch == 0, "cc_ide_read_sectorは共有バッファへ読み込んだセクタの内容を格納する");

    fake_reset();
    fake_push_status(0x21); // ERRビット -> 読み込み失敗
    lisp_val_t read_fail_args[2] = { result, os_make_fixnum(4) };
    lisp_val_t read_fail = cc_ide_read_sector(make_args(2, read_fail_args), nil);
    assert(read_fail == nil, "cc_ide_read_sectorは失敗時nilを返す");

    fake_reset();
    fake_push_status(0x00);
    fake_push_status(0x08);
    fake_push_status(0x00);
    lisp_val_t write_args[2] = { result, os_make_fixnum(7) };
    lisp_val_t write_ok = cc_ide_write_sector(make_args(2, write_args), nil);
    assert(write_ok == g_sym_t, "cc_ide_write_sectorは成功時tを返す");

    int write_mismatch = 0;
    for (int i = 0; i < 256; i++) {
        if (g_data_out[i] != buf_words[i]) {
            write_mismatch = 1;
        }
    }
    assert(write_mismatch == 0, "cc_ide_write_sectorは共有バッファの内容をそのままDATAレジスタへ書き出す");
}

void test_os_register_ide_subprimitives() {
    os_register_ide_subprimitives();

    lisp_val_t init_fn = os_get_function(os_make_symbol("%%IDE-INIT"), global_environment);
    lisp_val_t addr_fn = os_get_function(os_make_symbol("%%IDE-SECTOR-BUFFER-ADDRESS"), global_environment);
    lisp_val_t read_fn = os_get_function(os_make_symbol("%%IDE-READ-SECTOR"), global_environment);
    lisp_val_t write_fn = os_get_function(os_make_symbol("%%IDE-WRITE-SECTOR"), global_environment);
    lisp_val_t total_fn = os_get_function(os_make_symbol("%%IDE-TOTAL-SECTORS"), global_environment);

    assert(init_fn != nil, "os_register_ide_subprimitives後は%%IDE-INITがglobal_environmentから引ける");
    assert(addr_fn != nil, "os_register_ide_subprimitives後は%%IDE-SECTOR-BUFFER-ADDRESSがglobal_environmentから引ける");
    assert(read_fn != nil, "os_register_ide_subprimitives後は%%IDE-READ-SECTORがglobal_environmentから引ける");
    assert(write_fn != nil, "os_register_ide_subprimitives後は%%IDE-WRITE-SECTORがglobal_environmentから引ける");
    assert(total_fn != nil, "os_register_ide_subprimitives後は%%IDE-TOTAL-SECTORSがglobal_environmentから引ける");
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    setup_heap();

    test_os_ide_identify_success();
    test_os_ide_identify_no_device();
    test_os_ide_identify_atapi_out_of_scope();
    test_os_ide_read_sectors_success();
    test_os_ide_read_sectors_error_bit();
    test_os_ide_read_sectors_not_present();
    test_os_ide_write_sectors_success();
    test_os_ide_write_sectors_busy_timeout();
    test_cc_ide_init_and_singleton();
    test_os_register_ide_subprimitives();

    return g_test_failed ? 1 : 0;
}
