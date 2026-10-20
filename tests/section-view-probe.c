/* SPDX-License-Identifier: GPL-3.0-or-later
 * Asset-free shared-section contract: same PE on pinned Linux Wine and PS5.
 * A small control and 13.5 GiB section, two coherent aliases, handle-close
 * lifetime, concurrent faults, protection exceptions and surviving tail view.
 */
#include <stdint.h>
#include <windows.h>
static void log_case(SIZE_T size, const char *stage, DWORD error, int passed)
{
    char line[240];
    wsprintfA(line, "PS5_PROTON_SECTION_CASE bytes=%I64u stage=%s error=%lu passed=%d\n", (uint64_t)size,
              stage, error, passed);
    OutputDebugStringA(line);
}
typedef struct ThreadWork
{
    volatile unsigned char *full, *alias;
    SIZE_T offset;
    int passed;
} ThreadWork;
static DWORD WINAPI thread_check(void *opaque)
{
    ThreadWork *w = opaque;
    w->passed = 1;
    for (unsigned n = 1; n <= 24; n++)
    {
        w->full[w->offset] = (unsigned char)n;
        if (w->alias[w->offset] != n)
            w->passed = 0;
        w->alias[w->offset + 1] = (unsigned char)(n + 1);
        if (w->full[w->offset + 1] != n + 1)
            w->passed = 0;
    }
    return w->passed ? 0 : 1;
}
static int write_fault(volatile unsigned char *at)
{
    int caught = 0;
    __try
    {
        *at = 0x99;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH)
    {
        caught = 1;
    }
    return caught;
}
static int read_fault(volatile unsigned char *at)
{
    int caught = 0;
    __try
    {
        volatile unsigned char value = *at;
        (void)value;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH)
    {
        caught = 1;
    }
    return caught;
}
/* A fixed alias replaces a placeholder, then restores it on unmap. This
 * exercises the actual emulator's shared-section API without guest assets. */
