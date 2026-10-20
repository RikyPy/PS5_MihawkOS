/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A 32-bit (i386) Windows program under Wine's WoW64 on the console (docs/IA32_DESIGN.md, M3.4). Each check prints
 * one line, to standard output and OutputDebugString; the last is "PS5_PROTON_PE32_PROBE PASS" or "... FAIL",
 * which tools/run-console.py reads.
 *
 *   mode      running as 32-bit code: CS is a 32-bit selector (0x33 on the console, 0x23 on Linux, where the probe
 *             is checked first) and fs:[0x18] is this TEB
 *   syscalls  system calls from 32-bit code return (each crosses to the 64-bit side and back), and what one costs
 *   seh       an access violation, an integer divide by zero and an int3, each caught by a vectored handler that
 *             sees the faulting address and resumes after the instruction
 *   threads   four threads, each seeing its own TEB through %fs and its own TLS slot value; their TEBs are
 *             printed (on the console a 32-bit TEB is at the low half of its thread pointer: 0xff8.....)
 *   fpu       x87 and SSE values kept across system calls
 * Built by tools/build-wine-probe.sh with clang --target=i686-windows, no C runtime. */
#include <windows.h>

static HANDLE out;
int _fltused;   /* no C runtime: floating point needs the marker it would define */

/* Standard output, and the debugger channel too: a helper process's verdict reaches the run's log through
 * +debugstr, as the 64-bit probe's does (tests/wine-probe.c). */
static void put(const char *text)
{
    DWORD written, n = 0;
    while (text[n]) n++;
    WriteFile(out, text, n, &written, NULL);
    OutputDebugStringA(text);
}

static void put_hex(unsigned value)
{
    char text[11] = "0x";
    for (int i = 0; i < 8; i++) text[2 + i] = "0123456789abcdef"[(value >> (28 - 4 * i)) & 15];
    text[10] = 0;
    put(text);
}

static void put_dec(unsigned value)
{
    char text[12];
    int n = 11;
    text[n] = 0;
    do { text[--n] = (char)('0' + value % 10); value /= 10; } while (value);
    put(text + n);
}

static unsigned read_cs(void) { unsigned cs; __asm__ volatile("movl %%cs,%0" : "=r"(cs)); return cs & 0xffff; }
static void *teb_self(void) { void *self; __asm__ volatile("movl %%fs:0x18,%0" : "=r"(self)); return self; }
/* A 32-bit code selector: the console's (FreeBSD GUCODE32, 0x33) or Linux's (0x23), where the probe is checked too. */
static int cs_is_32bit(unsigned cs) { return cs == 0x33 || cs == 0x23; }

/* ---------------------------------------------------------------- seh */

static volatile LONG seen_av, seen_div, seen_bp;
static volatile DWORD av_address;

