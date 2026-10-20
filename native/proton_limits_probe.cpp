// SPDX-License-Identifier: GPL-3.0-or-later
#include "proton_limits_probe.h"
#include "../ps5/src/platform.h"
#include "../ps5/src/proton_launch.h"
#include <ps5platform/kernel.h>
#include <cstdint>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
// Each line to klog and to a file: klog can lose the last lines before the title replaces itself.
FILE *results;
__attribute__((format(printf, 1, 2))) void report(const char *format, ...)
{
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    say("%s", line);
    if (results) {
        fprintf(results, "%s\n", line);
        fflush(results);
    }
}

void pages()
{
    report("limits: getpagesize %d", getpagesize());
    void *area = mmap(nullptr, 65536, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (area == MAP_FAILED) {
        report("limits: pages: mmap failed, errno %d", errno);
        return;
    }
    char *base = static_cast<char *>(area);
    errno = 0;
    const int status = mprotect(base + 4096, 4096, PROT_READ);
    const int error = errno;
    // Where the read-only range the kernel keeps begins and ends: 4 KiB pages if it is exactly the
    // range asked for, 16 KiB if rounded.
    void *start = nullptr, *end = nullptr;
    uint32_t protection = 0;
    const int query = sceKernelQueryMemoryProtection(base + 4096, &start, &end, &protection);
    report("limits: pages: mprotect of 4 KiB at +4 KiB -> %d (errno %d); the kernel's range +%td..+%td protection %#x (query %d)",
        status, error, static_cast<char *>(start) - base, static_cast<char *>(end) - base, protection, query);
    munmap(area, 65536);
}

} // namespace

void proton_limits_probe(const char *scratch)
{
    results = fopen(PROTON_DATA_ROOT "/logs/limits-probe.txt", "w");
    // A call the console has taken out raises SIGSYS, whose default ends the process: ignored here,
    // the call returns ENOSYS instead.
    struct sigaction ignore{}, previous{};
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGSYS, &ignore, &previous);
    report("limits: probe starts (pid %d, uid %d, euid %d)", int(getpid()), int(getuid()), int(geteuid()));
    (void)scratch;
    pages();
    sigaction(SIGSYS, &previous, nullptr);
    report("limits: probe done");
    if (results) fclose(results);
    results = nullptr;
}
