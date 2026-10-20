/* SPDX-License-Identifier: GPL-3.0-or-later
 * How much memory a Windows process can commit on the console, and what stops it, with no
 * GPU in the picture. A JVM (a Java game) ran out of native memory at about 3.5 GiB mapped of
 * the 12 GiB direct memory; this commits the same way a heap does and says where it ends.
 *
 * MEM_PROBE_MODE=sweep (default): reserve MEM_PROBE_GIB (default 6) GiB, then commit 1 MiB
 *   at a time, writing a byte to every 4 KiB, until the target or a refusal.
 * MEM_PROBE_MODE=big: one MEM_COMMIT of MEM_PROBE_GIB GiB in a single call, then touch it.
 * MEM_PROBE_MODE=frag: MEM_PROBE_COUNT (default 12000) separate 64 KiB commits scattered
 *   over a reservation, the way a malloc-heavy process commits.
 * Reports what it reached; ends by itself and frees everything with the process. */
#include <windows.h>

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void number(char *out, unsigned long long value)
{
    char digits[24];
    int count = 0;
    do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count) *out++ = digits[--count];
    *out = 0;
}

static unsigned long long env_number(const WCHAR *name, unsigned long long fallback)
{
    WCHAR text[24];
    unsigned long long value = 0;
    unsigned i;

    if (!GetEnvironmentVariableW(name, text, 24)) return fallback;
    for (i = 0; text[i] >= L'0' && text[i] <= L'9'; i++) value = value * 10 + (unsigned)(text[i] - L'0');
    return value;
}

static void report(const char *verdict, const char *what, unsigned long long amount, unsigned long long steps, DWORD error)
{
    char text[200], a[24], b[24], c[24];

    number(a, amount);
    number(b, steps);
    number(c, error);
    lstrcpyA(text, "PS5_PROTON_MEM_PROBE ");
    lstrcatA(text, verdict);
    lstrcatA(text, ": ");
    lstrcatA(text, what);
    lstrcatA(text, " reached ");
    lstrcatA(text, a);
    lstrcatA(text, " MiB in ");
    lstrcatA(text, b);
    lstrcatA(text, " steps, last error ");
    lstrcatA(text, c);
    lstrcatA(text, "\n");
    say(text);
}

static void touch(volatile char *base, size_t bytes)
{
    size_t offset;
    for (offset = 0; offset < bytes; offset += 4096) base[offset] = (char)offset;
}

void mainCRTStartup(void)
{
    WCHAR mode[16];
    unsigned long long gib = env_number(L"MEM_PROBE_GIB", 6), count = env_number(L"MEM_PROBE_COUNT", 12000);
    char *base;
    unsigned long long done = 0, steps = 0;
    DWORD error = 0;

    if (!GetEnvironmentVariableW(L"MEM_PROBE_MODE", mode, 16)) lstrcpyW(mode, L"sweep");
    if (mode[0] == L'b') { /* big */
        base = VirtualAlloc(NULL, gib << 30, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        error = GetLastError();
        if (base) { touch(base, (size_t)(gib << 30)); done = gib << 10; }
        report(base ? "PASS" : "FAIL", "one commit", done, 1, base ? 0 : error);
        ExitProcess(base ? 0 : 1);
    }
    if (mode[0] == L'f') { /* frag */
        unsigned long long i, region = 4ULL << 30;
        base = VirtualAlloc(NULL, region, MEM_RESERVE, PAGE_NOACCESS);
        if (!base) { report("FAIL", "reserve", 0, 0, GetLastError()); ExitProcess(1); }
        for (i = 0; i < count; i++) {
            /* every other 64 KiB slot, so commits never touch */
            char *at = base + (i * 2) * 65536ULL;
            if (at + 65536 > base + region) break;
            if (!VirtualAlloc(at, 65536, MEM_COMMIT, PAGE_READWRITE)) { error = GetLastError(); break; }
            touch(at, 65536);
            done += 64; steps++;
        }
        report(steps == count ? "PASS" : "FAIL", "scattered 64 KiB commits", done >> 10, steps, error);
        ExitProcess(steps == count ? 0 : 1);
    }
    base = VirtualAlloc(NULL, gib << 30, MEM_RESERVE, PAGE_NOACCESS);
    if (!base) { report("FAIL", "reserve", 0, 0, GetLastError()); ExitProcess(1); }
    while (done < (gib << 10)) {
        if (!VirtualAlloc(base + (done << 20), 1 << 20, MEM_COMMIT, PAGE_READWRITE)) { error = GetLastError(); break; }
        touch(base + (done << 20), 1 << 20);
        done++;
        steps++;
    }
    report(done == (gib << 10) ? "PASS" : "FAIL", "sequential 1 MiB commits", done, steps, error);
    ExitProcess(done == (gib << 10) ? 0 : 1);
}
