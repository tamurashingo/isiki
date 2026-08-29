#ifndef _IDE_SUBPRIMITIVE_H_
#define _IDE_SUBPRIMITIVE_H_

#include "runtime.h"

/** (%%ide-init) 引数無し。Secondary IDEチャネルをプローブし、成功時block_device_t*をTAG_RAW_POINTER付きで、失敗時nilを返す */
lisp_val_t cc_ide_init(lisp_val_t args, lisp_val_t env);

/** (%%ide-sector-buffer-address device) 共有セクタバッファの先頭アドレスを素のFIXNUMで返す(deviceは未使用、将来のマルチデバイス対応用に引数だけ確保) */
lisp_val_t cc_ide_sector_buffer_address(lisp_val_t args, lisp_val_t env);

/** (%%ide-read-sector device lba) 1セクタ(512byte)を共有バッファへ読み込む。成功時t、失敗時nil */
lisp_val_t cc_ide_read_sector(lisp_val_t args, lisp_val_t env);

/** (%%ide-write-sector device lba) 共有バッファの内容を1セクタ書き込み、CACHE FLUSHまで実行する。成功時t、失敗時nil */
lisp_val_t cc_ide_write_sector(lisp_val_t args, lisp_val_t env);

/** (%%ide-total-sectors device) 総セクタ数をfixnumで返す */
lisp_val_t cc_ide_total_sectors(lisp_val_t args, lisp_val_t env);

/** %%IDE-*をglobal_environmentへネイティブ関数として登録する */
void os_register_ide_subprimitives(void);

#endif /* _IDE_SUBPRIMITIVE_H_ */
