/* SPDX-License-Identifier: GPL-3.0-or-later
 * Whether creating and ending threads leaks anything on the console. Direct3D's tests ran out of file
 * descriptors at about a thousand thread ids (each thread holds a dozen: the sockets to the in-process
 * server, a kqueue for its alert), with the process limit at 13,952, so either threads that end do not
 * give their descriptors back, or the programs under test keep over a thousand threads alive.
 *
 * Phase 1 creates a thread, waits for it and closes it, 3,000 times: a leak per thread fails a
 * creation at about a thousand. Phase 2 keeps 200 threads alive at once and then ends them, and phase 3
 * runs a thousand more one at a time, to show that what phase 2 held was given back. The handle count of
 * the process is read after each phase (the server's handles, not descriptors).
 *
 * Phase 4 is what the first three missed: threads that WAIT (a condition variable with a timeout, which is
 * NtWaitForAlertByThreadId underneath). Every thread id that waited kept a kqueue for the life of the process,
 * a console process can hold only about 256 files and kqueues together, and the waits began to fail with an
 * invalid thread id at about the 260th thread id. 3,000 sequential threads, each waiting once: no wait may fail
 * for any reason but its timeout. Ends by itself. */
#include <windows.h>

WINBASEAPI BOOL WINAPI GetProcessHandleCount(HANDLE process, PDWORD count);

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void number(char *out, ULONG_PTR value)
{
    char reversed[24];
    int count = 0, i;

    do reversed[count++] = '0' + value % 10; while ((value /= 10));
    for (i = 0; i < count; i++) out[i] = reversed[count - 1 - i];
    out[count] = 0;
}

static void line(const char *name, ULONG_PTR value)
{
    char text[128], digits[24];

    lstrcpyA(text, "PS5_PROTON_THREAD_LEAK_PROBE ");
    lstrcatA(text, name);
    lstrcatA(text, " ");
    number(digits, value);
    lstrcatA(text, digits);
    lstrcatA(text, "\n");
    say(text);
}

static HANDLE gate;
static volatile LONG highest_tid;

static DWORD WINAPI quick(void *arg)
{
    LONG tid = (LONG)GetCurrentThreadId(), seen;

    while ((seen = highest_tid) < tid && InterlockedCompareExchange(&highest_tid, tid, seen) != seen) {}
    return 0;
}

static DWORD WINAPI held(void *arg)
{
    quick(arg);
    WaitForSingleObject(gate, 60000);
    return 0;
}

static CONDITION_VARIABLE sleeper_cv = CONDITION_VARIABLE_INIT;
static SRWLOCK sleeper_lock = SRWLOCK_INIT;
static volatile LONG wait_errors;
static volatile LONG first_wait_error;

static DWORD WINAPI sleeper(void *arg)
{
    quick(arg);
    AcquireSRWLockExclusive(&sleeper_lock);
    if (!SleepConditionVariableSRW(&sleeper_cv, &sleeper_lock, 1, 0) && GetLastError() != ERROR_TIMEOUT) {
        if (!InterlockedIncrement(&wait_errors)) {}
        first_wait_error = (LONG)GetLastError();
    }
    ReleaseSRWLockExclusive(&sleeper_lock);
    return 0;
}

static DWORD handle_count(void)
{
    DWORD count = 0;
    GetProcessHandleCount(GetCurrentProcess(), &count);
    return count;
}

/* creates and ends threads one at a time; returns how many creations failed */
static unsigned int sequential(unsigned int total, DWORD *first_error)
{
    unsigned int i, failed = 0;

    for (i = 0; i < total; i++) {
        HANDLE thread = CreateThread(NULL, 0x10000, quick, NULL, 0, NULL);

        if (!thread) {
            if (!failed) *first_error = GetLastError();
            failed++;
            continue;
        }
        WaitForSingleObject(thread, 10000);
        CloseHandle(thread);
    }
    return failed;
}

void mainCRTStartup(void)
{
    enum { HELD = 200 };
    HANDLE threads[HELD];
    unsigned int i, created = 0, failed1, failed2 = 0, failed3;
    DWORD error1 = 0, error2 = 0, error3 = 0, before = handle_count();
    BOOL pass;

    line("handles at start", before);
    failed1 = sequential(3000, &error1);
    line("phase 1: 3000 sequential, creations failed", failed1);
    line("  first error", error1);
    line("  highest thread id", highest_tid);
    line("  handles", handle_count());

    gate = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (i = 0; i < HELD; i++) {
        threads[i] = CreateThread(NULL, 0x10000, held, NULL, 0, NULL);
        if (threads[i]) created++;
        else {
            if (!failed2) error2 = GetLastError();
            failed2++;
        }
    }
    line("phase 2: 200 alive at once, created", created);
    line("  creations failed", failed2);
    line("  first error", error2);
    line("  highest thread id", highest_tid);
    SetEvent(gate);
    for (i = 0; i < HELD; i++) {
        if (!threads[i]) continue;
        WaitForSingleObject(threads[i], 20000);
        CloseHandle(threads[i]);
    }
    line("  handles after they ended", handle_count());

    failed3 = sequential(1000, &error3);
    line("phase 3: 1000 more sequential, creations failed", failed3);
    line("  first error", error3);
    line("  highest thread id", highest_tid);
    line("  handles", handle_count());

    for (i = 0; i < 3000; i++) {
        HANDLE thread = CreateThread(NULL, 0x10000, sleeper, NULL, 0, NULL);

        if (!thread) {
            failed3++;
            continue;
        }
        WaitForSingleObject(thread, 10000);
        CloseHandle(thread);
    }
    line("phase 4: 3000 sequential threads that wait once, waits that failed", wait_errors);
    line("  first error", first_wait_error);
    line("  creations failed (total of phase 3 and 4)", failed3);
    line("  highest thread id", highest_tid);

    pass = !failed1 && !failed2 && !failed3 && !wait_errors;
    say(pass ? "PS5_PROTON_THREAD_LEAK_PROBE PASS: threads that end give everything back\n"
             : "PS5_PROTON_THREAD_LEAK_PROBE FAIL: a thread creation failed\n");
    ExitProcess(0);
}
