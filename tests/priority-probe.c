/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Windows thread priorities on the console, as a game would see them.
 *
 * Checks that priorities round-trip, then measures under full load (twice as
 * many busy NORMAL threads as CPUs) for one second each:
 *   equal          a NORMAL thread with a 2 ms period: Windows threads of
 *                  equal priority take turns, so it must keep running;
 *   time-critical  the same thread at THREAD_PRIORITY_TIME_CRITICAL: it must
 *                  wake close to its deadlines;
 *   below/idle     a BELOW_NORMAL and an IDLE busy thread: they must progress;
 *   inversion      a TIME_CRITICAL thread waits for a critical section a
 *                  BELOW_NORMAL thread holds while it works: bounded wait.
 * Every busy thread stops by its own clock after the second, so a scheduler
 * that starves threads makes a phase fail, never hang.
 *
 * "priority-probe.exe ramp L spin_ms" (C3 of the 2026-10-14 review, src/pw_helper_sched.h) runs only the equal phase,
 * as steps n = 1..L (L <= 64, spin_ms 100..2000): a witness that wakes every 2 ms, then n threads spinning on the
 * time-stamp counter with pause and no system call, as an emulator's spin guard does, to one deadline spin_ms away, so each
 * ends by itself. The witness runs 1 s past the deadline, so one that could not run at all still wakes and says for
 * how long. The ramp stops at the first step whose witness waited spin_ms/2 or more, or whose spinners did not end
 * within 3 s of their deadline: equal threads did not share, which fails the probe. Each step's begin and end are in
 * QueryPerformanceCounter ms, CLOCK_MONOTONIC on the console as the title's log is. Then 3 s for the system before the
 * next step.
 *
 * "PS5_PROTON_PRIORITY_PROBE bench name=value" lines, then PASS or FAIL. */
#include <windows.h>

/* No C runtime: the compiler may still turn a copy loop into memcpy. */
void *memcpy(void *destination, const void *source, size_t size)
{
    char *to = destination;
    const char *from = source;
    while (size--) *to++ = *from++;
    return destination;
}

static char out[2048];
static unsigned used, failures;
static LARGE_INTEGER frequency;

static void put(const char *text) { while (*text && used < sizeof(out) - 1) out[used++] = *text++; }
static void put_unsigned(unsigned long long value)
{
    char digits[24];
    int count = 0;
    do digits[count++] = '0' + value % 10; while ((value /= 10) && count < 24);
    while (count) { char c[2] = {digits[--count], 0}; put(c); }
}
static void flush(void)
{
    DWORD written;
    out[used] = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), out, used, &written, NULL);
    OutputDebugStringA(out);
    used = 0;
}
static void report(const char *name, unsigned long long value)
{
    put("PS5_PROTON_PRIORITY_PROBE bench ");
    put(name);
    put("=");
    put_unsigned(value);
    put("\r\n");
    flush();
}
static void check(BOOL ok, const char *name)
{
    if (ok) return;
    failures++;
    put("PS5_PROTON_PRIORITY_PROBE check failed: ");
    put(name);
    put("\r\n");
    flush();
}
static unsigned long long now_us(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart * 1000000ULL / frequency.QuadPart;
}

static unsigned long long phase_end;    /* every thread of a phase stops here */
static HANDLE start_gate;              /* do not measure thread creation */

static DWORD WINAPI busy(void *counter)
{
    volatile unsigned long long *count = counter;
    WaitForSingleObject(start_gate, INFINITE);
    while (now_us() < phase_end) for (int i = 0; i < 1000; i++) (*count)++;
    return 0;
}

/* Wakes every 2 ms until the phase ends; records how late each wake was. */
static unsigned periods;
static unsigned lateness[1000];
static DWORD WINAPI periodic(void *unused)
{
    unsigned long long next;
    (void)unused;
    WaitForSingleObject(start_gate, INFINITE);
    next = now_us() + 2000;
    periods = 0;
    while (now_us() < phase_end && periods < 1000) {
        unsigned long long now;
        while ((now = now_us()) < next) {
            const unsigned long long left = next - now;
            if (left > 1500) Sleep((DWORD)(left / 1000 - 1));
            else SwitchToThread();
        }
        lateness[periods++] = (unsigned)(now - next);
        next += 2000;
    }
    return 0;
}

