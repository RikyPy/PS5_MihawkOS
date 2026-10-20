/* SPDX-License-Identifier: GPL-3.0-or-later
 * A process that never ends and never pumps messages: its verdict, then
 * sleeping and spinning on two threads. The title's stop request can only
 * reach a program through a message loop, so this one is never reached that
 * way: the title must end it by SIGKILL to its helper (src/pw_hang_breaker.h)
 * and return to the launcher all the same. --hold=N (default 90): after N s it
 * ends itself, its own backstop when that ending does not come. */
#include <windows.h>

static DWORD WINAPI spin(void *unused)
{
    volatile unsigned long long counter = 0;
    (void)unused;
    for (;;) counter++;
}

/* The number after name on the command line (name ends in '='), or fallback. */
static DWORD option(const char *name, DWORD fallback)
{
    const int length = lstrlenA(name);
    for (const char *at = GetCommandLineA(); *at; ++at) {
        int i = 0;
        DWORD value = 0;
        while (i < length && at[i] == name[i]) ++i;
        if (i < length || at[length] < '0' || at[length] > '9') continue;
        for (at += length; *at >= '0' && *at <= '9'; ++at) value = value * 10 + (DWORD)(*at - '0');
        return value;
    }
    return fallback;
}

void mainCRTStartup(void)
{
    static const char message[] = "PS5_PROTON_HANG_PROBE PASS: running until it is ended from outside\r\n";
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, sizeof(message) - 1, &written, NULL);
    OutputDebugStringA(message);
    CreateThread(NULL, 0, spin, NULL, 0, NULL);
    Sleep(option("--hold=", 90) * 1000);
    ExitProcess(0);
}
