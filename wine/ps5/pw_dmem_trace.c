/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What the OpenGL driver takes from the console's direct memory. The 12 GiB pool is
 * shared by the CPU and the GPU side, and the driver (statically linked into win32u)
 * allocates real, zero-filled memory for every GL resource; when a Java game ran the
 * pool out, nothing said who had taken it. win32u is linked with
 * --wrap=sceKernelAllocateDirectMemory and --wrap=sceKernelReleaseDirectMemory
 * (tools/build-wine-ps5.sh), so every refusal and every allocation of 128 MiB or more, with
 * a running total, are logged, with the caller's address (resolve it against the
 * unstripped win32u.shared.elf: caller - this wrapper's address + its symbol). */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

int __real_sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t length, size_t alignment, int type,
                                         int64_t *offset);
int __real_sceKernelReleaseDirectMemory(int64_t start, size_t length);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long total, peak;
static unsigned allocations, releases;

int __wrap_sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t length, size_t alignment, int type,
                                         int64_t *offset)
{
    int rc = __real_sceKernelAllocateDirectMemory(start, end, length, alignment, type, offset);
    int report;

    pthread_mutex_lock(&lock);
    if (!rc) {
        total += length;
        allocations++;
        if (total > peak) peak = total;
    }
    report = rc || length >= (128u << 20); /* a refusal, or an allocation of 128 MiB or more */
    pthread_mutex_unlock(&lock);
    if (report)
        fprintf(stderr, "wine-ps5: gl dmem alloc %zu bytes (align %zu, type %d) rc=%#x; held %llu MiB, peak %llu MiB, "
                "%u allocations, %u releases; caller %p, wrapper %p\n", length, alignment, type, (unsigned)rc,
                total >> 20, peak >> 20, allocations, releases, __builtin_return_address(0),
                (void *)__wrap_sceKernelAllocateDirectMemory);
    return rc;
}

int __wrap_sceKernelReleaseDirectMemory(int64_t start, size_t length)
{
    int rc = __real_sceKernelReleaseDirectMemory(start, length);

    pthread_mutex_lock(&lock);
    if (!rc) {
        total = total >= length ? total - length : 0;
        releases++;
    }
    pthread_mutex_unlock(&lock);
    return rc;
}