static unsigned percentile(unsigned percent)
{
    unsigned sorted[1000];
    if (!periods) return 0;
    for (unsigned i = 0; i < periods; i++) sorted[i] = lateness[i];
    for (unsigned i = 1; i < periods; i++)
        for (unsigned j = i; j > 0 && sorted[j] < sorted[j - 1]; j--) {
            unsigned t = sorted[j]; sorted[j] = sorted[j - 1]; sorted[j - 1] = t;
        }
    unsigned index = (periods * percent + 99) / 100;
    return sorted[index ? index - 1 : 0];
}

static unsigned busy_threads;
static volatile unsigned long long counters[64];
static HANDLE threads[64];

static void start_load(void)
{
    start_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    check(start_gate != NULL, "create-start-gate");
    for (unsigned i = 0; i < busy_threads; i++) {
        counters[i] = 0;
        threads[i] = CreateThread(NULL, 0, busy, (void *)&counters[i], 0, NULL);
        check(threads[i] != NULL, "create-load-thread");
    }
}
static void release_load(unsigned milliseconds)
{
    phase_end = now_us() + milliseconds * 1000ULL;
    check(SetEvent(start_gate), "release-start-gate");
}
static void end_load(void)
{
    WaitForMultipleObjects(busy_threads, threads, TRUE, 10000);
    for (unsigned i = 0; i < busy_threads; i++) CloseHandle(threads[i]);
    CloseHandle(start_gate);
}

static void periodic_phase(const char *name, int priority)
{
    HANDLE ticker;
    start_load();
    ticker = CreateThread(NULL, 0, periodic, NULL, CREATE_SUSPENDED, NULL);
    SetThreadPriority(ticker, priority);
    ResumeThread(ticker);
    release_load(1000);
    WaitForSingleObject(ticker, 10000);
    CloseHandle(ticker);
    end_load();
    put("PS5_PROTON_PRIORITY_PROBE bench ");
    put(name);
    put("_periods=");
    put_unsigned(periods);
    put(" ");
    put(name);
    put("_p50_us=");
    put_unsigned(percentile(50));
    put(" ");
    put(name);
    put("_p99_us=");
    put_unsigned(percentile(99));
    put("\r\n");
    flush();
}

static CRITICAL_SECTION section;
static volatile LONG holder_ready;
static DWORD WINAPI holder(void *unused)
{
    volatile unsigned long long work = 0;
    (void)unused;
    EnterCriticalSection(&section);
    InterlockedExchange(&holder_ready, 1);
    /* 50 ms of work, or until the phase ends */
    const unsigned long long until = now_us() + 50000;
    while (now_us() < until && now_us() < phase_end) for (int i = 0; i < 1000; i++) work++;
    LeaveCriticalSection(&section);
    return 0;
}

