/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_SHARED_PAGE_H
#define PW_SHARED_PAGE_H
/* Memory a helper process shares with the title (its input page, its audio ring) is made by the helper and given to
 * the title, never the other way. The system closes a title while its helpers still run, and memory the title had
 * made and a helper still held stayed charged to the title: deleting the title's budget then logged "VM resource
 * leak: set:1, res:0, amount:0x8000", two 16 KiB pages, and the console panicked
 * (2026-10-06; another run that day ended the same way; records removed from the tree). Every
 * byte is written here, so every page is the helper's before the title maps it. */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef PW_MEMORY_HELPER_PHYSICAL
#include "../native/pw_physical_shared.h"
#endif

/* A shared memory object of bytes, zeroed and faulted in by the caller; its descriptor, or -1. */
static inline int pw_shared_page_create(size_t bytes)
{
    void *map = MAP_FAILED;
    int fd;
#ifdef PW_MEMORY_HELPER_PHYSICAL
    fd = pw_physical_shared_create(bytes);
    if (fd >= 0) return fd; /* Physical sizing is already zero-filled. */
#endif
#ifdef SHM_ANON
    fd = shm_open(SHM_ANON, O_RDWR, 0600);
#else
    /* A host build (the tests): a named object, unlinked at once. */
    static unsigned serial;
    char name[64];
    snprintf(name, sizeof(name), "/pw-shared-%d-%u", (int)getpid(), __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED));
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) shm_unlink(name);
#endif
    if (fd < 0) return -1;
    if (!ftruncate(fd, (off_t)bytes)) map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return -1;
    }
    memset(map, 0, bytes);
    munmap(map, bytes);
    return fd;
}
#endif