static LONG CALLBACK handler(EXCEPTION_POINTERS *e)
{
    CONTEXT *c = e->ContextRecord;
    switch (e->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        av_address = (DWORD)e->ExceptionRecord->ExceptionInformation[1];
        seen_av++;
        c->Eip += 3;            /* movl 8(%eax),%eax */
        return EXCEPTION_CONTINUE_EXECUTION;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        seen_div++;
        c->Eip += 2;            /* divl %ecx */
        return EXCEPTION_CONTINUE_EXECUTION;
    case EXCEPTION_BREAKPOINT:
        seen_bp++;
        c->Eip += 1;            /* int3: Windows reports the instruction's own address */
        return EXCEPTION_CONTINUE_EXECUTION;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

static int check_seh(void)
{
    AddVectoredExceptionHandler(1, handler);
    unsigned result;
    __asm__ volatile("movl $0, %%eax\n movl 8(%%eax), %%eax" : "=a"(result) :: "memory");
    __asm__ volatile("xorl %%edx,%%edx\n movl $7,%%eax\n xorl %%ecx,%%ecx\n divl %%ecx" : "=a"(result) :: "ecx", "edx");
    __asm__ volatile("int3" ::: "memory");
    put("seh av="); put_dec(seen_av); put(" at="); put_hex(av_address);
    put(" div="); put_dec(seen_div); put(" bp="); put_dec(seen_bp); put("\r\n");
    return seen_av == 1 && av_address == 8 && seen_div == 1 && seen_bp == 1;
}

/* ---------------------------------------------------------------- threads */

static DWORD slot;
static volatile LONG thread_ok;
static void *volatile thread_teb[4];

static DWORD WINAPI worker(void *arg)
{
    const DWORD value = (DWORD)(ULONG_PTR)arg;
    thread_teb[value - 1] = teb_self();
    TlsSetValue(slot, (void *)(ULONG_PTR)(value * 3));
    for (int i = 0; i < 50; i++) Sleep(0);
    if (teb_self() == NtCurrentTeb() && (DWORD)(ULONG_PTR)TlsGetValue(slot) == value * 3 && cs_is_32bit(read_cs()))
        InterlockedIncrement(&thread_ok);
    return 0;
}

static int check_threads(void)
{
    HANDLE threads[4];
    slot = TlsAlloc();
    for (int i = 0; i < 4; i++) threads[i] = CreateThread(NULL, 0, worker, (void *)(ULONG_PTR)(i + 1), 0, NULL);
    WaitForMultipleObjects(4, threads, TRUE, 30000);
    for (int i = 0; i < 4; i++) CloseHandle(threads[i]);
    put("threads ok="); put_dec(thread_ok); put(" of 4 tebs=");
    for (int i = 0; i < 4; i++) { put_hex((unsigned)(ULONG_PTR)thread_teb[i]); put(i < 3 ? "," : "\r\n"); }
    return thread_ok == 4;
}

/* ---------------------------------------------------------------- fpu */

static int check_fpu(void)
{
    static const float pattern[4] = { 1.5f, -2.25f, 3.125f, 4.0f };
    float back[4];
    double x87 = 0;
    __asm__ volatile("movups %0, %%xmm7" :: "m"(pattern) : "xmm7");
    __asm__ volatile("fldpi" ::: "st");
    for (int i = 0; i < 20; i++) Sleep(0);
    __asm__ volatile("movups %%xmm7, %0" : "=m"(back) :: "memory");
    __asm__ volatile("fstpl %0" : "=m"(x87) :: "st");
    const int ok = back[0] == 1.5f && back[1] == -2.25f && back[2] == 3.125f && back[3] == 4.0f &&
                   x87 > 3.14159 && x87 < 3.1416;
    put("fpu sse_kept="); put_dec(back[0] == 1.5f && back[3] == 4.0f); put(" x87_kept="); put_dec(x87 > 3.14159 && x87 < 3.1416);
    put("\r\n");
    return ok;
}

int mainCRTStartup(void)
{
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    const unsigned cs = read_cs();
    void *self = teb_self();
    const int mode = sizeof(void *) == 4 && cs_is_32bit(cs) && self == NtCurrentTeb();
    put("mode cs="); put_hex(cs); put(" fs_self="); put_hex((unsigned)(ULONG_PTR)self);
    put(" teb="); put_hex((unsigned)(ULONG_PTR)NtCurrentTeb()); put("\r\n");

    LARGE_INTEGER frequency, t0, t1;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 2000; i++) Sleep(0);
    QueryPerformanceCounter(&t1);
    const unsigned ns = (unsigned)((t1.QuadPart - t0.QuadPart) * 1000000000ull / (unsigned long long)frequency.QuadPart / 2000);
    put("syscalls sleep0_ns="); put_dec(ns); put("\r\n");

    const int seh = check_seh();
    const int threads = check_threads();
    const int fpu = check_fpu();
    const int pass = mode && seh && threads && fpu;
    put(pass ? "PS5_PROTON_PE32_PROBE PASS: 32-bit mode, system calls, SEH, threads and TLS, FPU\r\n"
             : "PS5_PROTON_PE32_PROBE FAIL\r\n");
    /* Keep Wine's handlers installed for the native SDK thread's routing check. */
    char native_signals[8];
    if (GetEnvironmentVariableA("WINE_PS5_NATIVE_SIGNALS", native_signals, sizeof(native_signals)) &&
        native_signals[0] == '1') Sleep(15000);
    ExitProcess(pass ? 0 : 1);
    return 0;
}
