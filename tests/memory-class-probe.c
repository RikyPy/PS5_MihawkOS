/* SPDX-License-Identifier: GPL-3.0-or-later
 * Same PE on pinned Wine and PS5. One 32 MiB resident class at a time;
 * 12-second phase holds let the owner's independent ten-second census sample
 * private, heap, pagefile, file-backed and copy-on-write allocations. The large
 * file's virtual section crosses the demand-cache threshold but touches only
 * 32 MiB. No pressure guard override and no user files.
 * --hold-ms=N holds each phase N ms instead: the gate reads no census sample, only
 * the checks and the exit status, so it runs with 0 (156 s of holds otherwise).
 */
#include <windows.h>

enum { BYTES = 32u << 20, LARGE = 256u << 20 };
static unsigned checks;
static DWORD hold_ms = 12000;
static void check(int okay, const char *stage)
{
    char line[160];
    wsprintfA(line, "memory-class: check=%s passed=%u error=%lu\n", stage, !!okay,
              okay ? 0 : GetLastError());
    OutputDebugStringA(line);
    ++checks;
    if (!okay) {
        OutputDebugStringA("PS5_PROTON_MEMORY_CLASS FAIL\n");
        ExitProcess(1);
    }
}
static void phase(const char *kind, const char *state)
{
    char line[160];
    wsprintfA(line, "memory-class: kind=%s state=%s touched=%u tick_ms=%lu\n",
              kind, state, !lstrcmpA(state, "held") ? BYTES : 0, GetTickCount());
    OutputDebugStringA(line);
    Sleep(hold_ms);
}
static void touch(volatile BYTE *p)
{
    for (unsigned i = 0; i < BYTES; i += 4096) p[i] = (BYTE)(i / 4096 + 1);
    int valid = 1;
    for (unsigned i = 0; i < BYTES; i += 4096)
        valid &= p[i] == (BYTE)(i / 4096 + 1);
    check(valid, "readback");
}
static void section(const char *kind, HANDLE file, DWORD size, DWORD access)
{
    HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READWRITE, 0, size, NULL);
    check(mapping != NULL, "section-create");
    volatile BYTE *p = MapViewOfFile(mapping, access, 0, 0, BYTES);
    check(p != NULL, "section-map");
    touch(p);
    if (access == FILE_MAP_WRITE) {
        volatile BYTE *alias = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, BYTES);
        check(alias != NULL, "alias-map");
        int coherent = 1;
        for (unsigned i = 0; i < BYTES; i += 4096) coherent &= p[i] == alias[i];
        check(coherent, "alias-coherent");
        check(UnmapViewOfFile((const void *)alias), "alias-unmap");
    }
    phase(kind, "held");
    check(UnmapViewOfFile((const void *)p), "section-unmap");
    check(CloseHandle(mapping), "section-close");
    phase(kind, "released");
}
static const char *after(const char *text, const char *prefix)
{
    for (; *text; ++text) {
        const char *a = text, *b = prefix;
        while (*b && *a == *b) ++a, ++b;
        if (!*b) return a;
    }
    return NULL;
}
void mainCRTStartup(void)
{
    const char *hold = after(GetCommandLineA(), "--hold-ms=");
    if (hold) for (hold_ms = 0; *hold >= '0' && *hold <= '9'; ++hold) hold_ms = hold_ms * 10 + *hold - '0';
    phase("baseline", "released");
    BYTE *p = VirtualAlloc(NULL, BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(p != NULL, "private-allocate");
    touch(p); phase("private", "held");
    check(VirtualFree(p, 0, MEM_RELEASE), "private-release");
    phase("private", "released");
    p = HeapAlloc(GetProcessHeap(), 0, BYTES);
    check(p != NULL, "heap-allocate");
    touch(p); phase("heap", "held");
    check(HeapFree(GetProcessHeap(), 0, p), "heap-release");
    phase("heap", "released");
    section("pagefile", INVALID_HANDLE_VALUE, BYTES, FILE_MAP_WRITE);
    char temp[MAX_PATH], path[MAX_PATH];
    check(GetTempPathA(sizeof(temp), temp) > 0, "temp-folder");
    check(GetTempFileNameA(temp, "pmc", 0, path) != 0, "temp-file");
    HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    check(file != INVALID_HANDLE_VALUE, "file-open");
    LARGE_INTEGER end; end.QuadPart = LARGE;
    check(SetFilePointerEx(file, end, NULL, FILE_BEGIN) && SetEndOfFile(file), "file-size");
    section("file-small", file, BYTES, FILE_MAP_WRITE);
    section("file-large", file, LARGE, FILE_MAP_WRITE);
    section("file-copy", file, BYTES, FILE_MAP_COPY);
    check(CloseHandle(file), "file-close");
    char line[128]; wsprintfA(line, "PS5_PROTON_MEMORY_CLASS PASS checks=%u\n", checks);
    OutputDebugStringA(line);
    ExitProcess(0);
}
