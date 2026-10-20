/* SPDX-License-Identifier: GPL-3.0-or-later
 * What a thread's stack looks like and what running off the bottom of it does.
 * Windows programs rely on it: a guard page below the committed stack raises
 * STATUS_GUARD_PAGE_VIOLATION and then STATUS_STACK_OVERFLOW on the stack
 * itself, which is how the Java VM turns deep recursion into a
 * StackOverflowError and the C runtime survives _resetstkoflw.
 *
 * Prints the thread's stack limits and the protection of the pages around
 * them. With STACK_PROBE_OVERFLOW=1 a thread with a 1 MiB stack then recurses
 * until a vectored handler sees an exception, records it (code, the stack
 * pointer, the fault address) and ends the thread; the verdict says whether
 * that was a guard page or a stack overflow, and where the stack pointer was
 * relative to the stack. Ends by itself.
 *
 * Without it, the main thread then prints its own stack (VirtualQuery of every
 * 64 KiB of [StackLimit, StackBase)) and reads every page of it, and a sampler
 * thread does what Chromium's stack profiler (base::StackCopierSuspend) does
 * while the main thread makes cheap system calls for 5 s: suspend it, take its
 * context, read its stack from Rsp up to StackBase, every other time set the
 * same context back as a debugger does, resume. It fails when a suspended
 * thread's Rsp is outside [DeallocationStack, StackBase) or a system call lost
 * a register or its return value. CEF 150 read from such an Rsp into the
 * no-access page under the main thread's stack and died
 * (2026-10-16, record removed from the tree: Rsp 0x100010fc70, the system-call frame
 * + 0x70 in the Unix stack: a thread caught returning from a system call). */
#include <windows.h>
#include <intrin.h>

enum { SAMPLE_MS = 5000, SAMPLE_MIN = 1000 };
int _fltused; /* floating point without the C runtime */

static volatile LONG seen;
static volatile DWORD first_code;
static volatile ULONG_PTR first_sp, first_address, first_ip;
static volatile LONG depth;

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void hex(char *out, ULONG_PTR value)
{
    static const char digits[] = "0123456789abcdef";
    int i;
    out[0] = '0'; out[1] = 'x';
    for (i = 0; i < 16; i++) out[2 + i] = digits[(value >> (60 - 4 * i)) & 15];
    out[18] = 0;
}

static void decimal(char *out, ULONG_PTR value)
{
    char digits[24];
    int count = 0;
    do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count) *out++ = digits[--count];
    *out = 0;
}

static void line(const char *name, ULONG_PTR value)
{
    char text[96], number[24];
    lstrcpyA(text, "PS5_PROTON_STACK_PROBE ");
    lstrcatA(text, name);
    lstrcatA(text, " ");
    hex(number, value);
    lstrcatA(text, number);
    lstrcatA(text, "\n");
    say(text);
}

static void page(const char *name, ULONG_PTR address)
{
    MEMORY_BASIC_INFORMATION info;
    char text[160], number[24];

    lstrcpyA(text, "PS5_PROTON_STACK_PROBE page ");
    lstrcatA(text, name);
    lstrcatA(text, " ");
    hex(number, address);
    lstrcatA(text, number);
    if (!VirtualQuery((void *)address, &info, sizeof(info))) {
        lstrcatA(text, " unreadable\n");
        say(text);
        return;
    }
    lstrcatA(text, " state=");
    hex(number, info.State);
    lstrcatA(text, number + 14);
    lstrcatA(text, " protect=");
    hex(number, info.Protect);
    lstrcatA(text, number + 14);
    lstrcatA(text, " allocation=");
    hex(number, (ULONG_PTR)info.AllocationBase);
    lstrcatA(text, number);
    lstrcatA(text, " size=");
    decimal(number, info.RegionSize);
    lstrcatA(text, number);
    lstrcatA(text, "\n");
    say(text);
}

static LONG CALLBACK handler(EXCEPTION_POINTERS *pointers)
{
    DWORD code = pointers->ExceptionRecord->ExceptionCode;

    if (code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_GUARD_PAGE && code != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedCompareExchange(&seen, 1, 0) == 0) {
        first_code = code;
        first_sp = pointers->ContextRecord->Rsp;
        first_ip = pointers->ContextRecord->Rip;
        first_address = pointers->ExceptionRecord->NumberParameters > 1 ? pointers->ExceptionRecord->ExceptionInformation[1] : 0;
    }
    /* a guard page violation is not the end of the stack: let it continue and
     * see whether the overflow follows */
    if (code == EXCEPTION_GUARD_PAGE) return EXCEPTION_CONTINUE_EXECUTION;
    ExitThread(0x77);
    return EXCEPTION_CONTINUE_SEARCH;
}

