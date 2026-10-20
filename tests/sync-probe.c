/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Windows synchronization on the console: semantics first, then cost.
 *
 * Every check names itself on failure; the verdict line is
 * "PS5_PROTON_SYNC_PROBE PASS" or "... FAIL <checks>". The measurements
 * follow as "PS5_PROTON_SYNC_PROBE bench <name>=<value>" lines (microseconds
 * per operation unless named otherwise), so a server-path run and a
 * fast-path run of the same build compare line by line.
 *
 * Kernel32 only, no C runtime: the probe runs in the bare probe prefix. */
#include <windows.h>

static char out[4096];
static unsigned used;
static unsigned failures;
static char failed[1024];
static unsigned failed_used;
static LARGE_INTEGER frequency;

static void put(const char *text)
{
    while (*text && used < sizeof(out) - 1) out[used++] = *text++;
}
static void put_unsigned(unsigned long long value)
{
    char digits[24];
    int count = 0;
    do digits[count++] = '0' + value % 10; while ((value /= 10) && count < 24);
    while (count) {
        char c[2] = {digits[--count], 0};
        put(c);
    }
}
/* value in thousandths, printed with three decimals */
static void put_milli(unsigned long long thousandths)
{
    put_unsigned(thousandths / 1000);
    put(".");
    char frac[4] = {'0' + thousandths / 100 % 10, '0' + thousandths / 10 % 10, '0' + thousandths % 10, 0};
    put(frac);
}
static void flush(void)
{
    DWORD written;
    out[used] = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), out, used, &written, NULL);
    OutputDebugStringA(out);
    used = 0;
}
static void check(BOOL ok, const char *name)
{
    if (ok) return;
    failures++;
    while (*name && failed_used < sizeof(failed) - 2) failed[failed_used++] = *name++;
    failed[failed_used++] = ' ';
    put("PS5_PROTON_SYNC_PROBE check failed: ");
    put(name);
    put("\r\n");
    flush();
}
static unsigned long long now_ticks(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}
static unsigned long long ticks_to_ns(unsigned long long ticks)
{
    return ticks * 1000000000ULL / frequency.QuadPart;
}
static void bench(const char *name, unsigned long long ticks, unsigned long long operations)
{
    /* nanoseconds per operation, printed as microseconds */
    put("PS5_PROTON_SYNC_PROBE bench ");
    put(name);
    put("=");
    put_milli(operations ? ticks_to_ns(ticks) / operations : 0);
    put("\r\n");
    flush();
}

/* ---- semantics ---------------------------------------------------------- */

static void test_events(void)
{
    HANDLE manual = CreateEventW(NULL, TRUE, FALSE, NULL), automatic = CreateEventW(NULL, FALSE, TRUE, NULL);
    check(manual && automatic, "event-create");
    check(WaitForSingleObject(manual, 0) == WAIT_TIMEOUT, "manual-starts-clear");
    SetEvent(manual);
    check(WaitForSingleObject(manual, 0) == WAIT_OBJECT_0 && WaitForSingleObject(manual, 0) == WAIT_OBJECT_0,
          "manual-stays-set");
    ResetEvent(manual);
    check(WaitForSingleObject(manual, 0) == WAIT_TIMEOUT, "manual-reset");
    check(WaitForSingleObject(automatic, 0) == WAIT_OBJECT_0, "auto-initial");
    check(WaitForSingleObject(automatic, 0) == WAIT_TIMEOUT, "auto-consumed");
    PulseEvent(manual);   /* no waiter: stays clear */
    check(WaitForSingleObject(manual, 0) == WAIT_TIMEOUT, "pulse-no-waiter");
    CloseHandle(manual);
    CloseHandle(automatic);
}

static void test_semaphores(void)
{
    HANDLE sem = CreateSemaphoreW(NULL, 2, 3, NULL);
    LONG previous = -1;
    check(sem != NULL, "sem-create");
    check(WaitForSingleObject(sem, 0) == WAIT_OBJECT_0 && WaitForSingleObject(sem, 0) == WAIT_OBJECT_0,
          "sem-two-units");
    check(WaitForSingleObject(sem, 0) == WAIT_TIMEOUT, "sem-empty");
    check(ReleaseSemaphore(sem, 2, &previous) && previous == 0, "sem-release");
    previous = -1;
    check(!ReleaseSemaphore(sem, 2, &previous) && GetLastError() == ERROR_TOO_MANY_POSTS, "sem-over-max");
    check(ReleaseSemaphore(sem, 1, &previous) && previous == 2, "sem-count-kept");
    CloseHandle(sem);
}