static int placeholder_alias(HANDLE section, uint64_t offset, volatile unsigned char *full)
{
    typedef void *(WINAPI * alloc_fn)(HANDLE, void *, SIZE_T, DWORD, DWORD, void *, ULONG);
    typedef void *(WINAPI * map_fn)(HANDLE, HANDLE, void *, ULONG64, SIZE_T, ULONG, ULONG, void *,
                                    ULONG);
    typedef BOOL(WINAPI * unmap_fn)(HANDLE, void *, ULONG);
    HMODULE module = GetModuleHandleA("kernelbase.dll");
    alloc_fn alloc = (alloc_fn)GetProcAddress(module, "VirtualAlloc2");
    map_fn map = (map_fn)GetProcAddress(module, "MapViewOfFile3");
    unmap_fn unmap = (unmap_fn)GetProcAddress(module, "UnmapViewOfFile2");
    if (!alloc || !map || !unmap)
        return 0;
    void *placeholder = alloc(GetCurrentProcess(), NULL, 65536, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                              PAGE_NOACCESS, NULL, 0);
    if (!placeholder)
        return 0;
    volatile unsigned char *alias = map(section, GetCurrentProcess(), placeholder, offset, 65536,
                                        MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    int passed = alias == placeholder;
    if (alias)
    {
        full[offset] = 0x31;
        passed = alias[0] == 0x31 && passed;
        alias[0] = 0x41;
        passed = full[offset] == 0x41 && passed;
        passed = unmap(GetCurrentProcess(), (void *)alias, MEM_PRESERVE_PLACEHOLDER) && passed;
        MEMORY_BASIC_INFORMATION info = {0};
        passed = VirtualQuery(placeholder, &info, sizeof(info)) == sizeof(info) &&
                 info.State == MEM_RESERVE && info.RegionSize == 65536 && passed;
        full[offset] = 0;
    }
    passed = VirtualFree(placeholder, 0, MEM_RELEASE) && passed;
    return passed;
}

/* An emulator's guest memory, as the earlier emulator experiment used it: the game writes through a view that replaced a
 * placeholder, under write tracking (each 4 KiB page read-only, a write fault opens it and is counted); textures are read
 * back through the whole-section alias (TryReadSparseBacking). A Unity game's menu glyphs drew as solid blocks on the
 * console (2026-10-15), so every byte of the tracked range must read back through the alias as written. Stores of every
 * shape: single bytes, a copy across 4 KiB pages, unaligned 16-byte stores across a page boundary, 8-byte stores at page
 * ends. On the console in a helper the stores beside read-only pages go through the 4 KiB MMU's alias of the same direct
 * memory (patch 1134, docs/MMU4K.md): what they wrote must be what both views read. */
#define TRACK_BYTES (1u << 20)
static volatile unsigned char *track_view;
static volatile LONG track_faults, track_strays;
static LONG CALLBACK track_handler(EXCEPTION_POINTERS *pointers)
{
    const EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    DWORD old;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2 ||
        record->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    const ULONG_PTR at = record->ExceptionInformation[1], base = (ULONG_PTR)track_view;
    if (!track_view || at < base || at - base >= TRACK_BYTES)
    {
        InterlockedIncrement(&track_strays);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    InterlockedIncrement(&track_faults);
    return VirtualProtect((void *)(at & ~(ULONG_PTR)4095), 4096, PAGE_READWRITE, &old) ? EXCEPTION_CONTINUE_EXECUTION
                                                                                         : EXCEPTION_CONTINUE_SEARCH;
}
static unsigned char track_value(SIZE_T i, unsigned round)
{
    return (unsigned char)(i * 131 + round * 29 + (i >> 12));
}
/* 1 when every byte reads back as expected; else logs the first difference. */
static int track_compare(SIZE_T size, const char *stage, volatile const unsigned char *alias, const unsigned char *expected)
{
    for (SIZE_T i = 0; i < TRACK_BYTES; i++)
        if (alias[i] != expected[i])
        {
            char line[240];
            SIZE_T differ = 0;
            for (SIZE_T j = i; j < TRACK_BYTES; j++)
                differ += alias[j] != expected[j];
            wsprintfA(line, "PS5_PROTON_SECTION_TRACK %s first=%#lx alias=%02x expected=%02x differ=%lu faults=%ld\n",
                      stage, (DWORD)i, alias[i], expected[i], (DWORD)differ, track_faults);
            OutputDebugStringA(line);
            log_case(size, stage, 0, 0);
            return 0;
        }
    return 1;
}
/* Three rounds of tracked stores into view (TRACK_BYTES), each byte read back through alias: the section's other view,
 * or view itself for private memory. */
static int track_rounds(SIZE_T size, volatile unsigned char *view, volatile unsigned char *alias)
{
    static unsigned char expected[TRACK_BYTES];
    DWORD old;
    int passed = 1;
    void *handler = AddVectoredExceptionHandler(1, track_handler);
    track_faults = track_strays = 0;
    passed = handler != NULL && passed;
    for (unsigned round = 0; passed && round < 3; round++)
    {
        for (SIZE_T i = 0; i < TRACK_BYTES; i++)
            alias[i] = expected[i] = track_value(i, round);
        track_view = view;
        passed = VirtualProtect((void *)view, TRACK_BYTES, PAGE_READONLY, &old) && passed;
        /* one byte in every 4 KiB page */
        for (SIZE_T i = 7; i < TRACK_BYTES; i += 4096)
            view[i] = expected[i] = (unsigned char)~expected[i];
        passed = track_compare(size, "track-bytes", alias, expected) && passed;
        passed = VirtualProtect((void *)view, TRACK_BYTES, PAGE_READONLY, &old) && passed;
        /* a copy across five 4 KiB pages, as memcpy does it */
        {
            unsigned char source[20000];
            SIZE_T start = 3 * 16384 + 4000, count = sizeof(source);
            for (SIZE_T i = 0; i < count; i++)
                source[i] = (unsigned char)(i ^ (round * 77));
            void *to = (void *)(view + start);
            const void *from = source;
            __asm__ volatile("rep movsb" : "+D"(to), "+S"(from), "+c"(count) : : "memory");
            for (SIZE_T i = 0; i < sizeof(source); i++)   /* volatile: no memcpy call, the probe has no C library */
                ((volatile unsigned char *)expected)[start + i] = source[i];
        }
        passed = track_compare(size, "track-copy", alias, expected) && passed;
        passed = VirtualProtect((void *)view, TRACK_BYTES, PAGE_READONLY, &old) && passed;
        /* unaligned 16-byte stores across a 4 KiB boundary, and 8-byte stores at 4 KiB page ends */
        for (SIZE_T page = 9; page < TRACK_BYTES / 4096; page += 13)
        {
            SIZE_T at = page * 4096 - 7;
            unsigned char sixteen[16];
            for (unsigned k = 0; k < 16; k++)
                sixteen[k] = expected[at + k] = (unsigned char)(k * 17 + round + page);
            __asm__ volatile("movdqu (%1), %%xmm0\n\tmovdqu %%xmm0, (%0)" : : "r"(view + at), "r"(sixteen)
                             : "xmm0", "memory");
            SIZE_T end = (page + 3) * 4096 - 8;
            uint64_t word = 0x0123456789abcdefull ^ ((uint64_t)page << 32) ^ round;
            *(volatile uint64_t *)(view + end) = word;
            for (unsigned k = 0; k < 8; k++)
                expected[end + k] = (unsigned char)(word >> (k * 8));
        }
        passed = track_compare(size, "track-wide", alias, expected) && passed;
        passed = VirtualProtect((void *)view, TRACK_BYTES, PAGE_READWRITE, &old) && passed;
        track_view = NULL;
    }
    if (handler)
        RemoveVectoredExceptionHandler(handler);
    {
        char line[200];
        wsprintfA(line, "PS5_PROTON_SECTION_TRACK bytes=%I64u faults=%ld strays=%ld passed=%d\n", (uint64_t)size,
                  track_faults, track_strays, passed);
        OutputDebugStringA(line);
    }
    return track_faults > 0 && passed;
}
static int tracked_writes(HANDLE section, SIZE_T size, volatile unsigned char *full)
{
    typedef void *(WINAPI * alloc_fn)(HANDLE, void *, SIZE_T, DWORD, DWORD, void *, ULONG);
    typedef void *(WINAPI * map_fn)(HANDLE, HANDLE, void *, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
    typedef BOOL(WINAPI * unmap_fn)(HANDLE, void *, ULONG);
    HMODULE module = GetModuleHandleA("kernelbase.dll");
    alloc_fn alloc = (alloc_fn)GetProcAddress(module, "VirtualAlloc2");
    map_fn map = (map_fn)GetProcAddress(module, "MapViewOfFile3");
    unmap_fn unmap = (unmap_fn)GetProcAddress(module, "UnmapViewOfFile2");
    /* Past the first 2 MiB chunk and off a chunk boundary, as a guest texture lies anywhere in the backing. */
    const uint64_t offset = size > ((uint64_t)64 << 20) ? ((uint64_t)37 << 20) + 0x30000 : 0;
    if (!alloc || !map || !unmap)
        return 0;
    void *placeholder = alloc(GetCurrentProcess(), NULL, TRACK_BYTES, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                              PAGE_NOACCESS, NULL, 0);
    if (!placeholder)
        return 0;
    volatile unsigned char *view = map(section, GetCurrentProcess(), placeholder, offset, TRACK_BYTES,
                                       MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    volatile unsigned char *alias = full + offset;
    int passed = view == placeholder && track_rounds(size, view, alias);
    for (SIZE_T i = 0; i < TRACK_BYTES; i++)   /* zero again: the checks after this one expect untouched bytes */
        alias[i] = 0;
    if (view)
        passed = unmap(GetCurrentProcess(), (void *)view, MEM_PRESERVE_PLACEHOLDER) && passed;
    passed = VirtualFree(placeholder, 0, MEM_RELEASE) && passed;
    return passed;
}
/* The same stores into 1 MiB of VirtualAlloc memory (on the console a helper's direct-memory run, which the 4 KiB MMU
 * maps a second time for its alias): each byte reads back as written. */
static int tracked_private(void)
{
    volatile unsigned char *memory = VirtualAlloc(NULL, TRACK_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    int passed = memory != NULL && track_rounds(0, memory, memory);
    log_case(0, "track-private", passed ? 0 : GetLastError(), passed);
    return memory && VirtualFree((void *)memory, 0, MEM_RELEASE) && passed;
}

static int check(SIZE_T size)
{
    HANDLE section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE | SEC_COMMIT,
                                        (DWORD)((uint64_t)size >> 32), (DWORD)size, NULL);
    if (!section)
    {
        log_case(size, "create", GetLastError(), 0);
        return 0;
    }
    volatile unsigned char *full = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
    if (!full)
    {
        DWORD error = GetLastError();
        CloseHandle(section);
        log_case(size, "map-full", error, 0);
        return 0;
    }
    uint64_t offset = size - 65536;
    volatile unsigned char *alias = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE,
                                                  (DWORD)(offset >> 32), (DWORD)offset, 65536);
    if (!alias)
    {
        DWORD error = GetLastError();
        UnmapViewOfFile((void *)full);
        CloseHandle(section);
        log_case(size, "map-alias", error, 0);
        return 0;
    }
    MEMORY_BASIC_INFORMATION info = {0};
    int passed = VirtualQuery((void *)full, &info, sizeof(info)) == sizeof(info) &&
                 info.State == MEM_COMMIT && info.Type == MEM_MAPPED && info.RegionSize >= size;
    volatile unsigned char *second = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
    if (!second)
    {
        UnmapViewOfFile((void *)full);
        UnmapViewOfFile((void *)alias);
        CloseHandle(section);
        log_case(size, "map-second", GetLastError(), 0);
        return 0;
    }
    passed = placeholder_alias(section, offset, full) && passed;
    passed = tracked_writes(section, size, full) && passed;
    passed = CloseHandle(section) && passed;
    ThreadWork work[8];
    HANDLE threads[8];
    unsigned started = 0;
    for (unsigned i = 0; i < 8; i++)
    {
        work[i].full = full;
        work[i].alias = second;
        work[i].offset = ((size / 2) & ~(SIZE_T)65535) + i * 16384;
        work[i].passed = 0;
        threads[i] = CreateThread(NULL, 0, thread_check, &work[i], 0, NULL);
        if (!threads[i])
        {
            passed = 0;
            break;
        }
        started++;
    }
    for (unsigned i = 0; i < started; i++)
    {
        DWORD status = 1;
        if (WaitForSingleObject(threads[i], 10000) != WAIT_OBJECT_0)
        {
            log_case(size, "thread-timeout", GetLastError(), 0);
            ExitProcess(1);
        }
        if (!GetExitCodeThread(threads[i], &status) || status || !work[i].passed)
            passed = 0;
        CloseHandle(threads[i]);
    }
    DWORD old = 0;
    const SIZE_T protect_offset = 131072;
    passed = VirtualProtect((void *)(full + protect_offset), 16384, PAGE_READONLY, &old) && passed;
    passed = write_fault(full + protect_offset) && passed;
    second[protect_offset] = 0x51;
    passed = full[protect_offset] == 0x51 && passed;
    passed = VirtualProtect((void *)(full + protect_offset), 16384, PAGE_NOACCESS, &old) && passed;
    passed = read_fault(full + protect_offset) && passed;
    passed = VirtualProtect((void *)(full + protect_offset), 16384, PAGE_READWRITE, &old) && passed;
    passed = full[protect_offset] == 0x51 && passed;
    passed = UnmapViewOfFile((void *)second) && passed;
    passed = full[0] == 0 && full[size - 1] == 0 && passed;
    full[offset] = 0x51;
    full[size - 1] = 0xa1;
    passed = alias[0] == 0x51 && alias[65535] == 0xa1 && passed;
    alias[0] = 0x71;
    passed = full[offset] == 0x71 && passed;
    passed = UnmapViewOfFile((void *)full) && passed;
    passed = alias[0] == 0x71 && alias[65535] == 0xa1 && passed;
    passed = UnmapViewOfFile((void *)alias) && passed;
    log_case(size, "complete", 0, passed);
    return passed;
}
void mainCRTStartup(void)
{
    unsigned passed = check((SIZE_T)2 << 20) + check((SIZE_T)13824 << 20) + tracked_private();
    OutputDebugStringA(passed == 3 ? "PS5_PROTON_LARGE_SECTION PASS cases=3\n"
                                   : "PS5_PROTON_LARGE_SECTION FAIL\n");
    ExitProcess(passed == 3 ? 0 : 1);
}
