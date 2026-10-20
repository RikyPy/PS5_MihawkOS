/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercise an emulator's address-space contract without guest code or assets:
 * scan three address bands, reserve free spans as placeholders, then release
 * every span. No physical commit or overwrite. Compare the same PE on Linux
 * Wine and PS5; report the unsupported band or first reservation conflict.
 * The earlier emulator experiment (removed) used these bands.
 */
#include <windows.h>
#include <stdint.h>

typedef void *(WINAPI *alloc2_fn)(HANDLE, void *, SIZE_T, DWORD, DWORD, void *, ULONG);
static void *reservations[8192];

void mainCRTStartup(void)
{
    static const uint64_t bands[][2] = {
        {0x40000, 0x1000000000},
        {0x1000000000, 0xfc00000000},
        {0x80000000000, 0x88000000000}
    };
    alloc2_fn alloc = (alloc2_fn)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "VirtualAlloc2");
    unsigned count = 0, completed = 0, released = 0, failures = !alloc;
    uint64_t first = 0, bytes = 0, reserved_bytes = 0;
    DWORD error = 0;
    SYSTEM_INFO system;
    GetNativeSystemInfo(&system);
    for (unsigned band = 0; alloc && !failures && band < 3; ++band) {
        unsigned before = count;
        uint64_t cursor = bands[band][0], limit = bands[band][1];
        while (cursor < limit) {
            MEMORY_BASIC_INFORMATION info = {0};
            if (!VirtualQuery((void *)(uintptr_t)cursor, &info, sizeof(info))) {
                first = cursor; error = GetLastError(); ++failures; break;
            }
            uint64_t base = (uintptr_t)info.BaseAddress;
            uint64_t end = base + info.RegionSize;
            if (end < base || end <= cursor) { first = cursor; ++failures; break; }
            if (end > limit) end = limit;
            uint64_t start = base > bands[band][0] ? base : bands[band][0];
            start = (start + 0xffff) & ~UINT64_C(0xffff);
            uint64_t aligned_end = end & ~UINT64_C(0xffff);
            if (info.State == MEM_FREE && aligned_end > start && aligned_end - start > 0x1000000) {
                if (count == sizeof(reservations) / sizeof(reservations[0])) {
                    first = start; ++failures; break;
                }
                void *at = alloc(GetCurrentProcess(), (void *)(uintptr_t)start,
                                 (SIZE_T)(aligned_end - start), MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                 PAGE_NOACCESS, NULL, 0);
                if (at != (void *)(uintptr_t)start) {
                    first = start; bytes = aligned_end - start;
                    error = GetLastError(); ++failures;
                    if (at) VirtualFree(at, 0, MEM_RELEASE);
                    break;
                }
                reservations[count++] = at;
                reserved_bytes += aligned_end - start;
            }
            cursor = end;
        }
        if (!failures && count == before) {
            first = bands[band][0]; bytes = limit - first;
            error = ERROR_NOT_ENOUGH_MEMORY; ++failures;
        }
        if (!failures) ++completed;
    }
    for (unsigned i = 0; i < count; ++i)
        if (VirtualFree(reservations[i], 0, MEM_RELEASE)) ++released;
        else ++failures;
    char line[300];
    int passed = !failures && completed == 3 && count;
    wsprintfA(line, "PS5_PROTON_GUEST_ADDRESS_SPACE %s: bands=%u reservations=%u bytes=%I64u first=%p size=%I64u error=%lu maximum=%p released=%u\n",
              passed ? "PASS" : "FAIL", completed, count, reserved_bytes,
              (void *)(uintptr_t)first, bytes, error, system.lpMaximumApplicationAddress, released);
    OutputDebugStringA(line);
    ExitProcess(passed ? 0 : 1);
}