static HANDLE abandon_mutex;
static DWORD WINAPI take_and_exit(void *unused)
{
    (void)unused;
    WaitForSingleObject(abandon_mutex, INFINITE);
    return 0;
}
static DWORD WINAPI try_release(void *mutex)
{
    return ReleaseMutex(mutex) ? 1 : GetLastError();
}

static void test_mutexes(void)
{
    HANDLE mutex = CreateMutexW(NULL, FALSE, NULL), thread;
    DWORD code = 0;
    check(mutex != NULL, "mutex-create");
    check(WaitForSingleObject(mutex, 0) == WAIT_OBJECT_0 && WaitForSingleObject(mutex, 0) == WAIT_OBJECT_0,
          "mutex-recursive");
    thread = CreateThread(NULL, 0, try_release, mutex, 0, NULL);
    WaitForSingleObject(thread, INFINITE);
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    check(code == ERROR_NOT_OWNER, "mutex-foreign-release");
    check(ReleaseMutex(mutex) && ReleaseMutex(mutex), "mutex-release-twice");
    check(!ReleaseMutex(mutex) && GetLastError() == ERROR_NOT_OWNER, "mutex-not-owned");
    /* abandonment: the owner exits holding it */
    abandon_mutex = CreateMutexW(NULL, FALSE, NULL);
    thread = CreateThread(NULL, 0, take_and_exit, NULL, 0, NULL);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    check(WaitForSingleObject(abandon_mutex, 1000) == WAIT_ABANDONED, "mutex-abandoned");
    check(WaitForSingleObject(abandon_mutex, 0) == WAIT_OBJECT_0, "mutex-owned-after-abandon");
    ReleaseMutex(abandon_mutex);
    ReleaseMutex(abandon_mutex);
    CloseHandle(abandon_mutex);
    CloseHandle(mutex);
}

static void test_wait_multiple(void)
{
    HANDLE events[4], all[2], mixed[2];
    for (int i = 0; i < 4; i++) events[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
    SetEvent(events[2]);
    SetEvent(events[3]);
    check(WaitForMultipleObjects(4, events, FALSE, 0) == WAIT_OBJECT_0 + 2, "wait-any-lowest-index");
    for (int i = 0; i < 4; i++) CloseHandle(events[i]);

    /* wait-all is atomic: with one of two auto-reset events set it times out
     * and must not have consumed the set one */
    all[0] = CreateEventW(NULL, FALSE, TRUE, NULL);
    all[1] = CreateEventW(NULL, FALSE, FALSE, NULL);
    check(WaitForMultipleObjects(2, all, TRUE, 20) == WAIT_TIMEOUT, "wait-all-incomplete");
    check(WaitForSingleObject(all[0], 0) == WAIT_OBJECT_0, "wait-all-consumed-nothing");
    SetEvent(all[0]);
    SetEvent(all[1]);
    check(WaitForMultipleObjects(2, all, TRUE, 0) == WAIT_OBJECT_0, "wait-all-complete");
    check(WaitForSingleObject(all[0], 0) == WAIT_TIMEOUT && WaitForSingleObject(all[1], 0) == WAIT_TIMEOUT,
          "wait-all-consumed-both");
    CloseHandle(all[0]);
    CloseHandle(all[1]);

    /* a mutex and a semaphore in one wait-all */
    mixed[0] = CreateMutexW(NULL, FALSE, NULL);
    mixed[1] = CreateSemaphoreW(NULL, 1, 1, NULL);
    check(WaitForMultipleObjects(2, mixed, TRUE, 0) == WAIT_OBJECT_0, "wait-all-mutex-semaphore");
    check(WaitForSingleObject(mixed[1], 0) == WAIT_TIMEOUT, "wait-all-took-semaphore");
    check(ReleaseMutex(mixed[0]), "wait-all-owns-mutex");
    CloseHandle(mixed[0]);
    CloseHandle(mixed[1]);
}

static void test_timeouts(void)
{
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    unsigned long long start = now_ticks(), elapsed;
    DWORD result = WaitForSingleObject(event, 50);
    elapsed = ticks_to_ns(now_ticks() - start) / 1000000;
    check(result == WAIT_TIMEOUT, "timeout-result");
    check(elapsed >= 45 && elapsed < 500, "timeout-duration");
    put("PS5_PROTON_SYNC_PROBE bench wait50_ms=");
    put_unsigned(elapsed);
    put("\r\n");
    flush();
    CloseHandle(event);
}

static volatile LONG apc_ran;
static void CALLBACK apc(ULONG_PTR value)
{
    InterlockedExchange(&apc_ran, (LONG)value);
}
static HANDLE apc_ready, apc_event;
static DWORD WINAPI alertable_waiter(void *unused)
{
    DWORD first, second;
    (void)unused;
    SetEvent(apc_ready);
    first = WaitForSingleObjectEx(apc_event, 5000, TRUE);
    second = SleepEx(5000, TRUE);
    return (first == WAIT_IO_COMPLETION ? 1 : 0) | (second == WAIT_IO_COMPLETION ? 2 : 0);
}

static void test_apcs(void)
{
    HANDLE thread;
    DWORD code = 0;
    apc_ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    apc_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    thread = CreateThread(NULL, 0, alertable_waiter, NULL, 0, NULL);
    WaitForSingleObject(apc_ready, INFINITE);
    Sleep(20);
    QueueUserAPC(apc, thread, 7);
    Sleep(20);
    QueueUserAPC(apc, thread, 9);
    check(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "apc-thread-ends");
    GetExitCodeThread(thread, &code);
    check(code == 3, "apc-wakes-alertable-waits");
    check(apc_ran == 9, "apc-ran");
    CloseHandle(thread);
    CloseHandle(apc_ready);
    CloseHandle(apc_event);
}

static HANDLE dup_event;
static DWORD WINAPI wait_dup(void *unused)
{
    (void)unused;
    return WaitForSingleObject(dup_event, 5000);
}

static void test_handles(void)
{
    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL), thread;
    DWORD code = 99;
    check(DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &dup_event, 0, FALSE,
                          DUPLICATE_SAME_ACCESS), "dup-handle");
    thread = CreateThread(NULL, 0, wait_dup, NULL, 0, NULL);
    Sleep(20);
    CloseHandle(event);    /* the duplicate keeps the object alive */
    {
        /* signal it through a second duplicate of the duplicate */
        HANDLE again;
        check(DuplicateHandle(GetCurrentProcess(), dup_event, GetCurrentProcess(), &again, 0, FALSE,
                              DUPLICATE_SAME_ACCESS), "dup-of-dup");
        SetEvent(again);
        CloseHandle(again);
    }
    WaitForSingleObject(thread, 5000);
    GetExitCodeThread(thread, &code);
    check(code == WAIT_OBJECT_0, "dup-wakes-waiter");
    CloseHandle(thread);
    CloseHandle(dup_event);
}

