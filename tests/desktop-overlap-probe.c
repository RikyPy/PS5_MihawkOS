/* SPDX-License-Identifier: GPL-3.0-or-later
 * Paint a green owned window once, then repaint the blue window behind it
 * every 100 ms. The console runner checks actual captured display pixels:
 * the middle must remain green and the outside blue. No game data required. */
#include <windows.h>

static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        COLORREF color = GetWindowLongPtrA(window, GWLP_USERDATA) ? RGB(0x20,0xe0,0x40) : RGB(0x20,0xa0,0xe0);
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &paint.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcA(window, message, w, l);
}

void mainCRTStartup(void)
{
    WNDCLASSA type = {0};
    type.lpfnWndProc = proc;
    type.hInstance = GetModuleHandleA(NULL);
    type.lpszClassName = "ps5-desktop-overlap";
    int okay = RegisterClassA(&type) != 0;
    HWND back = okay ? CreateWindowExA(0, type.lpszClassName, "back", WS_POPUP | WS_VISIBLE,
        300,200,800,500,NULL,NULL,type.hInstance,NULL) : NULL;
    HWND front = back ? CreateWindowExA(0,type.lpszClassName,"front",WS_POPUP,
        500,325,400,250,back,NULL,type.hInstance,NULL) : NULL;
    okay = back && front;
    if (okay) {
        SetWindowLongPtrA(front, GWLP_USERDATA, 1);
        ShowWindow(front, SW_SHOW);
        SetForegroundWindow(front);
        UpdateWindow(front);
        DWORD start = GetTickCount(), last = start;
        while (GetTickCount() - start < 15000) {
            MSG message;
            while (PeekMessageA(&message,NULL,0,0,PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageA(&message);
            }
            if (front && GetTickCount() - start >= 10000) {
                DestroyWindow(front);
                front = NULL;
                OutputDebugStringA("PS5_PROTON_DESKTOP_OVERLAP_PROBE: front closed\n");
                /* No background repaint: closing the front window must expose
                 * the retained blue pixels on the following display capture. */
            }
            if (front && GetTickCount() - start < 7000 && GetTickCount() - last >= 100) {
                InvalidateRect(back,NULL,FALSE);
                UpdateWindow(back);
                last = GetTickCount();
            }
            Sleep(10);
        }
    }
    if (front) DestroyWindow(front);
    if (back) DestroyWindow(back);
    OutputDebugStringA(okay ? "PS5_PROTON_DESKTOP_OVERLAP_PROBE PASS: windows painted; verify display captures\n"
                           : "PS5_PROTON_DESKTOP_OVERLAP_PROBE FAIL: window creation\n");
    ExitProcess(okay ? 0 : 1);
}
