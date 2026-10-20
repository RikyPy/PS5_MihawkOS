/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_wine_blocks.h"
#include <pthread.h>
#include <string.h>

static pthread_mutex_t blocks_lock = PTHREAD_MUTEX_INITIALIZER;
static PwWineBlocksOps blocks_ops;
static struct { uint64_t id; int used; uint64_t done[PW_WINE_BLOCKS_MAX_CHUNKS / 64]; } files[PW_WINE_BLOCKS_FILES];
static unsigned char chunk[PW_WINE_BLOCKS_CHUNK];
static uint64_t written, present, failed;

void pw_wine_blocks_init(const PwWineBlocksOps *ops)
{
    pthread_mutex_lock(&blocks_lock);
    blocks_ops = ops ? *ops : (PwWineBlocksOps){ 0 };
    memset(files, 0, sizeof(files));
    written = present = failed = 0;
    pthread_mutex_unlock(&blocks_lock);
}

static int all_zero(const unsigned char *bytes, size_t n)
{
    const uint64_t *words = (const uint64_t *)bytes;
    for (size_t i = 0; i < n / 8; i++) if (words[i]) return 0;
    return 1;
}

int pw_wine_blocks_ensure(int fd, uint64_t offset, size_t bytes)
{
    uint64_t id;
    int status = 0, slot = -1;

    if (!bytes) return 0;
    pthread_mutex_lock(&blocks_lock);
    if (!blocks_ops.identity || blocks_ops.identity(blocks_ops.context, fd, &id)) goto done;
    for (int i = 0; i < PW_WINE_BLOCKS_FILES; i++)
        if (files[i].used && files[i].id == id) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < PW_WINE_BLOCKS_FILES && slot < 0; i++)
            if (!files[i].used) { slot = i; files[i].used = 1; files[i].id = id; memset(files[i].done, 0, sizeof(files[i].done)); }
    if (slot < 0) { failed++; status = -1; goto done; }
    for (uint64_t c = offset / PW_WINE_BLOCKS_CHUNK; c <= (offset + bytes - 1) / PW_WINE_BLOCKS_CHUNK; c++) {
        if (c >= PW_WINE_BLOCKS_MAX_CHUNKS) { failed++; status = -1; break; }
        if ((files[slot].done[c / 64] >> (c % 64)) & 1) continue;
        const ssize_t got = blocks_ops.read(blocks_ops.context, fd, chunk, sizeof(chunk), c * PW_WINE_BLOCKS_CHUNK);
        if (got < 0) { failed++; status = -1; continue; }
        /* Past the end of the file there is nothing to map; a short read is the file's last chunk. */
        if (got && all_zero(chunk, (size_t)got)) {
            if (blocks_ops.write(blocks_ops.context, fd, chunk, (size_t)got, c * PW_WINE_BLOCKS_CHUNK) != got) {
                failed++; status = -1; continue;
            }
            written++;
        } else {
            present++;
        }
        files[slot].done[c / 64] |= UINT64_C(1) << (c % 64);
    }
done:
    pthread_mutex_unlock(&blocks_lock);
    return status;
}

void pw_wine_blocks_stats(uint64_t *w, uint64_t *p, uint64_t *f)
{
    pthread_mutex_lock(&blocks_lock);
    if (w) *w = written;
    if (p) *p = present;
    if (f) *f = failed;
    pthread_mutex_unlock(&blocks_lock);
}