/* The ramp (see the top). */
static unsigned long long witness_late_max_us, tsc_per_ms, spin_deadline;
static unsigned witness_wakes;
static DWORD WINAPI witness(void *unused)
{
    unsigned long long next = now_us() + 2000;
    (void)unused;
    for (;;) {
        unsigned long long now;
        while ((now = now_us()) < next) {
            const unsigned long long left = next - now;
            if (left > 1500) Sleep((DWORD)(left / 1000 - 1));
            else SwitchToThread();
        }
        if (now - next > witness_late_max_us) witness_late_max_us = now - next;
        witness_wakes++;
        if (now >= phase_end) return 0;
        next = now + 2000;
    }
}
static DWORD WINAPI spinner(void *unused)
{
    (void)unused;
    while (__builtin_ia32_rdtsc() < spin_deadline) __builtin_ia32_pause();
    return 0;
}
static void put_step(unsigned n, const char *name, unsigned long long value)
{
    put(" step");
    put_unsigned(n);
    put("_");
    put(name);
    put("=");
    put_unsigned(value);
}
static void ramp(unsigned last, unsigned spin_ms)
{
    unsigned stall = 0;
    {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        const unsigned long long first = __builtin_ia32_rdtsc();
        Sleep(200);
        const unsigned long long second = __builtin_ia32_rdtsc();
        QueryPerformanceCounter(&b);
        const unsigned long long us = (b.QuadPart - a.QuadPart) * 1000000ULL / frequency.QuadPart;
        tsc_per_ms = us ? (second - first) * 1000 / us : 0;
    }
    report("ramp_tsc_mhz", tsc_per_ms / 1000);
    /* A counter that does not tick near the CPU's rate would make the deadline wrong: no spinner starts. */
    check(tsc_per_ms >= 500000 && tsc_per_ms <= 10000000, "ramp-tsc-calibrated");
    for (unsigned n = 1; n <= last && !failures; n++) {
        HANDLE watcher, spinners[64];
        unsigned started = 0, late_end = 0;
        const unsigned long long begin = now_us();
        witness_late_max_us = 0;
        witness_wakes = 0;
        phase_end = begin + (spin_ms + 1000) * 1000ULL;
        watcher = CreateThread(NULL, 0, witness, NULL, 0, NULL);
        check(watcher != NULL, "ramp-witness");
        while (started < n && (spinners[started] = CreateThread(NULL, 0, spinner, NULL, CREATE_SUSPENDED, NULL))) started++;
        check(started == n, "ramp-spinners");
        spin_deadline = __builtin_ia32_rdtsc() + spin_ms * tsc_per_ms;
        for (unsigned i = 0; i < started; i++) ResumeThread(spinners[i]);
        if (started && WaitForMultipleObjects(started, spinners, TRUE, spin_ms + 3000) == WAIT_TIMEOUT) {
            late_end = 1;
            WaitForMultipleObjects(started, spinners, TRUE, 10000);
        }
        if (watcher) {
            check(WaitForSingleObject(watcher, spin_ms + 13000) == WAIT_OBJECT_0, "ramp-witness-ended");
            CloseHandle(watcher);
        }
        for (unsigned i = 0; i < started; i++) CloseHandle(spinners[i]);
        put("PS5_PROTON_PRIORITY_PROBE bench");
        put_step(n, "begin_ms", begin / 1000);
        put_step(n, "end_ms", now_us() / 1000);
        put_step(n, "witness_wakes", witness_wakes);
        put_step(n, "witness_late_max_ms", witness_late_max_us / 1000);
        put_step(n, "late_end", late_end);
        put("\r\n");
        flush();
        if (late_end || witness_late_max_us / 1000 >= spin_ms / 2) { stall = n; break; }
        Sleep(3000);
    }
    report("stall_n", stall);
    check(!stall, "ramp-equal-threads-share");
}

/* "ramp L spin_ms" after the program's name, or 0 when the command line is not that (the default probe runs). */
static unsigned next_number(const char **text)
{
    unsigned value = 0, digits = 0;
    while (**text == ' ' || **text == '\t') ++*text;
    while (**text >= '0' && **text <= '9' && digits++ < 6) value = value * 10 + (unsigned)(*(*text)++ - '0');
    return digits ? value : ~0u;
}
static int ramp_arguments(unsigned *last, unsigned *spin_ms)
{
    const char *line = GetCommandLineA();
    if (*line == '"') {
        for (++line; *line && *line != '"'; ++line) {}
        if (*line) ++line;
    } else {
        while (*line && *line != ' ' && *line != '\t') ++line;
    }
    while (*line == ' ' || *line == '\t') ++line;
    if (line[0] != 'r' || line[1] != 'a' || line[2] != 'm' || line[3] != 'p' || (line[4] != ' ' && line[4] != '\t'))
        return 0;
    line += 4;
    *last = next_number(&line);
    *spin_ms = next_number(&line);
    while (*line == ' ' || *line == '\t') ++line;
    return *line ? -1 : 1;
}

