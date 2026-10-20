/* SPDX-License-Identifier: GPL-3.0-or-later
 * The protection an image's data page reports once the program has written it.
 * A writable image page starts copy-on-write; on Windows the first write makes it a private copy, and
 * VirtualProtect then reports the old protection as PAGE_READWRITE. Wine cannot see a write to such a page
 * (the host maps it writable), so it reported PAGE_WRITECOPY, and Chromium's startup check that it seals
 * a page it wrote (protected memory: VirtualProtect(PAGE_READONLY) must return PAGE_READWRITE, then its
 * pair back) ended every CEF renderer on a breakpoint: a launcher on 2026-10-11, 11 of 11 renderers.
 * Compare identical PE32/PE64 executables on the pinned Wine and the console. Only VirtualProtect's answer
 * is covered: Wine's simulation marks a page copied at its first protection change, so a VirtualQuery
 * before that still says PAGE_WRITECOPY. */
#include <windows.h>

/* Initialised data, so the pages come from the image's file and are copy-on-write; page aligned so each
 * of the two pages is a whole Windows page whatever the host's page size. */
static volatile unsigned char sealed[0x2000] __attribute__((aligned(0x1000))) = {1};

static char line[400];
static int failures;

static void check(const char *what, BOOL ok, DWORD old, DWORD wanted)
{
    if (ok && old == wanted) return;
    ++failures;
    wsprintfA(line + lstrlenA(line), " %s ok=%d old=%#lx wanted=%#lx;", what, ok, old, wanted);
}

void mainCRTStartup(void)
{
    DWORD old = 0;
    BOOL ok;

    sealed[0] = 42;
    sealed[0x1000] = 43;
    ok = VirtualProtect((void *)sealed, 0x1000, PAGE_READONLY, &old);
    check("seal written page", ok, old, PAGE_READWRITE);
    ok = VirtualProtect((void *)sealed, 0x1000, PAGE_READWRITE, &old);
    check("unseal", ok, old, PAGE_READONLY);
    ok = VirtualProtect((void *)(sealed + 0x1000), 0x1000, PAGE_READONLY, &old);
    check("seal second page", ok, old, PAGE_READWRITE);
    ok = VirtualProtect((void *)(sealed + 0x1000), 0x1000, PAGE_READWRITE, &old);
    check("unseal second page", ok, old, PAGE_READONLY);
    sealed[1] = 44;
    if (sealed[0] != 42 || sealed[1] != 44 || sealed[0x1000] != 43) { ++failures; lstrcatA(line, " contents lost;"); }

    {
        char out[600];
        wsprintfA(out, "PS5_PROTON_IMAGE_PROTECT %s: failures=%d%s\n", failures ? "FAIL" : "PASS", failures, line);
        OutputDebugStringA(out);
    }
    ExitProcess(failures ? 1 : 0);
}
