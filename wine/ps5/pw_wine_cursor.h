/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_CURSOR_H
#define PW_WINE_CURSOR_H
#include <stdint.h>
/* The server alone publishes the selected input desktop's canonical position.
 * Native display readers copy one coherent pair, never server object pointers.
 * Packing also prevents mixed coordinates while an application moves the mouse.
 * The 64-bit server/native owner uses lock-free aligned 64-bit atomics. */
typedef struct PwWineCursor { uint64_t position; } PwWineCursor;
static inline void pw_wine_cursor_store(PwWineCursor *cursor,int32_t x,int32_t y)
{
    uint64_t pair=(uint32_t)x | ((uint64_t)(uint32_t)y<<32);
    __atomic_store_n(&cursor->position,pair,__ATOMIC_RELEASE);
}
static inline void pw_wine_cursor_read(const PwWineCursor *cursor,int *x,int *y)
{
    uint64_t pair=__atomic_load_n(&cursor->position,__ATOMIC_ACQUIRE);
    *x=(int32_t)(uint32_t)pair;
    *y=(int32_t)(uint32_t)(pair>>32);
}
#endif