static HANDLE many_event, many_started;
static volatile LONG many_woken;
static DWORD WINAPI many_waiter(void *unused)
{
    (void)unused;
    ReleaseSemaphore(many_started, 1, NULL);
    if (WaitForSingleObject(many_event, 5000) == WAIT_OBJECT_0) InterlockedIncrement(&many_woken);
    return 0;
}

static void test_multiple_waiters(void)
{
    HANDLE threads[4];
    for (int round = 0; round < 2; round++) {
        const BOOL manual = round == 0;
        many_event = CreateEventW(NULL, manual, FALSE, NULL);
        many_started = CreateSemaphoreW(NULL, 0, 4, NULL);
        many_woken = 0;
        for (int i = 0; i < 4; i++) threads[i] = CreateThread(NULL, 0, many_waiter, NULL, 0, NULL);
        for (int i = 0; i < 4; i++) WaitForSingleObject(many_started, INFINITE);
        Sleep(30);
        if (manual) {
            SetEvent(many_event);
        } else {
            for (int i = 0; i < 4; i++) {
                LONG before = many_woken;
                SetEvent(many_event);
                for (int spin = 0; spin < 500 && many_woken == before; spin++) Sleep(1);
                check(many_woken == before + 1, "auto-wakes-exactly-one");
            }
        }
        WaitForMultipleObjects(4, threads, TRUE, 5000);
        check(many_woken == 4, manual ? "manual-wakes-all" : "auto-wakes-each");
        for (int i = 0; i < 4; i++) CloseHandle(threads[i]);
        CloseHandle(many_event);
        CloseHandle(many_started);
    }
}

static HANDLE visible_ready, visible_done;
static volatile LONG visible_value;
static LONG visible_plain[16];
static DWORD WINAPI visible_consumer(void *unused)
{
    DWORD bad = 0;
    (void)unused;
    for (LONG i = 1; i <= 5000; i++) {
        WaitForSingleObject(visible_ready, INFINITE);
        for (int j = 0; j < 16; j++) if (visible_plain[j] != i) bad++;
        SetEvent(visible_done);
    }
    return bad;
}

