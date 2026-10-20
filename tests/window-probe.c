/* SPDX-License-Identifier: GPL-3.0-or-later
 * A window that polls its messages with PeekMessage and a short Sleep, the way
 * SDL pumps events: it creates a visible window, says so, and leaves on WM_CLOSE
 * (what the title posts for a stop request). If the request never arrives it
 * ends itself after 90 s, so the run always finishes through ExitProcess. */
#include <windows.h>

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE) {
        say("PS5_PROTON_WINDOW_PROBE: WM_CLOSE received\r\n");
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

void mainCRTStartup(void)
{
    WNDCLASSA class = {0};
    MSG message;
    HWND window;
    DWORD start = GetTickCount();

    class.lpfnWndProc = proc;
    class.hInstance = GetModuleHandleA(NULL);
    class.lpszClassName = "ps5_window_probe";
    RegisterClassA(&class);
    window = CreateWindowA("ps5_window_probe", "window probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           0, 0, 640, 360, NULL, NULL, class.hInstance, NULL);
    say(window ? "PS5_PROTON_WINDOW_PROBE PASS: window created, polling with PeekMessage\r\n"
               : "PS5_PROTON_WINDOW_PROBE FAIL: no window\r\n");
    for (;;) {
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                say("PS5_PROTON_WINDOW_PROBE: leaving after the window closed\r\n");
                ExitProcess(0);
            }
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        if (GetTickCount() - start > 90000) {
            say("PS5_PROTON_WINDOW_PROBE: no close request in 90 s; leaving by itself\r\n");
            ExitProcess(0);
        }
        Sleep(16);
    }
}
