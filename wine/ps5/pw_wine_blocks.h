/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_BLOCKS_H
#define PW_WINE_BLOCKS_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Real disk blocks under a section file before it is mapped.
 *
 * A large shared section on the console is a sparse file on /data (SHM_ANON refused 13.5 GiB with ENOMEM,
 * section-arena-native-2026-10-08-02), sized with ftruncate. Measured on that filesystem (bfs, docs/PLAN.md, patch
 * 1007): writes through a mapping into a range ftruncate created without blocks are not seen by other readers until
 * msync (305 of 600 appends read back wrong); writing one real byte first, which allocates the block, made it 0 of 600.
 * The section cache maps the same file pages at several addresses (an emulator's guest view and its alias of the whole
 * guest memory), so an incoherent page reads back another alias's stale bytes: tests/test_pw_wine_section_data.c
 * with PW_SECTION_INCOHERENT shows the cache depends on coherent mappings.
 *
 * pw_wine_blocks_ensure gives each 1 MiB chunk real blocks the first time any part of it is about to be mapped in this
 * process: it reads the chunk and, when it reads as all zeros (never written, or a hole), writes those zeros back.
 * A chunk that already holds data has its blocks. Chunks are remembered per file (its identity), so an alias of the
 * same file does not write again. The kernel is reached only through PwWineBlocksOps (host-tested). */

enum { PW_WINE_BLOCKS_CHUNK = 1 << 20, PW_WINE_BLOCKS_FILES = 16, PW_WINE_BLOCKS_MAX_CHUNKS = 65536 /* 64 GiB */ };

typedef struct PwWineBlocksOps {
    void *context;
    /* An identity for the file behind fd (device and inode), or -1 when it is not a regular file (nothing to do). */
    int (*identity)(void *context, int fd, uint64_t *id);
    ssize_t (*read)(void *context, int fd, void *buffer, size_t bytes, uint64_t offset);
    ssize_t (*write)(void *context, int fd, const void *buffer, size_t bytes, uint64_t offset);
} PwWineBlocksOps;

void pw_wine_blocks_init(const PwWineBlocksOps *ops);
/* Before mapping [offset, +bytes) of fd. 0, or -1 when a read or write failed (the caller maps anyway: the old
 * behaviour, and the failure is counted). */
int pw_wine_blocks_ensure(int fd, uint64_t offset, size_t bytes);
/* Chunks given blocks, chunks found holding data, failures. */
void pw_wine_blocks_stats(uint64_t *written, uint64_t *present, uint64_t *failed);

#endif
