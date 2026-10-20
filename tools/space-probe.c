/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * docs/SELF_INSTALL.md phase 0: what a payload (the setup program's kind) learns of free space, and what is mounted
 * at /mnt/usb0. The title could not ask: its statvfs import resolves to nothing (evidence/probe-install-2026-10-18-01).
 * statfs first for every path, statvfs after, so a missing import ends the run only after statfs has answered.
 */
#include <errno.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/statvfs.h>

int sceKernelDebugOutText(int, const char *);

static const char *const paths[] = { "/data", "/data/ps5proton", "/mnt/usb0", "/mnt/usb1" };

int main(void)
{
    char line[512];
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        struct statfs f = { 0 };
        const int got = statfs(paths[i], &f);
        snprintf(line, sizeof(line), "mihawkos-space: call=statfs path=%s result=%d errno=%d bsize=%llu bavail=%llu "
            "blocks=%llu type=%s from=%s\n", paths[i], got, got ? errno : 0, (unsigned long long)f.f_bsize,
            (unsigned long long)f.f_bavail, (unsigned long long)f.f_blocks, got ? "-" : f.f_fstypename,
            got ? "-" : f.f_mntfromname);
        sceKernelDebugOutText(0, line);
    }
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        struct statvfs v = { 0 };
        const int got = statvfs(paths[i], &v);
        snprintf(line, sizeof(line), "mihawkos-space: call=statvfs path=%s result=%d errno=%d frsize=%llu bavail=%llu\n",
            paths[i], got, got ? errno : 0, (unsigned long long)v.f_frsize, (unsigned long long)v.f_bavail);
        sceKernelDebugOutText(0, line);
    }
    sceKernelDebugOutText(0, "mihawkos-space: done\n");
    return 0;
}
