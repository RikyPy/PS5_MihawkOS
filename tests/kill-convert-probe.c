/* SPDX-License-Identifier: GPL-3.0-or-later
 * A thread killed while its execute fault converts code to JIT memory (patch 1127) must not take the virtual mutex
 * with it (patch 1171). A worker runs fresh code without end: every 64 KiB block it allocates gets a ret on each
 * 4 KiB page and is called page by page, so most of its time goes to conversions. The main thread kills it after a
 * random spin of up to 2 ms, then a checker thread needs the virtual mutex (VirtualAlloc, 5 s bound). Before 1171,
 * winedevice.exe's exit stalled this way with 16 KiB windows three times of three (gate-2026-10-16-browser-01).
 * No host oracle: upstream Wine ends a killed thread inside a syscall too, so on Linux the first kill that lands in
 * the worker's VirtualAlloc keeps the mutex (stuck=2 at once, 2026-10-16); on the console such a thread is marked and
 * ends on its way out (patch 0885), so only a kill inside the fault handler can keep it. */
#include <windows.h>

#define KILLS 300
#define BLOCK 0x10000

static HANDLE running, check_asked, check_done;

static DWORD WINAPI worker(void *arg)
{
    for (;;)
    {
        unsigned char *code = VirtualAlloc(NULL, BLOCK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        DWORD old, i;

        if (!code) continue;
        for (i = 0; i < BLOCK; i += 0x1000) code[i] = 0xc3;
        VirtualProtect(code, BLOCK, PAGE_EXECUTE_READ, &old);
        SetEvent(running);
        for (i = 0; i < BLOCK; i += 0x1000) ((void (*)(void))(code + i))();
        VirtualFree(code, 0, MEM_RELEASE);
    }
}

/* Made before the first kill: creating a thread takes the virtual mutex too, and the verdict must not wait on it. */
static DWORD WINAPI checker(void *arg)
{
    for (;;)
    {
        void *page;

        WaitForSingleObject(check_asked, INFINITE);
        if ((page = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)))
            VirtualFree(page, 0, MEM_RELEASE);
        SetEvent(check_done);
    }
}

void mainCRTStartup(void)
{
    LARGE_INTEGER frequency, start, now;
    DWORD seed = GetTickCount(), kills, worst = 0, stuck = 0, begun, waited;
    char out[200];

    running = CreateEventA(NULL, FALSE, FALSE, NULL);
    check_asked = CreateEventA(NULL, FALSE, FALSE, NULL);
    check_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    CloseHandle(CreateThread(NULL, 0x10000, checker, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL));
    QueryPerformanceFrequency(&frequency);
    for (kills = 0; kills < KILLS && !stuck; kills++)
    {
        HANDLE thread;
        LONGLONG spin;

        ResetEvent(running);  /* the last worker's set, if its kill came after it */
        thread = CreateThread(NULL, 0x10000, worker, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        WaitForSingleObject(running, INFINITE);  /* past thread start-up and its loader lock */
        seed = seed * 1103515245 + 12345;
        spin = frequency.QuadPart * ((seed >> 16) % 2000) / 1000000;
        QueryPerformanceCounter(&start);
        do QueryPerformanceCounter(&now); while (now.QuadPart - start.QuadPart < spin);
        TerminateThread(thread, 0);
        begun = GetTickCount();
        if (WaitForSingleObject(thread, 5000)) stuck = 1;  /* the kill never ended it */
        SetEvent(check_asked);
        if (!stuck && WaitForSingleObject(check_done, 5000)) stuck = 2;  /* it ended holding the virtual mutex */
        if ((waited = GetTickCount() - begun) > worst) worst = waited;
        CloseHandle(thread);
    }
    wsprintfA(out, "PS5_PROTON_KILL_CONVERT %s: kills=%lu stuck=%lu worst_ms=%lu\n", stuck ? "FAIL" : "PASS",
              kills, stuck, worst);
    OutputDebugStringA(out);
    /* ExitProcess would wait on the mutex a stuck kill kept; the stall watch ends what this cannot */
    if (stuck) TerminateProcess(GetCurrentProcess(), 1);
    ExitProcess(0);
}
