/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_WINDOW_H
#define PW_WINE_WINDOW_H
#include <stdint.h>
#include <stddef.h>
/* Helper-side pages only. Native loader supplies the existing physical-memory
 * allocator. Every new/replacement descriptor needs a bounded title ACK before
 * retiring the preceding page. The title is the independent scene publisher;
 * this module knows no Z order, visibility or input state. */
void pw_wine_window_set_allocator(int (*allocate)(size_t));
int pw_wine_window_present(uint32_t hwnd,uint64_t generation,const void *pixels,
                           uint32_t width,uint32_t height,uint32_t stride,int32_t offset_x,int32_t offset_y);
/* Shape and opacity are producer-owned surface state, committed atomically
 * with each image. Opaque calls retain the fast memcpy path. */
int pw_wine_window_present_surface(uint32_t hwnd,uint64_t generation,const void *pixels,
                           uint32_t width,uint32_t height,uint32_t stride,int32_t offset_x,int32_t offset_y,
                           uint32_t alpha_mask,uint32_t opacity,const void *shape,
                           uint32_t shape_stride,size_t shape_bytes);
void pw_wine_window_drop(uint32_t hwnd);
void pw_wine_window_release(void);
#endif