static __attribute__((noinline)) ULONG_PTR recurse(ULONG_PTR value)
{
    volatile char frame[192];
    int i;

    for (i = 0; i < (int)sizeof(frame); i += 64) frame[i] = (char)value;
    InterlockedIncrement(&depth);
    return recurse(value + frame[0] + 1) + frame[64];
}

static DWORD WINAPI worker(void *argument)
{
    ULONG_PTR base = __readgsqword(0x08), limit = __readgsqword(0x10), allocation = __readgsqword(0x1478);

    (void)argument;
    line("stack base", base);
    line("stack limit", limit);
    line("deallocation stack", allocation);
    line("stack bytes (base - limit)", base - limit);
    line("stack pointer", (ULONG_PTR)__builtin_frame_address(0));
    page("at limit", limit);
    page("below limit", limit - 0x1000);
    page("above allocation", allocation + 0x1000);
    page("at allocation", allocation);
    page("below allocation", allocation - 0x1000);
    if (argument) {
        AddVectoredExceptionHandler(1, handler);
        recurse(1);
    }
    return 0;
}

static HANDLE main_thread;
static ULONG_PTR main_allocation, main_base;
static volatile LONG sampling_done, samples, outside;
static volatile ULONG_PTR outside_sp, outside_ip, pages_read;

static DWORD WINAPI sampler(void *argument)
{
    DWORD start = GetTickCount();
    CONTEXT context;
    ULONG_PTR address;

    (void)argument;
    while (GetTickCount() - start < SAMPLE_MS) {
        if (SuspendThread(main_thread) == (DWORD)-1) break;
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(main_thread, &context)) {
            samples++;
            if (context.Rsp < main_allocation || context.Rsp >= main_base) {
                if (!outside++) { outside_sp = context.Rsp; outside_ip = context.Rip; }
            }
            else for (address = context.Rsp & ~(ULONG_PTR)0xfff; address < main_base; address += 0x1000) {
                (void)*(volatile const char *)address;
                pages_read++;
            }
            if (samples & 1) SetThreadContext(main_thread, &context);   /* as a debugger does: it must change nothing */
        }
        ResumeThread(main_thread);
    }
    sampling_done = 1;
    return 0;
}

static ULONG_PTR step(ULONG_PTR value, int left, int right, int again)
{
    value ^= value << left;
    value ^= value >> right;
    return value ^ (value << again);
}

/* System calls until the sampler is done, with eight values and a double kept across them, in nonvolatile general and
 * XMM registers or spilled to the stack. A return that loses a register, the stack pointer or the return value shows
 * here. */
