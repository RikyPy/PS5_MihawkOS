/* SPDX-License-Identifier: GPL-3.0-or-later
 * VirtualQuery must not call an occupied native mapping MEM_FREE. Compare
 * identical PE32/PE64 executables on pinned Wine and PS5. Scan 64 KiB blocks
 * below 16 MiB and power-of-two PE64 upper addresses, reserve reported-free
 * blocks as placeholders, then release. Upper addresses expose a query that
 * ignores Wine's measured host allocation limit.
 * No physical commit. A concurrent Windows allocation is excluded by requery.
 * An emulator's guest scan exposed the carrier ELF at 0x400000 on 2026-10-08. */
#include <windows.h>

typedef void *(WINAPI *alloc2_fn)(HANDLE, void *, SIZE_T, DWORD, DWORD, void *, ULONG);

void mainCRTStartup(void)
{
    alloc2_fn alloc = (alloc2_fn)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "VirtualAlloc2");
    unsigned checked = 0, failures = 0;
    ULONG_PTR first = 0;
    DWORD first_error = 0;
    for (ULONG_PTR address = 0x10000; alloc && address < 0x1000000; address += 0x10000) {
        MEMORY_BASIC_INFORMATION before = {0}, after = {0};
        if (!VirtualQuery((void *)address, &before, sizeof(before))) { ++failures; break; }
        if (before.State != MEM_FREE || before.RegionSize < 0x10000) continue;
        void *reserved = alloc(GetCurrentProcess(), (void *)address, 0x10000,
                               MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
        DWORD error = GetLastError();
        if (reserved) {
            ++checked;
            if (!VirtualFree(reserved, 0, MEM_RELEASE)) ++failures;
            continue;
        }
        if (!VirtualQuery((void *)address, &after, sizeof(after))) { ++failures; break; }
        if (after.State != MEM_FREE || after.RegionSize < 0x10000) continue;
        ++failures;
        if (!first) { first = address; first_error = error; }
    }
    unsigned upper_queries = 0, upper_free = 0, upper_unavailable = 0;
#ifdef _WIN64
    for (unsigned shift = 36; alloc && shift <= 46; ++shift) {
        ULONG_PTR address = (ULONG_PTR)1 << shift;
        MEMORY_BASIC_INFORMATION info = {0};
        if (!VirtualQuery((void *)address, &info, sizeof(info))) { ++failures; break; }
        ++upper_queries;
        if (info.State != MEM_FREE) { ++upper_unavailable; continue; }
        ++upper_free;
        void *reserved = alloc(GetCurrentProcess(), (void *)address, 0x10000,
                               MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
        DWORD error = GetLastError();
        if (reserved == (void *)address) {
            ++checked;
            if (!VirtualFree(reserved, 0, MEM_RELEASE)) ++failures;
        } else {
            if (reserved) VirtualFree(reserved, 0, MEM_RELEASE);
            ++failures;
            if (!first) { first = address; first_error = error; }
        }
    }
#endif
    int passed = alloc && checked && !failures;
    char line[300];
    wsprintfA(line, "PS5_PROTON_VIRTUAL_QUERY %s: reserved=%u failures=%u first_free_conflict=%p error=%lu upper_queries=%u upper_free=%u upper_unavailable=%u\n",
        passed ? "PASS" : "FAIL", checked, failures, (void *)first, first_error,
        upper_queries, upper_free, upper_unavailable);
    OutputDebugStringA(line);
    ExitProcess(passed ? 0 : 1);
}
