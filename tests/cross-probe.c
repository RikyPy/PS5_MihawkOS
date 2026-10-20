/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What one crossing from a Windows program to Wine's Unix side costs, by kind (docs/IA32_DESIGN.md, M4 step 1).
 * The same source is built as cross32-probe.exe (i386, under WoW64) and cross64-probe.exe (x86_64), so the two
 * differ only in the WoW64 path, and both run under Wine on Linux too, the oracle. Each kind is timed over CALLS
 * calls and printed as one line "cross bits=<32|64> <kind>_ns=<ns per call> ..."; the last line is
 * "PS5_PROTON_CROSS_PROBE PASS" when every call returned what it should, else "... FAIL <kind>".
 *
 *   cpu      NtGetCurrentProcessorNumber: a system call with the least Unix work
 *   counter  NtQueryPerformanceCounter: a system call that reads the clock
 *   yield    NtYieldExecution: a system call that asks the scheduler
 *   close    NtClose on a handle that does not exist: a wineserver request
 *   sleep0   Sleep(0), what pe32-probe timed (kernel32, then NtDelayExecution)
 * Built by tools/build-wine-probe.sh with clang, no C runtime. */
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>

enum { CALLS = 4000 };
int _fltused;   /* no C runtime: floating point needs the marker it would define */

static HANDLE out;
static char line[256];
static unsigned used;

static void add(const char *text)
{
    while (*text && used < sizeof(line) - 1) line[used++] = *text++;
    line[used] = 0;
}

static void add_dec(unsigned value)
{
    char text[12];
    int n = 11;
    text[n] = 0;
    do { text[--n] = (char)('0' + value % 10); value /= 10; } while (value);
    add(text + n);
}

/* One line to standard output and to the debugger channel: a helper's verdict reaches the run's log through
 * +debugstr (pe32-probe.c). */
static void flush_line(void)
{
    DWORD written;
    add("\r\n");
    WriteFile(out, line, used, &written, NULL);
    OutputDebugStringA(line);
    used = 0;
    line[0] = 0;
}

static LARGE_INTEGER frequency, started;

static void start(void) { QueryPerformanceCounter(&started); }

/* Nanoseconds per call since start(). */
static unsigned stop(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (unsigned)((unsigned long long)(now.QuadPart - started.QuadPart) * 1000000000ull /
                      (unsigned long long)frequency.QuadPart / CALLS);
}

int mainCRTStartup(void)
{
    const char *failed = NULL;
    unsigned cpu, counter, yield, close, sleep0;
    LARGE_INTEGER value;
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    QueryPerformanceFrequency(&frequency);

    start();
    for (int i = 0; i < CALLS; i++)
        if (NtGetCurrentProcessorNumber() > 4096) failed = "cpu";
    cpu = stop();

    start();
    for (int i = 0; i < CALLS; i++)
        if (NtQueryPerformanceCounter(&value, NULL)) failed = "counter";
    counter = stop();

    start();
    for (int i = 0; i < CALLS; i++) {
        const NTSTATUS status = NtYieldExecution();
        if (status != STATUS_SUCCESS && status != STATUS_NO_YIELD_PERFORMED) failed = "yield";
    }
    yield = stop();

    start();
    for (int i = 0; i < CALLS; i++)
        if (NtClose((HANDLE)(ULONG_PTR)0x7ffffff0) != STATUS_INVALID_HANDLE) failed = "close";
    close = stop();

    start();
    for (int i = 0; i < CALLS; i++) Sleep(0);
    sleep0 = stop();

    add("cross bits="); add_dec(sizeof(void *) * 8);
    add(" calls="); add_dec(CALLS);
    add(" cpu_ns="); add_dec(cpu);
    add(" counter_ns="); add_dec(counter);
    add(" yield_ns="); add_dec(yield);
    add(" close_ns="); add_dec(close);
    add(" sleep0_ns="); add_dec(sleep0);
    flush_line();
    if (failed) { add("PS5_PROTON_CROSS_PROBE FAIL "); add(failed); }
    else add("PS5_PROTON_CROSS_PROBE PASS: system calls and a server request timed");
    flush_line();
    ExitProcess(failed ? 1 : 0);
    return 0;
}
