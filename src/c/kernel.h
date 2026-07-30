#ifndef _KERNEL_H_
#define _KERNEL_H_

#include "types.h"

void kernel_main(UINT64 fb_base, UINT32 fb_width, UINT32 fb_height, UINT32 fb_pixels_per_scanline, UINT64 heap_base, UINT64 heap_size);

#endif /* _KERNEL_H_ */

