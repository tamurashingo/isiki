#include "ide_subprimitive.h"
#include "runtime.h"
#include "lisp.h"
#include "block_device.h"
#include "drivers/ide.h"

/**
 * [性能測定] documents/performance-measurement.md参照。os_ide_read_sectors
 * (drivers/ide.c)の実行時アドレスをfixnumとして返す、命令数内訳計測専用の
 * 診断用組み込み関数。ビルド成果物(BOOTX64.EFI)のシンボルテーブルから
 * ファイル上の相対仮想アドレス(RVA)は静的に分かるが、UEFIローダが実際に
 * イメージをロードする実行時ベースアドレスは起動のたびに(あるいは
 * ホスト/QEMU構成によって)変わりうるため、この関数自身のポインタ値を
 * 実行時に取得することで両者を対応付け、-dfilter/TCGプラグインの
 * アドレス範囲指定に使う。診断専用のため、他の組み込み関数と異なり
 * 恒久的な公開APIとしての安定性は保証しない
 */
lisp_val_t cc_diag_ide_read_sectors_addr(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    return os_make_fixnum((UINT64)(lisp_addr_t)(void *)os_ide_read_sectors);
}

lisp_val_t cc_ide_device_count(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    return os_make_fixnum(os_block_device_count());
}

lisp_val_t cc_ide_device_at(lisp_val_t args, lisp_val_t env) {
    (void)env;
    UINT32 index = (UINT32)(cc_car(args) >> 3);
    block_device_t *dev = os_block_device_at(index);
    if (dev == 0) {
        return nil;
    }
    return ((lisp_val_t)(lisp_addr_t)dev) | TAG_RAW_POINTER;
}

lisp_val_t cc_ide_device_name(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t device = cc_car(args);
    block_device_t *dev = (block_device_t *)(lisp_addr_t)(device & ~TAG_MASK);
    return os_make_string(dev->name);
}

lisp_val_t cc_ide_device_model(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t device = cc_car(args);
    block_device_t *dev = (block_device_t *)(lisp_addr_t)(device & ~TAG_MASK);
    return os_make_string(dev->model);
}

lisp_val_t cc_ide_sector_buffer_address(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    return os_make_fixnum((UINT64)(lisp_addr_t)os_block_device_ide_sector_buffer());
}

lisp_val_t cc_ide_read_sector(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t device = cc_car(args);
    UINT32 lba = (UINT32)(cc_car(cc_cdr(args)) >> 3);

    block_device_t *dev = (block_device_t *)(lisp_addr_t)(device & ~TAG_MASK);
    UINT8 *buf = os_block_device_ide_sector_buffer();

    char err_msg[128];
    err_msg[0] = '\0';
    if (!dev->read_sectors(dev, lba, 1, buf, err_msg, sizeof(err_msg))) {
        return nil;
    }
    return g_sym_t;
}

lisp_val_t cc_ide_write_sector(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t device = cc_car(args);
    UINT32 lba = (UINT32)(cc_car(cc_cdr(args)) >> 3);

    block_device_t *dev = (block_device_t *)(lisp_addr_t)(device & ~TAG_MASK);
    UINT8 *buf = os_block_device_ide_sector_buffer();

    char err_msg[128];
    err_msg[0] = '\0';
    if (!dev->write_sectors(dev, lba, 1, buf, err_msg, sizeof(err_msg))) {
        return nil;
    }
    return g_sym_t;
}

lisp_val_t cc_ide_total_sectors(lisp_val_t args, lisp_val_t env) {
    (void)env;
    lisp_val_t device = cc_car(args);
    block_device_t *dev = (block_device_t *)(lisp_addr_t)(device & ~TAG_MASK);
    return os_make_fixnum(dev->total_sectors);
}

void os_register_ide_subprimitives(void) {
    os_set_function(os_make_symbol("%%IDE-DEVICE-COUNT"), os_make_native_function((lisp_addr_t)(void *)cc_ide_device_count), global_environment);
    os_set_function(os_make_symbol("%%IDE-DEVICE-AT"), os_make_native_function((lisp_addr_t)(void *)cc_ide_device_at), global_environment);
    os_set_function(os_make_symbol("%%IDE-DEVICE-NAME"), os_make_native_function((lisp_addr_t)(void *)cc_ide_device_name), global_environment);
    os_set_function(os_make_symbol("%%IDE-DEVICE-MODEL"), os_make_native_function((lisp_addr_t)(void *)cc_ide_device_model), global_environment);
    os_set_function(os_make_symbol("%%IDE-SECTOR-BUFFER-ADDRESS"), os_make_native_function((lisp_addr_t)(void *)cc_ide_sector_buffer_address), global_environment);
    os_set_function(os_make_symbol("%%IDE-READ-SECTOR"), os_make_native_function((lisp_addr_t)(void *)cc_ide_read_sector), global_environment);
    os_set_function(os_make_symbol("%%IDE-WRITE-SECTOR"), os_make_native_function((lisp_addr_t)(void *)cc_ide_write_sector), global_environment);
    os_set_function(os_make_symbol("%%IDE-TOTAL-SECTORS"), os_make_native_function((lisp_addr_t)(void *)cc_ide_total_sectors), global_environment);
    os_set_function(os_make_symbol("%%DIAG-IDE-READ-SECTORS-ADDR"), os_make_native_function((lisp_addr_t)(void *)cc_diag_ide_read_sectors_addr), global_environment);
}
