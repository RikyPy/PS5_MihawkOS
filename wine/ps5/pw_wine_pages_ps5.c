/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The console side of pw_wine_pages, linked into ntdll.prx: the helper reserves its shared-page range before the
 * program runs (pw_wine_set_frame_allocator is called by the loader before Wine starts) and maps each page there
 * with a fixed mapping over that reservation, as the GPU client does for its host ranges. */
#include "pw_wine_pages.h"
#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>

extern int sceKernelReserveVirtualRange(void **address, size_t bytes, int flags, size_t alignment);
extern int sceKernelMunmap(void *address, size_t bytes);
extern int sceKernelDebugOutText(int channel, const char *text);

enum { KERNEL_FIXED = 0x10, KERNEL_NO_OVERWRITE = 0x80 };

static void say(const char *line)
{
    fputs(line, stderr);
    sceKernelDebugOutText(0, line);
}

static int lay_back(void *context, uintptr_t address, size_t bytes)
{
    void *at = (void *)address;
    (void)context;
    /* A reservation laid over the mapping replaces it; if that is refused the range is at least unmapped, and lost. */
    if (sceKernelReserveVirtualRange(&at, bytes, KERNEL_FIXED, PW_WINE_PAGES_PAGE) || at != (void *)address) {
        char line[128];
        snprintf(line, sizeof(line), "[wine-ps5] shared page %#lx +%#zx: reservation not laid back, range lost\n",
                 (unsigned long)address, bytes);
        say(line);
        (void)sceKernelMunmap((void *)address, bytes);
        return -1;
    }
    return 0;
}

static int map(void *context, uintptr_t address, size_t bytes, int protection, int fd)
{
    static unsigned refusals;
    void *mapped = mmap((void *)address, bytes, protection, MAP_SHARED | MAP_FIXED, fd, 0);
    char resident[4];
    int status = mapped == (void *)address ? 0 : -1, saved = mapped == MAP_FAILED ? errno : 0;
    (void)context;

    /* A mapping that came back with nothing behind it is what broke the window pages; mincore answers ENOMEM for an
     * address with no mapping (sceKernelVirtualQuery does not list physical shared memory over a reservation). */
    if (!status && mincore((void *)address, PW_WINE_PAGES_PAGE, resident) && errno == ENOMEM) {
        status = -1;
        saved = ENOMEM;
    }
    if (status) {
        if (refusals++ < 12) {
            char line[160];
            snprintf(line, sizeof(line), "[wine-ps5] shared page %#lx +%#zx (fd %d) not mapped: got %p, errno %d\n",
                     (unsigned long)address, bytes, fd, mapped, saved);
            say(line);
        }
        if (mapped != MAP_FAILED && mapped != (void *)address) (void)munmap(mapped, bytes);
        errno = saved ? saved : ENOMEM;
    }
    return status;
}

/* Below the user limit (0xfc00000000: no reservation at or above it is granted, mapping-boundary-2026-10-08-01),
 * clear of the heap areas (0x2000000000-0x6800000000) and the section arena (0x8000000000). */
static const uintptr_t hints[] = { 0xfbf8000000u, 0x7ff0000000u, 0x6ff0000000u };

int pw_wine_pages_reserve(void)
{
    static const PwWinePagesOps ops = { NULL, map, lay_back };
    char line[128];

    if (pw_wine_pages_base(NULL)) return 0;
    for (unsigned i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        void *at = (void *)hints[i];
        if (sceKernelReserveVirtualRange(&at, PW_WINE_PAGES_MAX_BYTES, KERNEL_FIXED | KERNEL_NO_OVERWRITE,
                                         PW_WINE_PAGES_PAGE) || at != (void *)hints[i])
            continue;
        if (!pw_wine_pages_init(&ops, hints[i], PW_WINE_PAGES_MAX_BYTES)) {
            snprintf(line, sizeof(line), "wine-ps5: shared pages at %#lx (%zu MiB)\n", (unsigned long)hints[i],
                     (size_t)(PW_WINE_PAGES_MAX_BYTES >> 20));
            fputs(line, stderr);
            return 0;
        }
        (void)sceKernelMunmap(at, PW_WINE_PAGES_MAX_BYTES);
    }
    say("[wine-ps5] no shared-page range: no shared page is mapped\n");
    pw_wine_pages_no_range();
    return -1;
}
