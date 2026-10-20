/* SPDX-License-Identifier: GPL-3.0-or-later
 * What a pagefile-backed Windows section (CreateFileMapping with no file: how Chromium shares memory between its processes) costs
 * the console's flexible memory: it makes one of PS5_PROBE_SECTION_MIB (default 128, at most 512), maps it, writes every page,
 * keeps it PS5_PROBE_HOLD_SECONDS (default 12; ShellCore's VM Stats line comes about every 39 s), reads samples back and lets it go. The helper's own census lines (vm resident, memory resident) show where its
 * pages are counted (docs/PLAN.md, "Direct memory shared between processes"). */
#include <windows.h>

static unsigned env_number(const char *name, unsigned fallback)
{
    char text[16];
    unsigned value = 0;
    DWORD length = GetEnvironmentVariableA(name, text, sizeof(text));
    if (!length || length >= sizeof(text)) return fallback;
    for (DWORD i = 0; i < length; i++) {
        if (text[i] < '0' || text[i] > '9') return fallback;
        value = value * 10 + (unsigned)(text[i] - '0');
    }
    return value ? value : fallback;
}

void mainCRTStartup(void)
{
    char line[160];
    unsigned mib = env_number("PS5_PROBE_SECTION_MIB", 128), wrong = 0;
    if (mib > 512) mib = 512;
    const SIZE_T bytes = (SIZE_T)mib << 20;
    HANDLE section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)bytes, NULL);
    unsigned char *view = section ? MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, bytes) : NULL;
    if (!view) {
        wsprintfA(line, "PS5_PROTON_SECTION_MEMORY FAIL: section or view error %lu\n", GetLastError());
        OutputDebugStringA(line);
        ExitProcess(1);
    }
    for (SIZE_T at = 0; at < bytes; at += 4096) view[at] = (unsigned char)(at >> 12);
    OutputDebugStringA("PS5_PROTON_SECTION_MEMORY: written\n");
    Sleep(env_number("PS5_PROBE_HOLD_SECONDS", 12) * 1000);
    for (SIZE_T at = 0; at < bytes; at += 4096 * 7) if (view[at] != (unsigned char)(at >> 12)) wrong++;
    UnmapViewOfFile(view);
    CloseHandle(section);
    wsprintfA(line, "PS5_PROTON_SECTION_MEMORY %s: %u MiB, %u wrong samples\n", wrong ? "FAIL" : "PASS", mib, wrong);
    OutputDebugStringA(line);
    ExitProcess(wrong != 0);
}
