/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_PHYSICAL_SHARED_H
#define PW_PHYSICAL_SHARED_H
/* Payload-only memory object, never an ordinary file. Firmware 10.01 type
 * 0x0c is qualified in physical-shared-contract and physical-shared-transfer:
 * zeroing, growth, shared/fixed offset maps, msync and SCM_RIGHTS work;
 * byte I/O, private maps and shrink do not. Sizing eagerly consumes flexible
 * backing, not ForApp DMEM; aliases did not increase resident FMEM. A fd or
 * mapping retains ownership after immediate unlink. Failure retains neither.
 * Only the payload imports physhm_* from libkernel_sys. PRXs use callbacks.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
extern int physhm_open(const char *, int, unsigned, int);
extern int physhm_unlink(const char *);
static inline int pw_physical_shared_create(size_t bytes)
{
    static unsigned serial;
    if (!bytes || bytes > (size_t)INT64_MAX - 16383) { errno = EINVAL; return -1; }
    const size_t aligned = (bytes + 16383) & ~(size_t)16383;
    char name[64];
    snprintf(name, sizeof(name), "/pw-physical-%d-%u", (int)getpid(),
             __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED));
    int fd = physhm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600, 0x0c);
    if (fd < 0) return -1;
    if (physhm_unlink(name) || ftruncate(fd, (off_t)aligned)) {
        const int saved = errno;
        close(fd);
        physhm_unlink(name); /* Retry cleanup if the first unlink failed. */
        errno = saved;
        return -1;
    }
    /* Every large one is said, with a running total: sizing consumes flexible backing whether or not a page is touched or mapped, and the
     * kernel's resident figure for the process rose by 250 MiB in 2 s at a Chromium GPU process's start (2026-10-12; record removed from the tree). */
    {
        static size_t total;
        const size_t now = __atomic_add_fetch(&total, aligned, __ATOMIC_RELAXED);
        if (aligned >= ((size_t)8 << 20))
            fprintf(stderr, "wine-ps5: physical shared memory made: %zu MiB (all made so far %zu MiB)\n", aligned >> 20, now >> 20);
    }
    static unsigned reported;
    if (!__atomic_exchange_n(&reported, 1, __ATOMIC_RELAXED))
        fprintf(stderr, "wine-ps5: physical shared memory-only backing active, first_bytes=%zu\n", aligned);
    return fd;
}
#endif
