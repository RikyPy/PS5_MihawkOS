/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_DMEM_SHARE_H
#define PW_DMEM_SHARE_H
/* Direct memory (the application's 11,712 MiB pool) shared between processes, for what the title and a helper both map
 * (the GPU broker's host-visible buffers). A range belongs to the process that allocated it: a second process's
 * sceKernelMapDirectMemory fails with EACCES (0x8002000d) until the owner grants its pid. The system compositor does the
 * same for the games it composes: sceKernelProtectDirectMemoryForPID(offset, length, 0x33, pid), one ioctl (0xc018800f) on
 * the dmem device, read from libkernel_sys 10.01 (docs/PLAN.md, "Direct memory shared between processes").
 * Proven on the console with two payloads and with the title (dmem-share-2026-10-12-05, dmem-title-share-2026-10-12-01).
 *
 * The owner must outlive every mapper: the kernel frees a range when its owner ends. The memory helper owns them (the
 * title ends it by closing its control socket, after the helpers have gone). Only payloads import these calls. */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

extern int sceKernelAllocateDirectMemoryForApp(int64_t, int64_t, size_t, size_t, int, int64_t *);
extern int sceKernelProtectDirectMemoryForPID(int64_t, size_t, int, int);
extern int sceKernelReleaseDirectMemory(int64_t, size_t);
extern int64_t sceKernelGetDirectMemorySize(void);

#define PW_DMEM_SHARE_TYPE 12     /* GPU-visible, as RADV allocates it */
#define PW_DMEM_SHARE_PROT 0x33   /* CPU read/write, GPU read/write */
#define PW_DMEM_SHARE_PAGE 16384u

/* Allocate `bytes` (a multiple of the page size) and grant each pid above zero. 0 with *offset set, or -1 with errno. */
static inline int pw_dmem_share_alloc(uint64_t bytes, int pid_a, int pid_b, int64_t *offset)
{
    int64_t at = -1;
    if (!bytes || bytes % PW_DMEM_SHARE_PAGE || bytes > (uint64_t)INT64_MAX) { errno = EINVAL; return -1; }
    int status = sceKernelAllocateDirectMemoryForApp(0, sceKernelGetDirectMemorySize(), (size_t)bytes, PW_DMEM_SHARE_PAGE,
                                                     PW_DMEM_SHARE_TYPE, &at);
    if (status) { errno = status & 0xffff ? status & 0xffff : ENOMEM; return -1; }
    const int pids[2] = {pid_a, pid_b};
    for (int i = 0; i < 2; ++i) {
        if (pids[i] <= 0) continue;
        status = sceKernelProtectDirectMemoryForPID(at, (size_t)bytes, PW_DMEM_SHARE_PROT, pids[i]);
        if (status) {
            sceKernelReleaseDirectMemory(at, (size_t)bytes);
            errno = status & 0xffff ? status & 0xffff : EACCES;
            return -1;
        }
    }
    *offset = at;
    return 0;
}

/* Grant a range this process owns to one more pid (a second process mapping a shared section's memory). 0, or -1 with errno. */
static inline int pw_dmem_share_grant(int64_t offset, uint64_t bytes, int pid)
{
    if (pid <= 0 || !bytes || bytes % PW_DMEM_SHARE_PAGE || bytes > (uint64_t)SIZE_MAX) { errno = EINVAL; return -1; }
    const int status = sceKernelProtectDirectMemoryForPID(offset, (size_t)bytes, PW_DMEM_SHARE_PROT, pid);
    if (status) { errno = status & 0xffff ? status & 0xffff : EACCES; return -1; }
    return 0;
}

static inline int pw_dmem_share_free(int64_t offset, uint64_t bytes)
{
    const int status = sceKernelReleaseDirectMemory(offset, (size_t)bytes);
    if (status) { errno = status & 0xffff ? status & 0xffff : EINVAL; return -1; }
    return 0;
}
#endif