static ULONG_PTR calls_while_sampled(ULONG_PTR *lost)
{
    NTSTATUS (WINAPI *counter)(LARGE_INTEGER *, LARGE_INTEGER *) =
        (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryPerformanceCounter");
    LARGE_INTEGER now;
    ULONG_PTR a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8, kept[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    ULONG_PTR calls = 0, i, j;
    double counted = 0;

    *lost = 0;
    while (!sampling_done) {
        if (counter(&now, NULL)) ++*lost;   /* a system call with little Unix work, STATUS_SUCCESS always */
        a = step(a, 13, 7, 17); b = step(b, 5, 9, 11); c = step(c, 21, 3, 6); d = step(d, 2, 15, 25);
        e = step(e, 13, 7, 17); f = step(f, 13, 7, 17); g = step(g, 13, 7, 17); h = step(h, 13, 7, 17);
        counted += 1;
        calls++;
    }
    for (i = 0; i < calls; i++) {
        kept[0] = step(kept[0], 13, 7, 17); kept[1] = step(kept[1], 5, 9, 11);
        kept[2] = step(kept[2], 21, 3, 6); kept[3] = step(kept[3], 2, 15, 25);
        for (j = 4; j < 8; j++) kept[j] = step(kept[j], 13, 7, 17);
    }
    *lost += (a != kept[0]) + (b != kept[1]) + (c != kept[2]) + (d != kept[3]) + (e != kept[4]) + (f != kept[5]) +
             (g != kept[6]) + (h != kept[7]) + (counted != (double)calls);
    return calls;
}

/* The main thread's stack as other threads see it, while it makes system calls. */
static BOOL main_stack_sampled(void)
{
    ULONG_PTR base = __readgsqword(0x08), limit = __readgsqword(0x10), address, calls, lost;
    char text[160], number[24];
    HANDLE thread;

    main_allocation = __readgsqword(0x1478);
    main_base = base;
    line("main stack base", base);
    line("main stack limit", limit);
    line("main deallocation stack", main_allocation);
    line("main stack pointer", (ULONG_PTR)__builtin_frame_address(0));
    page("main below limit", limit - 0x1000);
    page("main at allocation", main_allocation);
    for (address = limit; address < base; address = (address + 0x10000) & ~(ULONG_PTR)0xffff) page("main", address);
    for (address = limit; address < base; address += 0x1000) (void)*(volatile const char *)address;
    line("main pages read", (base - limit) / 0x1000);

    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread, 0, FALSE,
                         DUPLICATE_SAME_ACCESS) ||
        !(thread = CreateThread(NULL, 0, sampler, NULL, 0, NULL))) {
        say("PS5_PROTON_STACK_PROBE FAIL: no sampler thread\n");
        return FALSE;
    }
    calls = calls_while_sampled(&lost);
    WaitForSingleObject(thread, INFINITE);

    lstrcpyA(text, "PS5_PROTON_STACK_PROBE samples ");
    decimal(number, (ULONG_PTR)samples);
    lstrcatA(text, number);
    lstrcatA(text, " outside the stack ");
    decimal(number, (ULONG_PTR)outside);
    lstrcatA(text, number);
    lstrcatA(text, " system calls ");
    decimal(number, calls);
    lstrcatA(text, number);
    lstrcatA(text, " pages read ");
    decimal(number, pages_read);
    lstrcatA(text, number);
    lstrcatA(text, " values or returns lost ");
    decimal(number, lost);
    lstrcatA(text, number);
    lstrcatA(text, "\n");
    say(text);
    if (outside) {
        line("first outside rsp", outside_sp);
        line("first outside rip", outside_ip);
    }
    return samples >= SAMPLE_MIN && !outside && !lost;
}

void mainCRTStartup(void)
{
    static WCHAR flag[8];
    HANDLE thread;
    DWORD result = 0;
    BOOL overflow = GetEnvironmentVariableW(L"STACK_PROBE_OVERFLOW", flag, 8) != 0;
    char text[160], number[24];

    thread = CreateThread(NULL, 0x100000, worker, overflow ? (void *)1 : NULL, 0, NULL);
    if (!thread || WaitForSingleObject(thread, 20000) != WAIT_OBJECT_0) {
        say("PS5_PROTON_STACK_PROBE FAIL: the thread did not finish\n");
        ExitProcess(1);
    }
    GetExitCodeThread(thread, &result);
    if (!overflow) {
        if (!main_stack_sampled()) {
            say("PS5_PROTON_STACK_PROBE FAIL: a suspended main thread's stack pointer was outside its stack, "
                "a system call lost a value or its return, or too few samples\n");
            ExitProcess(1);
        }
        say("PS5_PROTON_STACK_PROBE PASS: every suspended main-thread context was on its stack, every page read, "
            "nothing lost\n");
        ExitProcess(0);
    }
    lstrcpyA(text, "PS5_PROTON_STACK_PROBE recursion depth ");
    decimal(number, (ULONG_PTR)depth);
    lstrcatA(text, number);
    lstrcatA(text, "\n");
    say(text);
    line("first exception code", first_code);
    line("stack pointer at the exception", first_sp);
    line("instruction pointer", first_ip);
    line("fault address", first_address);
    /* the handler ended the thread: any exception seen while the stack pointer
     * was inside the stack's range is a clean overflow */
    say(seen && result == 0x77 && first_code == EXCEPTION_STACK_OVERFLOW
            ? "PS5_PROTON_STACK_PROBE PASS: the overflow was reported as a stack overflow\n"
            : "PS5_PROTON_STACK_PROBE FAIL: the overflow was not reported as a stack overflow\n");
    ExitProcess(seen && first_code == EXCEPTION_STACK_OVERFLOW ? 0 : 1);
}