void mainCRTStartup(void)
{
    static const int levels[] = {THREAD_PRIORITY_IDLE, THREAD_PRIORITY_LOWEST, THREAD_PRIORITY_BELOW_NORMAL,
                                 THREAD_PRIORITY_NORMAL, THREAD_PRIORITY_ABOVE_NORMAL, THREAD_PRIORITY_HIGHEST,
                                 THREAD_PRIORITY_TIME_CRITICAL};
    SYSTEM_INFO info;
    unsigned last = 0, spin_ms = 0;
    QueryPerformanceFrequency(&frequency);
    GetSystemInfo(&info);
    busy_threads = info.dwNumberOfProcessors * 2;
    if (busy_threads > 64) busy_threads = 64;
    report("cpus", info.dwNumberOfProcessors);
    const int ramp_mode = ramp_arguments(&last, &spin_ms);
    if (ramp_mode) {
        check(ramp_mode > 0 && last >= 1 && last <= 64 && spin_ms >= 100 && spin_ms <= 2000, "ramp-arguments");
        if (!failures) ramp(last, spin_ms);
        put(failures ? "PS5_PROTON_PRIORITY_PROBE FAIL\r\n"
                     : "PS5_PROTON_PRIORITY_PROBE PASS: ramp, equal threads shared at every step\r\n");
        flush();
        ExitProcess(failures ? 1 : 0);
    }

    for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        check(SetThreadPriority(GetCurrentThread(), levels[i]), "set-priority");
        check(GetThreadPriority(GetCurrentThread()) == levels[i], "get-priority");
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);

    /* The probe's own thread outranks the load, so it is not part of it. */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    periodic_phase("equal", THREAD_PRIORITY_NORMAL);
    check(periods >= 300, "equal-priority-threads-share");
    periodic_phase("time_critical", THREAD_PRIORITY_TIME_CRITICAL);
    check(periods >= 400 && percentile(99) < 1000, "time-critical-wakes-on-time");

    {
        volatile unsigned long long below = 0, idle = 0;
        HANDLE low[2];
        start_load();
        low[0] = CreateThread(NULL, 0, busy, (void *)&below, CREATE_SUSPENDED, NULL);
        low[1] = CreateThread(NULL, 0, busy, (void *)&idle, CREATE_SUSPENDED, NULL);
        SetThreadPriority(low[0], THREAD_PRIORITY_BELOW_NORMAL);
        SetThreadPriority(low[1], THREAD_PRIORITY_IDLE);
        ResumeThread(low[0]);
        ResumeThread(low[1]);
        release_load(1000);
        WaitForMultipleObjects(2, low, TRUE, 10000);
        end_load();
        CloseHandle(low[0]);
        CloseHandle(low[1]);
        unsigned long long normal = 0;
        for (unsigned i = 0; i < busy_threads; i++) normal += counters[i];
        normal /= busy_threads;
        report("below_normal_permille_of_normal", normal ? below * 1000 / normal : 0);
        report("idle_permille_of_normal", normal ? idle * 1000 / normal : 0);
        check(below > 0 && idle > 0, "lowered-threads-progress");
    }

    {
        HANDLE owner;
        unsigned long long waited;
        BOOL entered;
        InitializeCriticalSection(&section);
        holder_ready = 0;
        /* The load runs first; then a BELOW_NORMAL thread takes the lock and
         * works while this TIME_CRITICAL thread waits for it. */
        start_load();
        release_load(2000);
        owner = CreateThread(NULL, 0, holder, NULL, CREATE_SUSPENDED, NULL);
        SetThreadPriority(owner, THREAD_PRIORITY_BELOW_NORMAL);
        ResumeThread(owner);
        while (!holder_ready && now_us() < phase_end) Sleep(1);
        const unsigned long long start = now_us();
        while (!(entered = TryEnterCriticalSection(&section)) && now_us() < phase_end) Sleep(1);
        waited = now_us() - start;
        report("inversion_holder_started", holder_ready);
        if (entered) LeaveCriticalSection(&section);
        check(holder_ready && entered, "inversion-holder-ran");
        end_load();
        WaitForSingleObject(owner, 10000);
        CloseHandle(owner);
        DeleteCriticalSection(&section);
        report("inversion_wait_us", waited);
        check(waited >= 40000 && waited < 1000000, "inversion-bounded");
    }

    put(failures ? "PS5_PROTON_PRIORITY_PROBE FAIL\r\n"
                 : "PS5_PROTON_PRIORITY_PROBE PASS: round trips, sharing, time-critical, lowered progress, inversion\r\n");
    flush();
    ExitProcess(failures ? 1 : 0);
}
