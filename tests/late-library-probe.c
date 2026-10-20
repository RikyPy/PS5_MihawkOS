/* SPDX-License-Identifier: GPL-3.0-or-later
 * Reserve large free Windows intervals, then load graphics/theme libraries.
 * The same asset-free PE compares late image placement on pinned Linux Wine
 * and PS5. Keep small holes and native mappings intact; release every span.
 */
#include <stdint.h>
#include <windows.h>

typedef void *(WINAPI *alloc2_fn)(HANDLE, void *, SIZE_T, DWORD, DWORD, void *, ULONG);
static void *reservations[8192];
static SIZE_T sizes[8192];

static void report(const char *stage, unsigned count, uint64_t bytes, DWORD error, int passed)
{
    char line[260];
    wsprintfA(line, "PS5_PROTON_LATE_LIBRARY_STAGE stage=%s spans=%u bytes=%I64u error=%lu passed=%d\n",
              stage, count, bytes, error, passed);
    OutputDebugStringA(line);
}

void mainCRTStartup(void)
{
    static const uint64_t bands[][2] = {
        {UINT64_C(0x40000), UINT64_C(0x1000000000)},
        {UINT64_C(0x1000000000), UINT64_C(0xfc00000000)}};
    alloc2_fn alloc = (alloc2_fn)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "VirtualAlloc2");
    unsigned count = 0, released = 0;
    uint64_t reserved_bytes = 0;
    int passed = !!alloc;
    DWORD error = 0;
    for (unsigned band = 0; passed && band < 2; ++band)
    {
        uint64_t cursor = bands[band][0], limit = bands[band][1];
        while (cursor < limit)
        {
            MEMORY_BASIC_INFORMATION info = {0};
            if (!VirtualQuery((void *)(uintptr_t)cursor, &info, sizeof(info)))
            {
                error = GetLastError();
                passed = 0;
                break;
            }
            uint64_t base = (uintptr_t)info.BaseAddress;
            uint64_t end = base + info.RegionSize;
            if (end <= cursor || end < base)
            {
                passed = 0;
                break;
            }
            if (end > limit)
                end = limit;
            uint64_t start = base > bands[band][0] ? base : bands[band][0];
            start = (start + 65535) & ~UINT64_C(65535);
            uint64_t aligned_end = end & ~UINT64_C(65535);
            if (info.State == MEM_FREE && aligned_end > start && aligned_end - start > 0x1000000)
            {
                if (count == sizeof(reservations) / sizeof(reservations[0]))
                {
                    passed = 0;
                    break;
                }
                void *at = alloc(GetCurrentProcess(), (void *)(uintptr_t)start,
                                 (SIZE_T)(aligned_end - start), MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                 PAGE_NOACCESS, NULL, 0);
                if (!at && GetLastError() == ERROR_NOT_ENOUGH_MEMORY && count)
                {
                    /* The console gives a process a finite budget of address space (the base PS5, 13.40, stopped after 31 GiB):
                     * what it allowed is what the libraries are loaded around. Said, and not a failure. */
                    char budget[160];
                    wsprintfA(budget, "PS5_PROTON_LATE_LIBRARY_STAGE stage=budget start=%I64x size=%I64x error=%lu\n", start,
                              aligned_end - start, GetLastError());
                    OutputDebugStringA(budget);
                    band = 2;
                    break;
                }
                if (at != (void *)(uintptr_t)start)
                {
                    char where[200];
                    error = GetLastError();
                    wsprintfA(where, "PS5_PROTON_LATE_LIBRARY_STAGE stage=reserve-refused start=%I64x size=%I64x query_base=%I64x query_size=%I64x error=%lu\n",
                              start, aligned_end - start, (uint64_t)(uintptr_t)info.BaseAddress, (uint64_t)info.RegionSize, error);
                    OutputDebugStringA(where);
                    {
                        /* The refusal is what the engine learns a mapping that is not Wine's from: asked again, the
                         * range that was reported free is split around it. */
                        MEMORY_BASIC_INFORMATION again = {0};
                        char note[200];
                        BOOL asked = VirtualQuery((void *)(uintptr_t)start, &again, sizeof(again));
                        wsprintfA(note, "PS5_PROTON_LATE_LIBRARY_STAGE stage=asked-again start=%I64x asked=%d base=%I64x size=%I64x state=%lx\n", start, (int)asked,
                                  (uint64_t)(uintptr_t)again.BaseAddress, (uint64_t)again.RegionSize, again.State);
                        OutputDebugStringA(note);
                    }
                    if (at)
                        VirtualFree(at, 0, MEM_RELEASE);
                    passed = 0;
                    break;
                }
                sizes[count] = (SIZE_T)(aligned_end - start);
                reservations[count++] = at;
                reserved_bytes += aligned_end - start;
            }
            cursor = end;
        }
    }
    passed = passed && count;
    report("reserve", count, reserved_bytes, error, passed);
    static const char *libraries[] = {"vulkan-1.dll", "uxtheme.dll"};
    for (unsigned i = 0; passed && i < sizeof(libraries) / sizeof(libraries[0]); ++i)
    {
        HMODULE library = LoadLibraryA(libraries[i]);
        error = library ? 0 : GetLastError();
        passed = library && FreeLibrary(library);
        report(libraries[i], count, reserved_bytes, error, passed);
    }
    for (unsigned i = 0; i < count; ++i)
        if (VirtualFree(reservations[i], 0, MEM_RELEASE))
            ++released;
    passed = passed && released == count;
    report("release", released, reserved_bytes, 0, passed);
    /* What VirtualQuery says of the released spans now: free on the pinned Linux Wine. On the console a mapping the
     * kernel holds inside one that is not Wine's is reported taken, never free. Recorded, not judged. */
    unsigned taken = 0;
    uint64_t taken_bytes = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        char *at = reservations[i], *end = at + sizes[i];
        int any = 0;
        while (at < end)
        {
            MEMORY_BASIC_INFORMATION info = {0};
            if (!VirtualQuery(at, &info, sizeof(info)) || !info.RegionSize)
                break;
            char *next = (char *)info.BaseAddress + info.RegionSize;
            if (info.State != MEM_FREE)
            {
                any = 1;
                taken_bytes += (uint64_t)((next < end ? next : end) - at);
            }
            at = next;
        }
        taken += any;
    }
    report("after-release", taken, taken_bytes, 0, passed);
    OutputDebugStringA(passed ? "PS5_PROTON_LATE_LIBRARY PASS\n" : "PS5_PROTON_LATE_LIBRARY FAIL\n");
    ExitProcess(passed ? 0 : 1);
}
