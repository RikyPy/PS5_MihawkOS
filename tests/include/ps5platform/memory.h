/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host stand-in for the payload SDK's <ps5platform/memory.h> (the same declarations), so a host test can compile
 * console glue that includes it without the SDK's include tree, whose libc headers would replace the host's. */
#ifndef PS5PLATFORM_MEMORY_H
#define PS5PLATFORM_MEMORY_H
#include <stdbool.h>
#include <stdint.h>
struct ps5_memory_stats {
    uint64_t total_bytes, free_bytes, largest_bytes, flexible_bytes;
    unsigned free_ranges;
};
bool ps5_memory_query(struct ps5_memory_stats *stats);
#endif
