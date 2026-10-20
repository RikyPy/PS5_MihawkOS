/* SPDX-License-Identifier: GPL-3.0-or-later
 * Does the console's resident-memory figure ("RSS x of N" in SceShellCore's VM Stats line) apply to memory that lives in direct memory?
 * The guard (src/pw_memory_guard.h) ends processes at 940 MiB because the console froze once at 1,026 MiB, when the memory was flexible.
 * A launcher sits at about 887 MiB and its GPU process pushes it past the figure (1,002 MiB against 967), so the answer decides whether
 * moving memory into direct memory (the heap already is) or sharing it can help, or only using less can.
 *
 *   memory-cap-probe --mode=heap|section [--step=64] [--max=1400] [--pause=6]
 *
 * heap: VirtualAlloc memory, which the console serves from direct memory. section: a pagefile-backed section mapped and touched, which is
 * physical shared memory (flexible class). Each step commits and touches STEP MiB, says so, and waits PAUSE seconds, so the title's census
 * and the kernel's VM Stats lines can be read against the total. Run with PROTON_SYSTEM_MEMORY_LIMIT=0 (the guard off) and stop it by hand.
 * Prints PS5_PROTON_MEMCAP lines and PS5_PROTON_MEMCAP DONE. */
#include <windows.h>

static void line(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static int number(const char *command, const char *key, int fallback)
{
    const char *c = command;
    int length = lstrlenA(key);
    for (; *c; ++c) {
        int i = 0;
        while (i < length && c[i] == key[i]) ++i;
        if (i == length) {
            int value = 0;
            const char *digits = c + length;
            while (*digits >= '0' && *digits <= '9') value = value * 10 + (*digits++ - '0');
            return value;
        }
    }
    return fallback;
}

void mainCRTStartup(void)
{
    const char *command = GetCommandLineA();
    BOOL section = FALSE;
    int step = number(command, "--step=", 64), max = number(command, "--max=", 1400), pause = number(command, "--pause=", 6);
    int total = 0;
    char text[160];
    const char *c;
    HANDLE mapping = NULL;

    for (c = command; *c; ++c)
        if (c[0] == '-' && c[1] == '-' && c[2] == 'm' && c[3] == 'o' && c[4] == 'd' && c[5] == 'e' && c[6] == '=' && c[7] == 's') section = TRUE;
    wsprintfA(text, "PS5_PROTON_MEMCAP start mode=%s step=%d max=%d pause=%d\n", section ? "section" : "heap", step, max, pause);
    line(text);
    if (section) {
        mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)max << 20, NULL);
        if (!mapping) { line("PS5_PROTON_MEMCAP FAIL: no section\n"); ExitProcess(1); }
    }
    while (total + step <= max) {
        volatile char *block;
        SIZE_T offset;
        if (section) block = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, (DWORD)total << 20, (SIZE_T)step << 20);
        else block = VirtualAlloc(NULL, (SIZE_T)step << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!block) {
            wsprintfA(text, "PS5_PROTON_MEMCAP FAIL: no memory at %d MiB (error %lu)\n", total, GetLastError());
            line(text);
            break;
        }
        for (offset = 0; offset < ((SIZE_T)step << 20); offset += 4096) block[offset] = (char)(offset >> 12);
        total += step;
        wsprintfA(text, "PS5_PROTON_MEMCAP step total=%d MiB at %lu ms\n", total, GetTickCount());
        line(text);
        Sleep((DWORD)pause * 1000);
    }
    wsprintfA(text, "PS5_PROTON_MEMCAP holding %d MiB for 20 s\n", total);
    line(text);
    Sleep(20000);
    line("PS5_PROTON_MEMCAP DONE\n");
    ExitProcess(0);
}
