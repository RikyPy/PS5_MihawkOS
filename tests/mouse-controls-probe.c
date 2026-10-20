/* SPDX-License-Identifier: GPL-3.0-or-later
 * Check actual Wine messages for remapped left/right/middle press and release,
 * in a PE32 helper. Movement and all clicks must arrive within 45 seconds. */
#include <windows.h>
static unsigned moves, downs[3], ups[3];

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
    case WM_MOUSEMOVE: ++moves; return 0;
    case WM_LBUTTONDOWN: ++downs[0]; return 0;
    case WM_LBUTTONUP: ++ups[0]; return 0;
    case WM_RBUTTONDOWN: ++downs[1]; return 0;
    case WM_RBUTTONUP: ++ups[1]; return 0;
    case WM_MBUTTONDOWN: ++downs[2]; return 0;
    case WM_MBUTTONUP: ++ups[2]; return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

void mainCRTStartup(void)
{
    WNDCLASSA type = {0};
    type.lpfnWndProc = proc;
    type.hInstance = GetModuleHandleA(NULL);
    type.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    type.lpszClassName = "ps5-mouse-controls";
    HWND window = RegisterClassA(&type) ? CreateWindowExA(0, type.lpszClassName, "Mouse controls probe",
        WS_POPUP | WS_VISIBLE, 0, 0, 1920, 1080, NULL, NULL, type.hInstance, NULL) : NULL;
    SetForegroundWindow(window);
    UpdateWindow(window);
    DWORD start = GetTickCount();
    int passed = 0;
    while (window && GetTickCount() - start < 45000) {
        MSG message;
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        if ((passed = moves && downs[0] == 1 && ups[0] == 1 && downs[1] == 1 && ups[1] == 1 &&
                      downs[2] == 1 && ups[2] == 1)) break;
        Sleep(10);
    }
    char line[200];
    wsprintfA(line, "PS5_PROTON_MOUSE_CONTROLS %s: moves=%u left=%u/%u right=%u/%u middle=%u/%u\n",
        passed ? "PASS" : "FAIL", moves, downs[0], ups[0], downs[1], ups[1], downs[2], ups[2]);
    OutputDebugStringA(line);
    if (window) DestroyWindow(window);
    ExitProcess(passed ? 0 : 1);
}
