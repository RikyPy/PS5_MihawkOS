/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * docs/SELF_INSTALL.md phase 0, probe 4: whether a loader starts an ELF the size the setup program would be. The
 * blob (pseudo-random bytes made by tools/run-big-elf-probe.py) is linked into .rodata; the payload hashes all of it,
 * says what it found in klog and as a notification, and returns. No file, no thread, no fork(), no /dev/dmem.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

extern const unsigned char big_elf_blob[], big_elf_blob_end[];

struct notification { char unused[45]; char message[3075]; };
int sceKernelSendNotificationRequest(int, struct notification *, size_t, int);
int sceKernelDebugOutText(int, const char *);

int main(int argc, char **argv)
{
    uint64_t hash = 1469598103934665603ull;   /* FNV-1a 64: the runner computes the same over the blob */
    for (const unsigned char *p = big_elf_blob; p < big_elf_blob_end; p++) hash = (hash ^ *p) * 1099511628211ull;
    const size_t size = (size_t)(big_elf_blob_end - big_elf_blob);
    char line[256];
    snprintf(line, sizeof(line), "mihawkos-big-elf: bytes=%zu fnv=%016llx expect=%s route=%s\n", size,
        (unsigned long long)hash, BIG_ELF_EXPECT, argc > 1 ? argv[1] : "-");
    sceKernelDebugOutText(0, line);
    struct notification note = { { 0 }, { 0 } };
    snprintf(note.message, sizeof(note.message), "MihawkOS probe: a %zu MB ELF started, %s", size >> 20,
        hash == (uint64_t)BIG_ELF_EXPECT_VALUE ? "intact" : "DAMAGED");
    sceKernelSendNotificationRequest(0, &note, sizeof(note), 0);
    return 0;
}
