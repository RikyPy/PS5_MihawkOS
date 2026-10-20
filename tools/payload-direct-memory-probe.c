/* SPDX-License-Identifier: GPL-3.0-or-later
 * Direct memory in an independent payload while the bounded test owner runs.
 * Default: one 16 KiB page. mib=N tests at most 2 GiB. wait-owner keeps only
 * one page until the original owner is absent, then verifies and frees it.
 * No budget assignments or kernel data are changed.
 */
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <ps5platform/kernel.h>

extern int sceKernelAllocateMainDirectMemory(size_t, size_t, int, int64_t *);
extern int sceKernelAllocateDirectMemoryForApp(int64_t, int64_t, size_t, size_t, int, int64_t *);
extern int sceKernelGetBudget(int64_t *);
extern int sceKernelGetResidentFmemCount(int, uint64_t *, uint64_t *);

static void census(const char *phase)
{
    uint64_t resident = 0, mapped = 0;
    int fmem = sceKernelGetResidentFmemCount(getpid(), &resident, &mapped);
    printf("direct %s pid=%d fmem_status=%#x resident_kib=%" PRIu64 " mapped_kib=%" PRIu64
           "\n", phase, getpid(), (unsigned)fmem, resident * 16, mapped * 16);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    pid_t owner = 0;
    size_t bytes = 16384;
    int wait_owner = 0, app = 0;
    for (int i = 0; i < argc; ++i) {
        if (!strncmp(argv[i], "owner=", 6)) owner = (pid_t)atoi(argv[i] + 6);
        if (!strcmp(argv[i], "wait-owner")) wait_owner = 1;
        if (!strcmp(argv[i], "app")) app = 1;
        if (!strncmp(argv[i], "mib=", 4)) {
            char *end;
            unsigned long mib = strtoul(argv[i] + 4, &end, 10);
            if (*end || !mib || mib > 2048) return 1;
            bytes = (size_t)mib << 20;
        }
    }
    if (owner <= 0 || kill(owner, 0) || (wait_owner && bytes != 16384)) return 1;
    int64_t budget = -1;
    int budget_status = sceKernelGetBudget(&budget);
    printf("direct start owner=%d budget=%" PRId64 " budget_status=%#x app=%d pool_mib=%" PRId64 " bytes=%zu\n",
           owner, budget, (unsigned)budget_status, app, sceKernelGetDirectMemorySize() >> 20, bytes);
    census("before");
    int64_t offset = -1;
    void *address = NULL;
    int allocation = app ? sceKernelAllocateDirectMemoryForApp(0, sceKernelGetDirectMemorySize(), bytes, 16384, 0x0c, &offset)
                         : sceKernelAllocateMainDirectMemory(bytes, 16384, 0x0c, &offset);
    int mapping = allocation ? -1 : sceKernelMapDirectMemory(&address, bytes, 3, 0, offset, 16384);
    printf("direct allocation=%#x map=%#x offset=%" PRId64 "\n",
           (unsigned)allocation, (unsigned)mapping, offset);
    fflush(stdout);
    int contents = 0, owner_gone = !wait_owner, unmap = -1, release = -1;
    if (!mapping) {
        volatile unsigned char *p = address;
        for (size_t at = 0; at < bytes; at += 16384) {
            p[at] = (unsigned char)(at / 16384); p[at + 16383] = (unsigned char)~p[at];
        }
        census("touched");
        if (wait_owner) {
            const time_t end = time(NULL) + 150;
            while (time(NULL) < end && !kill(owner, 0)) usleep(100000);
            owner_gone = kill(owner, 0) && errno == ESRCH;
            printf("direct original_owner_absent=%d\n", owner_gone);
            fflush(stdout);
        }
        contents = 1;
        for (size_t at = 0; at < bytes; at += 16384)
            if (p[at] != (unsigned char)(at / 16384) || p[at + 16383] != (unsigned char)~p[at]) contents = 0;
        unmap = sceKernelMunmap(address, bytes);
    }
    if (!allocation) release = sceKernelReleaseDirectMemory(offset, bytes);
    census("released");
    int passed = !allocation && !mapping && contents && owner_gone && !unmap && !release;
    printf("PS5_PROTON_PAYLOAD_DMEM_PROBE %s contents=%d unmap=%#x release=%#x\n",
           passed ? "PASS" : "FAIL", contents, (unsigned)unmap, (unsigned)release);
    fflush(stdout);
    return !passed;
}