static void test_memory_visibility(void)
{
    HANDLE thread;
    DWORD bad = 1;
    visible_ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    visible_done = CreateEventW(NULL, FALSE, FALSE, NULL);
    thread = CreateThread(NULL, 0, visible_consumer, NULL, 0, NULL);
    for (LONG i = 1; i <= 5000; i++) {
        for (int j = 0; j < 16; j++) visible_plain[j] = i;   /* plain stores */
        SetEvent(visible_ready);
        WaitForSingleObject(visible_done, INFINITE);
    }
    WaitForSingleObject(thread, INFINITE);
    GetExitCodeThread(thread, &bad);
    check(bad == 0, "signal-orders-memory");
    CloseHandle(thread);
    CloseHandle(visible_ready);
    CloseHandle(visible_done);
    (void)visible_value;
}

static DWORD WINAPI short_thread(void *unused)
{
    (void)unused;
    Sleep(20);
    return 5;
}

static void test_signal_and_wait_and_threads(void)
{
    HANDLE a = CreateEventW(NULL, FALSE, FALSE, NULL), b = CreateEventW(NULL, FALSE, TRUE, NULL);
    HANDLE pair[2];
    DWORD code = 0;
    check(SignalObjectAndWait(a, b, 0, FALSE) == WAIT_OBJECT_0, "signal-and-wait");
    check(WaitForSingleObject(a, 0) == WAIT_OBJECT_0, "signal-and-wait-signalled");
    /* a thread object (signalled by its exit) beside an event */
    pair[0] = CreateEventW(NULL, TRUE, FALSE, NULL);
    pair[1] = CreateThread(NULL, 0, short_thread, NULL, 0, NULL);
    check(WaitForMultipleObjects(2, pair, FALSE, 5000) == WAIT_OBJECT_0 + 1, "wait-thread-and-event");
    GetExitCodeThread(pair[1], &code);
    check(code == 5, "thread-exit-code");
    CloseHandle(pair[0]);
    CloseHandle(pair[1]);
    CloseHandle(a);
    CloseHandle(b);
}

static HANDLE pool_sem;
static volatile LONG pool_taken;
static DWORD WINAPI pool_worker(void *unused)
{
    (void)unused;
    for (int i = 0; i < 2000; i++) {
        WaitForSingleObject(pool_sem, INFINITE);
        InterlockedIncrement(&pool_taken);
        ReleaseSemaphore(pool_sem, 1, NULL);
    }
    return 0;
}

static void test_semaphore_contention(void)
{
    HANDLE threads[4];
    LONG count = -1;
    pool_sem = CreateSemaphoreW(NULL, 2, 2, NULL);
    for (int i = 0; i < 4; i++) threads[i] = CreateThread(NULL, 0, pool_worker, NULL, 0, NULL);
    WaitForMultipleObjects(4, threads, TRUE, INFINITE);
    for (int i = 0; i < 4; i++) CloseHandle(threads[i]);
    check(pool_taken == 8000, "sem-contention-all-taken");
    WaitForSingleObject(pool_sem, 0);
    WaitForSingleObject(pool_sem, 0);
    check(WaitForSingleObject(pool_sem, 0) == WAIT_TIMEOUT, "sem-contention-count");
    ReleaseSemaphore(pool_sem, 2, &count);
    check(count == 0, "sem-contention-final");
    CloseHandle(pool_sem);
}

/* ---- cost ---------------------------------------------------------------- */

static HANDLE ping, pong;
enum { ROUND_TRIPS = 5000 };
static DWORD WINAPI ponger(void *unused)
{
    (void)unused;
    for (int i = 0; i < ROUND_TRIPS; i++) {
        WaitForSingleObject(ping, INFINITE);
        SetEvent(pong);
    }
    return 0;
}

/* The same round trip with another process (a helper of its own): its events
 * are inherited, so both processes wait and signal one pair of objects. */
