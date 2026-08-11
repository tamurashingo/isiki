#ifndef _KERNEL_H_
#define _KERNEL_H_

#include "types.h"

/**
 * カーネルのエントリポイント。ヒープ・仮想バッファ・プロセス・GDT/PIC/PIT/IDTを
 * 初期化し、最後にプロセススケジューラを起動する(戻ってこない)
 * @param fb_base 物理frame bufferのアドレス
 * @param fb_width frame bufferの横幅(pixel)
 * @param fb_height frame bufferの高さ(pixel)
 * @param fb_pixels_per_scanline frame buffer1行あたりのピクセル数
 * @param heap_base Lispヒープの先頭アドレス
 * @param heap_size Lispヒープのサイズ(バイト)
 * @param boot_epoch_seconds UEFI GetTimeで取得した起動時点のUTC
 *        (Universal Time Format: 1900-01-01からの経過秒数。GetTime失敗時は0)
 */
void kernel_main(UINT64 fb_base, UINT32 fb_width, UINT32 fb_height, UINT32 fb_pixels_per_scanline, UINT64 heap_base, UINT64 heap_size, UINT64 boot_epoch_seconds);

/**
 * kernel_mainに渡されたboot_epoch_secondsを返す
 * @return 起動時点のUTC(Universal Time Format)
 */
UINT64 kernel_get_boot_epoch_seconds(void);

#endif /* _KERNEL_H_ */

