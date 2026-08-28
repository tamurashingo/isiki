#include "ide_subprimitive.h"
#include "runtime.h"
#include "lisp.h"
#include "block_device.h"

lisp_val_t cc_ide_init(lisp_val_t args, lisp_val_t env) {
    (void)args;
    (void)env;
    block_device_t *dev = os_block_device_ide_instance();
    if (dev == 0) {
        return nil;
    }
    return ((lisp_val_t)(lisp_addr_t)dev) | TAG_RAW_POINTER;
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
    os_set_function(os_make_symbol("%%IDE-INIT"), os_make_native_function((lisp_addr_t)(void *)cc_ide_init), global_environment);
    os_set_function(os_make_symbol("%%IDE-SECTOR-BUFFER-ADDRESS"), os_make_native_function((lisp_addr_t)(void *)cc_ide_sector_buffer_address), global_environment);
    os_set_function(os_make_symbol("%%IDE-READ-SECTOR"), os_make_native_function((lisp_addr_t)(void *)cc_ide_read_sector), global_environment);
    os_set_function(os_make_symbol("%%IDE-WRITE-SECTOR"), os_make_native_function((lisp_addr_t)(void *)cc_ide_write_sector), global_environment);
    os_set_function(os_make_symbol("%%IDE-TOTAL-SECTORS"), os_make_native_function((lisp_addr_t)(void *)cc_ide_total_sectors), global_environment);
}