static unsigned long long hex_after(const char *text, int skip)
{
    unsigned long long value = 0;
    while (*text && skip) if (*text++ == ' ') --skip;
    for (; (*text >= '0' && *text <= '9') || (*text >= 'a' && *text <= 'f'); text++)
        value = value * 16 + (unsigned)(*text <= '9' ? *text - '0' : *text - 'a' + 10);
    return value;
}
/* Writes value in hex at out; returns the end. */
static char *put_hex(char *out, unsigned long long value)
{
    char digits[17];
    int count = 0;
    do digits[count++] = "0123456789abcdef"[value % 16]; while ((value /= 16) && count < 16);
    while (count) *out++ = digits[--count];
    *out = 0;
    return out;
}
static int process_round_trips(unsigned long long *ticks)
{
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), NULL, TRUE};
    HANDLE ping2 = CreateEventW(&inherit, FALSE, FALSE, NULL), pong2 = CreateEventW(&inherit, FALSE, FALSE, NULL);
    WCHAR path[MAX_PATH];
    char command[MAX_PATH + 64], *at = command;
    WCHAR wide[sizeof(command)];
    STARTUPINFOW startup = {sizeof(startup)};
    PROCESS_INFORMATION child;
    DWORD code = 1;
    int done = 0;
    for (const char *text = "sync-probe.exe ponger "; *text;) *at++ = *text++;
    at = put_hex(at, (unsigned long long)(ULONG_PTR)ping2);
    *at++ = ' ';
    put_hex(at, (unsigned long long)(ULONG_PTR)pong2);
    for (int i = 0; (wide[i] = (WCHAR)command[i]); i++) {}
    if (!GetModuleFileNameW(NULL, path, MAX_PATH) ||
        !CreateProcessW(path, wide, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &child))
        return 0;
    unsigned long long start = now_ticks();
    for (; done < ROUND_TRIPS; done++) {
        SetEvent(ping2);
        if (WaitForSingleObject(pong2, 5000)) break;
    }
    *ticks = now_ticks() - start;
    if (!WaitForSingleObject(child.hProcess, 10000)) GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    CloseHandle(ping2);
    CloseHandle(pong2);
    return done == ROUND_TRIPS && code == 0;
}

static CRITICAL_SECTION section;
static volatile LONG section_counter;
static DWORD WINAPI section_worker(void *unused)
{
    (void)unused;
    for (int i = 0; i < 50000; i++) {
        EnterCriticalSection(&section);
        section_counter++;
        LeaveCriticalSection(&section);
    }
    return 0;
}

/* TerminateThread on a thread blocked in a wait: its wait must not take the next set. On the console Wine marks a
 * killed thread and ends it at the user-mode boundary, so the sync device ends its wait at the SIGQUIT
 * (src/pw_ntsync.c); before, the terminated thread kept its place and took the set meant for the live waiter. */
static HANDLE doomed_event;
static DWORD WINAPI doomed_waiter(void *unused)
{
    (void)unused;
    WaitForSingleObject(doomed_event, INFINITE);
    return 7;
}
static DWORD WINAPI live_waiter(void *unused)
{
    (void)unused;
    return WaitForSingleObject(doomed_event, 5000);
}
static void test_terminate_waiter(void)
{
    DWORD code = 0;
    HANDLE doomed, live;
    doomed_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    doomed = CreateThread(NULL, 0, doomed_waiter, NULL, 0, NULL);
    Sleep(100);                 /* blocked first, so first in the event's queue */
    live = CreateThread(NULL, 0, live_waiter, NULL, 0, NULL);
    Sleep(100);
    check(doomed && live && TerminateThread(doomed, 9), "terminate-waiter");
    check(WaitForSingleObject(doomed, 5000) == WAIT_OBJECT_0 && GetExitCodeThread(doomed, &code) && code == 9,
          "terminated-waiter-ends");
    SetEvent(doomed_event);
    check(WaitForSingleObject(live, 6000) == WAIT_OBJECT_0 && GetExitCodeThread(live, &code) && code == WAIT_OBJECT_0,
          "set-after-terminate-wakes-live");
    CloseHandle(doomed);
    CloseHandle(live);
    CloseHandle(doomed_event);
}

