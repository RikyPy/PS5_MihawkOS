/* SPDX-License-Identifier: GPL-3.0-or-later
 * Which window holds the foreground and the keyboard focus once a game has been started from the Mihawk desktop, and does the game's
 * window ever get them (the user's report of 2026-10-13: a .NET (MonoGame) game started from its desktop shortcut ignores the pad, and SDL drops
 * joystick events while its window has no keyboard focus). Run inside the Mihawk session:
 *
 *   game-focus-probe --exe=C:\path\game.exe [--click-desktop] [--seconds=N]
 *
 * --click-desktop first clicks an empty spot of the desktop, as a person's click on the shortcut would, so that Explorer's desktop holds the
 * foreground when the game starts. Once a second it prints the foreground window, the keyboard focus and the active window of its thread,
 * and each visible top-level window of the game's process; a line is printed only when something changed. */
#include <windows.h>
#include <shellapi.h>

static void line(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void pump(DWORD milliseconds)
{
    DWORD start = GetTickCount();
    MSG message;
    do {
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        Sleep(10);
    } while (GetTickCount() - start < milliseconds);
}

static void mouse(DWORD flags, int x, int y)
{
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dx = x * 65535 / 1919;
    input.mi.dy = y * 65535 / 1079;
    input.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &input, sizeof(input));
}

static void describe(char *out, size_t size, HWND window)
{
    char klass[48] = "-", title[48] = "";
    DWORD process = 0;
    if (!window) { lstrcpynA(out, "none", (int)size); return; }
    GetClassNameA(window, klass, sizeof(klass));
    GetWindowTextA(window, title, sizeof(title));
    GetWindowThreadProcessId(window, &process);
    wsprintfA(out, "%p[%s '%s' pid %lu]", (void *)window, klass, title, process);
}

static BOOL starts_with(const char *text, const char *prefix)
{
    while (*prefix) if (*text++ != *prefix++) return FALSE;
    return TRUE;
}

struct game_window { DWORD process; HWND window; };

static BOOL CALLBACK find_game_window(HWND window, LPARAM parameter)
{
    struct game_window *game = (struct game_window *)parameter;
    DWORD process = 0;
    RECT rect;
    GetWindowThreadProcessId(window, &process);
    if (process != game->process || !IsWindowVisible(window)) return TRUE;
    GetWindowRect(window, &rect);
    if (rect.right - rect.left < 100) return TRUE;
    game->window = window;
    return FALSE;
}

void mainCRTStartup(void)
{
    char command[MAX_PATH * 2], exe[MAX_PATH] = "", last[600] = "", text[800];
    const char *c;
    int seconds = 120, click = 0;
    DWORD begin;
    PROCESS_INFORMATION process = {0};
    STARTUPINFOA startup = {0};
    HWND progman;

    startup.cb = sizeof(startup);

    lstrcpynA(command, GetCommandLineA(), sizeof(command));
    for (c = command; *c; ++c) {
        if (c != command && c[-1] != ' ') continue;
        if (starts_with(c, "--click-desktop")) click = 1;
        if (starts_with(c, "--exe=")) {
            const char *end = c + 6;   /* the path may hold spaces: it ends at the next " --" or the end */
            while (*end && !(end[0] == ' ' && end[1] == '-' && end[2] == '-')) ++end;
            lstrcpynA(exe, c + 6, (int)(end - c - 6) + 1);
        }
        if (starts_with(c, "--seconds=")) {
            const char *d = c + 10;
            seconds = 0;
            while (*d >= '0' && *d <= '9') seconds = seconds * 10 + (*d++ - '0');
        }
    }
    if (!exe[0]) { line("game-focus FAIL: --exe= missing\n"); ExitProcess(2); }

    progman = FindWindowW(L"Progman", NULL);
    for (begin = GetTickCount(); !progman && GetTickCount() - begin < 30000; ) { pump(100); progman = FindWindowW(L"Progman", NULL); }
    pump(1500);
    if (click) {
        /* an empty desktop spot, away from the icon column and the taskbar */
        mouse(MOUSEEVENTF_MOVE, 900, 500); pump(80);
        mouse(MOUSEEVENTF_LEFTDOWN, 900, 500); pump(60); mouse(MOUSEEVENTF_LEFTUP, 900, 500);
        pump(500);
    }
    {
        char foreground[200];
        describe(foreground, sizeof(foreground), GetForegroundWindow());
        wsprintfA(text, "game-focus note: before the launch, click=%d foreground=%s\n", click, foreground);
        line(text);
    }
    if (!CreateProcessA(exe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        wsprintfA(text, "game-focus FAIL: CreateProcess %s error=%lu\n", exe, GetLastError());
        line(text);
        ExitProcess(3);
    }
    CloseHandle(process.hThread);
    begin = GetTickCount();
    while (GetTickCount() - begin < (DWORD)seconds * 1000) {
        struct game_window game = {process.dwProcessId, NULL};
        HWND foreground = GetForegroundWindow();
        GUITHREADINFO info = {0};
        char f[200], a[200], k[200], g[200], now[700];
        DWORD alive = WaitForSingleObject(process.hProcess, 0);
        info.cbSize = sizeof(info);
        EnumWindows(find_game_window, (LPARAM)&game);
        GetGUIThreadInfo(0, &info);
        describe(f, sizeof(f), foreground);
        describe(a, sizeof(a), info.hwndActive);
        describe(k, sizeof(k), info.hwndFocus);
        describe(g, sizeof(g), game.window);
        wsprintfA(now, "foreground=%s active=%s focus=%s game_window=%s game_is_foreground=%d game_running=%d", f, a, k, g,
                  game.window && game.window == foreground, alive == WAIT_TIMEOUT);
        if (lstrcmpA(now, last)) {
            wsprintfA(text, "game-focus at %lu s: %s\n", (GetTickCount() - begin) / 1000, now);
            line(text);
            lstrcpynA(last, now, sizeof(last));
        }
        if (alive != WAIT_TIMEOUT) break;
        pump(1000);
    }
    line("PS5_PROTON_GAME_FOCUS PASS\n");
    TerminateProcess(process.hProcess, 0);
    ExitProcess(0);
}
