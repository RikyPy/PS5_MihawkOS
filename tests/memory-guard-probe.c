/* SPDX-License-Identifier: GPL-3.0-or-later
 * The title's guard over the console's system memory (src/pw_memory_guard.h), from the game's side: a window that
 * holds --hold=N MiB (300 unless told), committed 16 MiB at a time and every 4 KiB page written, as a game's heap
 * grows. Run with the guard's limit below what that makes (PROTON_SYSTEM_MEMORY_LIMIT), it passes when the title asks
 * it to close (WM_CLOSE, as a user's close would) while it holds the memory, and fails when nothing has asked after
 * 120 s. Run as the first program in the launcher layout, it is a helper, so its memory is the console's system
 * memory the guard watches. */
#include <windows.h>

static volatile LONG closes;
static unsigned long held_mib;

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        HBRUSH brush = CreateSolidBrush(RGB(0xe0, 0x60, 0x20));
        FillRect(dc, &paint.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE: {
        char line[128];
        InterlockedIncrement(&closes);
        wsprintfA(line, "memory-guard: close request while holding %lu MiB\n", held_mib);
        say(line);
        DestroyWindow(window);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

static unsigned long hold_setting(void)
{
    const char *line = GetCommandLineA();
    for (; *line; ++line) {
        const char *key = "--hold=";
        int i = 0;
        while (key[i] && line[i] == key[i]) ++i;
        if (!key[i]) {
            unsigned long value = 0;
            for (line += i; *line >= '0' && *line <= '9'; ++line) value = value * 10 + (unsigned long)(*line - '0');
            return value;
        }
    }
    return 300;
}

/* The messages that came in, and whether one was the end. */
static int pump(void)
{
    MSG message;
    while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) return 1;
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    return 0;
}

void mainCRTStartup(void)
{
    const unsigned long want = hold_setting();
    WNDCLASSA type = {0};
    char line[160];
    type.lpfnWndProc = proc;
    type.hInstance = GetModuleHandleA(NULL);
    type.lpszClassName = "ps5-memory-guard-probe";
    HWND window = RegisterClassA(&type) ?
        CreateWindowExA(0, "ps5-memory-guard-probe", "memory guard probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 160, 120,
                        640, 360, NULL, NULL, type.hInstance, NULL) : NULL;
    if (!window) {
        say("PS5_PROTON_MEMORY_GUARD_PROBE FAIL: no window\n");
        ExitProcess(1);
    }
    SetForegroundWindow(window);
    UpdateWindow(window);

    int done = 0;
    while (!done && held_mib < want) {
        volatile unsigned char *chunk = VirtualAlloc(NULL, 16u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!chunk) {
            wsprintfA(line, "PS5_PROTON_MEMORY_GUARD_PROBE FAIL: refused at %lu MiB (error %lu)\n", held_mib,
                      GetLastError());
            say(line);
            ExitProcess(1);
        }
        for (unsigned long at = 0; at < (16u << 20); at += 4096) chunk[at] = (unsigned char)(at >> 12);
        held_mib += 16;
        done = pump();
        Sleep(50);
    }
    wsprintfA(line, "memory-guard: holding %lu MiB\n", held_mib);
    say(line);
    const DWORD start = GetTickCount();
    while (!done && GetTickCount() - start < 120000) {
        done = pump();
        Sleep(20);
    }
    if (closes) {
        wsprintfA(line, "PS5_PROTON_MEMORY_GUARD_PROBE PASS: asked to close while holding %lu MiB\n", held_mib);
        say(line);
        ExitProcess(0);
    }
    wsprintfA(line, "PS5_PROTON_MEMORY_GUARD_PROBE FAIL: not asked to close in 120 s while holding %lu MiB\n",
              held_mib);
    say(line);
    ExitProcess(1);
}