static void benches(void)
{
    unsigned long long start;
    HANDLE thread, event = CreateEventW(NULL, TRUE, TRUE, NULL), events[4];
    HANDLE sem = CreateSemaphoreW(NULL, 0, 1, NULL), mutex = CreateMutexW(NULL, FALSE, NULL);

    ping = CreateEventW(NULL, FALSE, FALSE, NULL);
    pong = CreateEventW(NULL, FALSE, FALSE, NULL);
    thread = CreateThread(NULL, 0, ponger, NULL, 0, NULL);
    start = now_ticks();
    for (int i = 0; i < ROUND_TRIPS; i++) {
        SetEvent(ping);
        WaitForSingleObject(pong, INFINITE);
    }
    bench("event_round_trip_us", now_ticks() - start, ROUND_TRIPS);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    {
        unsigned long long ticks = 0;
        const int worked = process_round_trips(&ticks);
        check(worked, "process-round-trips");
        if (worked) bench("process_round_trip_us", ticks, ROUND_TRIPS);
    }

    start = now_ticks();
    for (int i = 0; i < 20000; i++) {
        SetEvent(ping);
        ResetEvent(ping);
    }
    bench("set_reset_pair_us", now_ticks() - start, 20000);

    start = now_ticks();
    for (int i = 0; i < 20000; i++) WaitForSingleObject(event, 0);
    bench("wait_signalled_us", now_ticks() - start, 20000);

    for (int i = 0; i < 4; i++) events[i] = CreateEventW(NULL, TRUE, i == 3, NULL);
    start = now_ticks();
    for (int i = 0; i < 20000; i++) WaitForMultipleObjects(4, events, FALSE, 0);
    bench("wait_any4_us", now_ticks() - start, 20000);
    for (int i = 0; i < 4; i++) SetEvent(events[i]);
    start = now_ticks();
    for (int i = 0; i < 20000; i++) WaitForMultipleObjects(4, events, TRUE, 0);
    bench("wait_all4_us", now_ticks() - start, 20000);
    for (int i = 0; i < 4; i++) CloseHandle(events[i]);

    start = now_ticks();
    for (int i = 0; i < 20000; i++) {
        ReleaseSemaphore(sem, 1, NULL);
        WaitForSingleObject(sem, 0);
    }
    bench("sem_release_wait_us", now_ticks() - start, 20000);

    start = now_ticks();
    for (int i = 0; i < 20000; i++) {
        WaitForSingleObject(mutex, 0);
        ReleaseMutex(mutex);
    }
    bench("mutex_take_release_us", now_ticks() - start, 20000);

    InitializeCriticalSection(&section);
    {
        HANDLE workers[2];
        start = now_ticks();
        for (int i = 0; i < 2; i++) workers[i] = CreateThread(NULL, 0, section_worker, NULL, 0, NULL);
        WaitForMultipleObjects(2, workers, TRUE, INFINITE);
        bench("critsec_contended_us", now_ticks() - start, 100000);
        for (int i = 0; i < 2; i++) CloseHandle(workers[i]);
    }
    check(section_counter == 100000, "critsec-counter");
    DeleteCriticalSection(&section);

    start = now_ticks();
    for (int i = 0; i < 2000; i++) Sleep(0);
    bench("sleep0_us", now_ticks() - start, 2000);
    start = now_ticks();
    for (int i = 0; i < 50; i++) Sleep(1);
    bench("sleep1_us", now_ticks() - start, 50);

    CloseHandle(ping);
    CloseHandle(pong);
    CloseHandle(event);
    CloseHandle(sem);
    CloseHandle(mutex);
}

void mainCRTStartup(void)
{
    const char *line = GetCommandLineA();
    for (const char *at = line; *at; at++) {
        if (at[0] != ' ' || at[1] != 'p' || at[2] != 'o' || at[3] != 'n' || at[4] != 'g') continue;
        /* the other process of process_round_trips: sync-probe.exe ponger <ping> <pong> */
        HANDLE ping2 = (HANDLE)(ULONG_PTR)hex_after(at + 1, 1), pong2 = (HANDLE)(ULONG_PTR)hex_after(at + 1, 2);
        for (int i = 0; i < ROUND_TRIPS; i++) {
            if (WaitForSingleObject(ping2, 5000)) ExitProcess(2);
            SetEvent(pong2);
        }
        ExitProcess(0);
    }
    QueryPerformanceFrequency(&frequency);
    test_events();
    test_semaphores();
    test_mutexes();
    test_wait_multiple();
    test_timeouts();
    test_apcs();
    test_handles();
    test_multiple_waiters();
    test_memory_visibility();
    test_signal_and_wait_and_threads();
    test_semaphore_contention();
    test_terminate_waiter();
    benches();
    if (failures) {
        failed[failed_used] = 0;
        put("PS5_PROTON_SYNC_PROBE FAIL ");
        put_unsigned(failures);
        put(" checks: ");
        put(failed);
    } else {
        put("PS5_PROTON_SYNC_PROBE PASS: events, semaphores, mutexes, wait-all, timeouts, APCs, handles, waiters, visibility, "
            "terminated waiters");
    }
    put("\r\n");
    flush();
    ExitProcess(failures ? 1 : 0);
}
