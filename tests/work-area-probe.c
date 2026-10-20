/* SPDX-License-Identifier: GPL-3.0-or-later
 * Work-area ownership is shared by the desktop, not cached by each client.
 * Exercise SPI, monitor information and real maximization across PE32/PE64;
 * restore the original area even after a failed child. No application moves
 * itself around the taskbar to conceal a broken shared contract. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, failures, changes;
static LRESULT CALLBACK observer_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_SETTINGCHANGE && w == SPI_SETWORKAREA) ++changes;
    return DefWindowProcW(window, message, w, l);
}
static void check(BOOL okay, const char *what)
{
    char line[256]; DWORD written;
    ++checks; failures += !okay;
    wsprintfA(line, "work-area %s: %s error=%lu\n", okay ? "PASS" : "FAIL", what, GetLastError());
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, lstrlenA(line), &written, NULL);
    OutputDebugStringA(line);
}
static void area(const RECT *expected)
{
    RECT actual = {0}; MONITORINFO info = {sizeof(info)};
    check(SystemParametersInfoW(SPI_GETWORKAREA, 0, &actual, 0) && EqualRect(&actual, expected), "SPI shared area");
    check(GetMonitorInfoW(MonitorFromRect(expected, MONITOR_DEFAULTTOPRIMARY), &info) &&
          EqualRect(&info.rcWork, expected), "monitor shared area");
}
static void maximize(const RECT *expected)
{
    HWND window = CreateWindowExW(0, L"STATIC", L"Mihawk work-area probe", WS_OVERLAPPEDWINDOW,
                                 expected->left + 80, expected->top + 80, 320, 240,
                                 NULL, NULL, GetModuleHandleW(NULL), NULL);
    check(window != NULL, "real window created");
    if (window)
    {
        RECT client = {0}; POINT points[2];
        ShowWindow(window, SW_SHOWMAXIMIZED);
        check(IsZoomed(window), "normal maximize command");
        BOOL got = GetClientRect(window, &client);
        points[0].x = client.left; points[0].y = client.top;
        points[1].x = client.right; points[1].y = client.bottom;
        got = ClientToScreen(window, points) && ClientToScreen(window, points + 1) && got;
        check(got && points[0].x >= expected->left && points[0].y >= expected->top &&
              points[1].x <= expected->right && points[1].y <= expected->bottom &&
              points[1].x - points[0].x > 200 && points[1].y - points[0].y > 100,
              "maximized client stays inside shared work area");
        ShowWindow(window, SW_RESTORE);
        check(!IsZoomed(window), "normal restore command");
        DestroyWindow(window);
    }
}
static void verdict(const char *mode)
{
    char line[180]; DWORD written;
    wsprintfA(line, "PS5_PROTON_WORK_AREA %s mode=%s bits=%u checks=%u failures=%u\n",
              failures ? "FAIL" : "PASS", mode, (unsigned)sizeof(void *) * 8, checks, failures);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, lstrlenA(line), &written, NULL);
    OutputDebugStringA(line);
}
void mainCRTStartup(void)
{
    const WCHAR *mode = wcsstr(GetCommandLineW(), L"--child ");
    if (mode)
    {
        RECT selected; WCHAR *end; const WCHAR *p = mode + 8;
        selected.left = wcstol(p, &end, 10); selected.top = wcstol(end, &end, 10);
        selected.right = wcstol(end, &end, 10); selected.bottom = wcstol(end, &end, 10);
        area(&selected); maximize(&selected);
        selected.bottom -= 16;
        check(SystemParametersInfoW(SPI_SETWORKAREA, 0, &selected, SPIF_SENDCHANGE), "child updates shared area");
        area(&selected);
        verdict("child"); ExitProcess(failures != 0);
    }
    RECT original = {0}, selected; MONITORINFO info = {sizeof(info)};
    BOOL saved = SystemParametersInfoW(SPI_GETWORKAREA, 0, &original, 0);
    check(saved, "original area saved");
    check(GetMonitorInfoW(MonitorFromPoint((POINT){0,0}, MONITOR_DEFAULTTOPRIMARY), &info), "actual monitor available");
    if (!saved || IsRectEmpty(&info.rcMonitor)) { verdict("root"); ExitProcess(1); }
    WNDCLASSW type = {0}; type.lpfnWndProc = observer_proc;
    type.hInstance = GetModuleHandleW(NULL); type.lpszClassName = L"MihawkWorkAreaObserver";
    check(RegisterClassW(&type) != 0, "notification class registered");
    HWND observer = CreateWindowExW(0, type.lpszClassName, L"Work-area observer", WS_POPUP,
                                    0, 0, 1, 1, NULL, NULL, type.hInstance, NULL);
    check(observer != NULL, "real notification observer");
    selected = info.rcMonitor; selected.bottom -= 48;
    check(SystemParametersInfoW(SPI_SETWORKAREA, 0, &selected, SPIF_SENDCHANGE), "parent reserves taskbar height");
    check(changes != 0, "parent change notification without registry update");
    area(&selected); maximize(&selected);
    WCHAR executable[MAX_PATH], command[1024];
    GetModuleFileNameW(NULL, executable, MAX_PATH);
    WCHAR *base = wcsrchr(executable, L'\\');
    if (base) lstrcpyW(base + 1, sizeof(void *) == 8 ? L"work-area32-probe.exe" : L"work-area64-probe.exe");
    wsprintfW(command, L"\"%s\" --child %ld %ld %ld %ld", executable,
              selected.left, selected.top, selected.right, selected.bottom);
    STARTUPINFOW startup = {sizeof(startup)}; PROCESS_INFORMATION child = {0};
    BOOL started = CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &child);
    check(started, "opposite-architecture child starts");
    if (started)
    {
        DWORD start = GetTickCount(), code = ~0u, wait;
        do
        {
            MSG message;
            while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
            { TranslateMessage(&message); DispatchMessageW(&message); }
            wait = WaitForSingleObject(child.hProcess, 10);
        } while (wait == WAIT_TIMEOUT && GetTickCount() - start < 15000);
        check(wait == WAIT_OBJECT_0, "child finishes within deadline");
        check(GetExitCodeProcess(child.hProcess, &code) && code == 0, "child shared-area verdict");
        if (wait != WAIT_OBJECT_0) { TerminateProcess(child.hProcess, 1); WaitForSingleObject(child.hProcess, 2000); }
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
        check(changes >= 2, "child change reaches already-running parent");
        selected.bottom -= 16; area(&selected); maximize(&selected);
    }
    check(SystemParametersInfoW(SPI_SETWORKAREA, 0, &original, SPIF_SENDCHANGE), "original area restored");
    area(&original);
    check(changes >= 3, "restore change notification");
    if (observer) DestroyWindow(observer);
    verdict("root"); ExitProcess(failures != 0);
}
