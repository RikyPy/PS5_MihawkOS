/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The in-process synchronization device (src/pw_ntsync.c) as the operations
 * Wine's ntsync code calls (its include/wine/ps5_ntsync.h, added by patch
 * 0890). Linked into ntdll.prx; ntdll hands the table to the server. Built
 * with the patched Wine tree's include directory, so a signature that drifts
 * from the header is a compile error. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "wine/ps5_ntsync.h"
#include "../../src/pw_ntsync.h"

_Static_assert(PS5_NTSYNC_DEVICE == PW_NTSYNC_DEVICE, "one device number");
_Static_assert(NTSYNC_MAX_WAIT_COUNT == PW_NTSYNC_MAX_WAIT, "one wait limit");

static int wait(int all, const int *objects, uint32_t count, uint32_t owner, int alert, uint64_t timeout,
                int realtime, uint32_t *index)
{
    PwNtsyncWait wait = {.timeout = timeout, .realtime = realtime, .all = all, .objects = objects,
                         .count = count, .owner = owner, .alert = alert};
    const int status = pw_ntsync_wait(&wait);
    *index = wait.index;
    return status;
}

static const struct ps5_ntsync_ops ops = {
    .create_semaphore = pw_ntsync_create_semaphore,
    .create_mutex = pw_ntsync_create_mutex,
    .create_event = pw_ntsync_create_event,
    .ref = pw_ntsync_ref,
    .release = pw_ntsync_release,
    .semaphore_release = pw_ntsync_semaphore_release,
    .semaphore_read = pw_ntsync_semaphore_read,
    .mutex_unlock = pw_ntsync_mutex_unlock,
    .mutex_kill = pw_ntsync_mutex_kill,
    .mutex_read = pw_ntsync_mutex_read,
    .event_set = pw_ntsync_event_set,
    .event_reset = pw_ntsync_event_reset,
    .event_pulse = pw_ntsync_event_pulse,
    .event_read = pw_ntsync_event_read,
    .wait = wait,
    .note = pw_ntsync_note,
};

/* WINE_PS5_NTSYNC_TRACE keeps the last operations for the dump the title
 * prints when a game does not end (pw_ntsync_dump): 1 for 65536 of them, or
 * a count. */
const struct ps5_ntsync_ops *pw_wine_ntsync_ops(void)
{
    const char *trace = getenv("WINE_PS5_NTSYNC_TRACE");
    const long capacity = trace ? strtol(trace, NULL, 10) : 0;
    if (capacity > 0) pw_ntsync_trace(capacity == 1 ? 65536u : (uint32_t)capacity);
    return &ops;
}

/* A helper's chunks of the objects table, mapped from the region it joined, the first time it reaches each
 * (src/pw_ntsync.h, pw_ntsync_set_chunk_mapper): the census counts what it maps. */
static int chunk_region = -1;
static void *map_chunk(size_t offset, size_t bytes)
{
    void *map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, chunk_region, (off_t)offset);
    if (map != MAP_FAILED) return map;
    fprintf(stderr, "wine-ps5: synchronization device: objects at %zu not mapped (errno %d)\n", offset, errno);
    return NULL;
}

/* The device in this process, over the region of shared memory every process of the session maps
 * (src/pw_ntsync.h): format it (the title, first, which maps it whole) or join it (a helper, which maps the fixed
 * tables and its chunks of objects as it reaches them, and keeps the descriptor for them). NULL, with the descriptor
 * closed, when it cannot be used. */
static const struct ps5_ntsync_ops *attach(int region, int format)
{
    const size_t bytes = format ? pw_ntsync_region_bytes() : pw_ntsync_fixed_bytes();
    void *map = region < 0 ? MAP_FAILED : mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, region, 0);
    int status;
    if (!format && map != MAP_FAILED && chunk_region < 0) {
        chunk_region = region;
        pw_ntsync_set_chunk_mapper(map_chunk);
    }
    status = map == MAP_FAILED ? -errno : pw_ntsync_attach(map, bytes, format);
    if (region >= 0 && region != chunk_region) close(region);
    if (status == -EBUSY) {     /* this process has the device already: keep the one it has */
        munmap(map, bytes);
        return pw_wine_ntsync_ops();
    }
    if (status) {
        if (map != MAP_FAILED) munmap(map, bytes);
        if (region >= 0 && region == chunk_region) {
            pw_ntsync_set_chunk_mapper(NULL);
            close(region);
            chunk_region = -1;
        }
        fprintf(stderr, "wine-ps5: synchronization device unavailable here: %d\n", status);
        return NULL;
    }
    return pw_wine_ntsync_ops();
}

/* The title, before its server starts: the region is the descriptor in WINE_PS5_NTSYNC_REGION, made by the memory
 * helper only when WINE_PS5_NTSYNC=1 (native/proton_wine.cpp); without one every wait goes to the server. */
const struct ps5_ntsync_ops *pw_wine_ntsync_start(void)
{
    const char *region = getenv("WINE_PS5_NTSYNC_REGION");
    if (!region) return NULL;
    return attach(dup(atoi(region)), 1);
}

/* A helper, when the server says its objects are in the device: the region the server sent. */
const struct ps5_ntsync_ops *pw_wine_ntsync_join(int region)
{
    return attach(region, 0);
}
