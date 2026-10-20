/* SPDX-License-Identifier: GPL-3.0-or-later
 * A window drawn with GDI, for the title's desktop (docs/WINDOWS_PROCESSES.md, "The desktop"). It paints its client
 * area one colour (0x20a0e0), takes the foreground and counts the pointer's moves and left clicks that reach it. It
 * passes once it has seen a move, a press and a release, which the title's touchpad makes (a pad script); a close
 * request (WM_CLOSE) ends it as a user's would, and it ends by itself after 120 s. Run as the first program in the
 * launcher layout, it is a helper: its frames reach the screen only through the title's compositor, and its input
 * only through the desktop the title injects into. */
#include <windows.h>

static volatile LONG moves, downs, ups, closes;

static void say(const char *text)
{
    OutputDebugStringA(text);
}

static void count_line(void)
{
    char line[160];
    wsprintfA(line, "desktop: moves %ld downs %ld ups %ld closes %ld\n", moves, downs, ups, closes);
    say(line);
}

static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        HBRUSH brush = CreateSolidBrush(RGB(0x20, 0xa0, 0xe0));
        FillRect(dc, &paint.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (!moves) say("desktop: first pointer move\n");
        InterlockedIncrement(&moves);
        return 0;
    case WM_LBUTTONDOWN: InterlockedIncrement(&downs); say("desktop: left press\n"); return 0;
    case WM_LBUTTONUP: InterlockedIncrement(&ups); say("desktop: left release\n"); return 0;
    case WM_CLOSE: InterlockedIncrement(&closes); say("desktop: close request\n"); DestroyWindow(window); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

void mainCRTStartup(void)
{
    WNDCLASSA type = {0};
    type.lpfnWndProc = proc;
    type.hInstance = GetModuleHandleA(NULL);
    type.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    type.lpszClassName = "ps5-desktop-probe";
    HWND window = RegisterClassA(&type) ?
        CreateWindowExA(0, "ps5-desktop-probe", "desktop probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 150, 800, 500,
                        NULL, NULL, type.hInstance, NULL) : NULL;
    if (!window) {
        say("PS5_PROTON_DESKTOP_PROBE FAIL: no window\n");
        ExitProcess(1);
    }
    SetForegroundWindow(window);
    UpdateWindow(window);
    say("desktop: window shown\n");
    const DWORD start = GetTickCount();
    int done = 0;
    while (!done && GetTickCount() - start < 120000) {
        MSG message;
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) done = 1;
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        if (moves && downs && ups) done = 1;
        Sleep(10);
    }
    count_line();
    if (IsWindow(window)) DestroyWindow(window);
    const int passed = moves && downs && ups;
    say(passed ? "PS5_PROTON_DESKTOP_PROBE PASS: the window was shown and the pointer reached it\n"
               : "PS5_PROTON_DESKTOP_PROBE FAIL: the pointer did not reach the window\n");
    ExitProcess(passed ? 0 : 1);
}
