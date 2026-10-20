/* SPDX-License-Identifier: GPL-3.0-or-later
 * Run in the desktop session (run-console.py --profile mihawk-os): opens each shell surface the approved prototype
 * draws and has the title capture the display with it open, for tools/run-shell-surfaces.py to compare with the
 * prototype. Captures: 1 the desktop, 2 Start (light), 3 Start (dark), 4 Search from the taskbar's search pill,
 * 5 Quick Settings from the status button, 6 the notification centre from the clock, 7 the hidden icons from the
 * chevron. Start's pins are the defaults for the capture (the user's are saved and restored); the pointer rests in the
 * top right corner. A surface that does not open is a failed check, and its capture is still taken. */
#include <windows.h>
#include <uxtheme.h>
#include <string.h>

static unsigned checks, failures;
static char capture_base[MAX_PATH];
static const WCHAR pin_key[] = L"Software\\Mihawk\\Shell";

static void line(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}
static void check(BOOL okay, const char *what)
{
    char text[256]; ++checks; failures += !okay;
    wsprintfA(text, "shell-surfaces %s: %s error=%lu\n", okay ? "PASS" : "FAIL", what, GetLastError()); line(text);
}
static void pump(DWORD milliseconds)
{
    DWORD start = GetTickCount();
    do { MSG message; while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } Sleep(10); }
    while (GetTickCount() - start < milliseconds);
}
/* The title saves a complete numbered PPM after its displayed flip (as tests/desktop-session-probe.c asks for it). */
static BOOL capture_display(unsigned number)
{
    char stem[MAX_PATH], path[MAX_PATH], request[MAX_PATH], *dot = NULL;
    if (!capture_base[0] || lstrlenA(capture_base) >= MAX_PATH - 24) return FALSE;
    lstrcpyA(stem, capture_base);
    for (char *c = stem; *c; ++c) if (*c == '.') dot = c;
    if (dot) *dot = 0;
    wsprintfA(path, "%s-%02u.ppm", stem, number); wsprintfA(request, "%s.broker-request", capture_base);
    DeleteFileA(path); pump(400);
    HANDLE file = CreateFileA(request, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE || !CloseHandle(file)) return FALSE;
    DWORD started = GetTickCount(); unsigned complete_ticks = 0;
    while (GetTickCount() - started < 10000)
    {
        WIN32_FILE_ATTRIBUTE_DATA image;
        BOOL complete = GetFileAttributesExA(path, GetFileExInfoStandard, &image) && !image.nFileSizeHigh &&
                        image.nFileSizeLow == 1920 * 1080 * 3 + 17;
        if (complete && ++complete_ticks == 3) return TRUE;
        if (!complete) complete_ticks = 0;
        pump(20);
    }
    DeleteFileA(request); return FALSE;
}
typedef HRESULT (WINAPI *open_theme_fn)(const WCHAR *, const WCHAR *, const WCHAR *, HANDLE *, DWORD);
typedef HRESULT (WINAPI *apply_theme_fn)(HANDLE, char *, HWND);
typedef HRESULT (WINAPI *close_theme_fn)(HANDLE);
static HRESULT apply_theme(const WCHAR *colour)
{
    WCHAR path[MAX_PATH]; HMODULE module = LoadLibraryW(L"uxtheme.dll"); HANDLE theme = NULL; HRESULT result = E_NOTIMPL;
    if (!module) return result;
    GetWindowsDirectoryW(path, MAX_PATH); lstrcatW(path, L"\\resources\\themes\\light\\light.msstyles");
    open_theme_fn open = (open_theme_fn)GetProcAddress(module, MAKEINTRESOURCEA(2));
    apply_theme_fn apply = (apply_theme_fn)GetProcAddress(module, MAKEINTRESOURCEA(4));
    close_theme_fn close = (close_theme_fn)GetProcAddress(module, MAKEINTRESOURCEA(3));
    if (open && apply && close && SUCCEEDED(result = open(path, colour, L"NormalSize", &theme, 0))) { result = apply(theme, NULL, NULL); close(theme); }
    FreeLibrary(module); pump(300); return result;
}
static HWND wait_window(const WCHAR *cls, BOOL visible, DWORD ms)
{
    DWORD start = GetTickCount(); HWND window;
    do { window = FindWindowW(cls, NULL); if (window && !!IsWindowVisible(window) == visible) return window; pump(20); }
    while (GetTickCount() - start < ms);
    return visible ? NULL : window;
}
static BOOL CALLBACK find_start(HWND child, LPARAM param)
{
    WCHAR title[32]; GetWindowTextW(child, title, 32);
    if (!lstrcmpW(title, L"Start")) *(HWND *)param = child;
    return TRUE;
}
/* A surface opened by a taskbar button, captured, then dismissed; its rectangle goes to the log for the comparison. */
/* optional: the hidden icons open only when the notification area has icons, as on Windows */
static void surface(HWND tray, HWND button, const WCHAR *cls, unsigned number, const char *name, BOOL optional)
{
    char text[256]; RECT r = {0}; HWND window;
    check(button != NULL, name);
    /* a click as the pointer makes it: the clock is a notifying static, the others buttons */
    if (button) { PostMessageW(button, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(4, 4)); PostMessageW(button, WM_LBUTTONUP, 0, MAKELPARAM(4, 4)); }
    window = button ? wait_window(cls, TRUE, 5000) : NULL;
    wsprintfA(text, "%s opens", name);
    if (!optional) check(window != NULL, text);
    else { wsprintfA(text, "shell-surfaces note: %s %s\n", name, window ? "opened" : "did not open (no notification-area icons)"); line(text); }
    if (window) { GetWindowRect(window, &r); pump(300); }
    wsprintfA(text, "shell-surfaces surface number=%u name=%s rect=%ld,%ld,%ld,%ld\n", number, name, r.left, r.top, r.right, r.bottom); line(text);
    wsprintfA(text, "%s capture", name); check(capture_display(number), text);
    if (window) { PostMessageW(window, WM_CLOSE, 0, 0); wait_window(cls, FALSE, 3000); }
    (void)tray;
}
void mainCRTStartup(void)
{
    const char *arguments = GetCommandLineA(); char text[160]; HWND shell = NULL, tray, start = NULL;
    for (const char *c = arguments; *c; ++c)
        if ((c == arguments || c[-1] == ' ') && !strncmp(c, "--capture=", 10))
        {
            const char *value = c + 10, *end = value;
            while (*end && *end != ' ') ++end;
            if (end - value >= MAX_PATH - 24) ExitProcess(2);
            lstrcpynA(capture_base, value, (int)(end - value) + 1); break;
        }
    DWORD begun = GetTickCount();
    do { pump(100); shell = GetShellWindow(); } while (!IsWindow(shell) && GetTickCount() - begun < 15000);
    check(IsWindow(shell), "Explorer is the shell");
    tray = wait_window(L"Shell_TrayWnd", TRUE, 10000); check(tray != NULL, "taskbar");
    check(SUCCEEDED(apply_theme(L"Blue")), "light theme");
    /* The capture shows Start's default pins; the user's come back afterwards. */
    BYTE *saved = NULL; DWORD saved_size = 0, saved_type = 0; HKEY key;
    LONG saved_result = RegGetValueW(HKEY_CURRENT_USER, pin_key, L"StartPins", RRF_RT_ANY, &saved_type, NULL, &saved_size);
    if (saved_result == ERROR_SUCCESS && (saved = HeapAlloc(GetProcessHeap(), 0, saved_size)) &&
        RegGetValueW(HKEY_CURRENT_USER, pin_key, L"StartPins", RRF_RT_ANY, &saved_type, saved, &saved_size)) saved_result = ERROR_INVALID_DATA;
    check(saved_result == ERROR_SUCCESS || saved_result == ERROR_FILE_NOT_FOUND || saved_result == ERROR_PATH_NOT_FOUND, "read the user's pins");
    if (saved_result == ERROR_SUCCESS && !RegOpenKeyExW(HKEY_CURRENT_USER, pin_key, 0, KEY_SET_VALUE, &key))
    { RegDeleteValueW(key, L"StartPins"); RegCloseKey(key); }
    SetCursorPos(1910, 8);
    check(capture_display(1), "desktop capture");
    if (tray) EnumChildWindows(tray, find_start, (LPARAM)&start);
    check(start != NULL, "Start button");
    if (start) PostMessageW(start, BM_CLICK, 0, 0);
    HWND panel = start ? wait_window(L"MihawkStart", TRUE, 5000) : NULL; RECT r = {0};
    check(panel != NULL, "Start opens");
    if (panel) { GetWindowRect(panel, &r); pump(300); }
    wsprintfA(text, "shell-surfaces surface number=2 name=start rect=%ld,%ld,%ld,%ld\n", r.left, r.top, r.right, r.bottom); line(text);
    check(capture_display(2), "Start capture");
    check(SUCCEEDED(apply_theme(L"Dark")), "dark theme");
    pump(300);
    wsprintfA(text, "shell-surfaces surface number=3 name=start-dark rect=%ld,%ld,%ld,%ld\n", r.left, r.top, r.right, r.bottom); line(text);
    check(capture_display(3), "dark Start capture");
    check(SUCCEEDED(apply_theme(L"Blue")), "light theme again");
    if (panel) { PostMessageW(panel, WM_CLOSE, 0, 0); wait_window(L"MihawkStart", FALSE, 3000); }
    /* the taskbar's fixed buttons (patch 1140): search 0xffe0, chevron 0xffe2, status 0xffe3; the clock 0xfff1 */
    surface(tray, tray ? GetDlgItem(tray, 0xffe0) : NULL, L"MihawkStart", 4, "search", FALSE);
    surface(tray, tray ? GetDlgItem(tray, 0xffe3) : NULL, L"MihawkQuickSettings", 5, "quicksettings", FALSE);
    surface(tray, tray ? GetDlgItem(tray, 0xfff1) : NULL, L"MihawkNotifications", 6, "notifications", FALSE);
    surface(tray, tray ? GetDlgItem(tray, 0xffe2) : NULL, L"MihawkHiddenIcons", 7, "hiddenicons", TRUE);
    if (saved_result == ERROR_SUCCESS && saved && !RegCreateKeyExW(HKEY_CURRENT_USER, pin_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL))
    { check(!RegSetValueExW(key, L"StartPins", 0, saved_type, saved, saved_size), "restore the user's pins"); RegCloseKey(key); }
    wsprintfA(text, "PS5_PROTON_SHELL_SURFACES_PROBE %s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures); line(text);
    if (IsWindow(shell)) PostMessageW(shell, WM_CLOSE, 0, 0);
    ExitProcess(failures ? 1 : 0);
}
