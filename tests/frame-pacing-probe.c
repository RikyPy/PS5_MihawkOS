/* SPDX-License-Identifier: GPL-3.0-or-later
 * Run in the desktop session (run-console.py --profile mihawk-os): the composed desktop in the scenes the owner feels
 * its refresh rate in, each ended by a display capture, which ends the GPU broker's phase (graphics patch 0019: the
 * phase's compositions and the pacing of the desktop's presents), for tools/run-frame-pacing.py.
 *   1 start-up      2 idle desktop (8 s)         3 Start opening (300 ms)        4 Start held (6 s)
 *   5 Quick Settings opening (300 ms)            6 Quick Settings held (6 s)     7 a window opened (500 ms)
 *   8 that window dragged down across the taskbar and back, a move at each 1/60 s (6 s)
 *   9 the desktop's menu opening, right-clicked at 600,400 (300 ms)
 *  10 the pointer down its rows and back, a move at each 1/60 s, each row's plate drawn over the acrylic (6 s)
 *  11 File Explorer opening (500 ms): its first composition builds Mica Alt's image of the wallpaper
 * Each sweep (8, 10) writes its own cadence as a note: the periods between its moves and what each move call took, so a
 * slow interval of the broker's (graphics patch 0020: content, compose, copy, wait) can be set beside it. The sweeps move
 * on a 60 Hz grid: a move then 16 ms of messages ran at 19.0 ms a move on the console (Sleep and the tick count overshoot),
 * and that cadence was all of the drag's intervals over 20 ms (frame-pacing-2026-10-17-shell-batch-01: 99 of 99 content).
 * The pointer rests in the top right corner. A surface that does not open is a failed check; its phases still end. */
#include <windows.h>
#include <string.h>

static unsigned checks, failures;
static char capture_base[MAX_PATH];

static void line(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}
static void check(BOOL okay, const char *what)
{
    char text[256]; ++checks; failures += !okay;
    wsprintfA(text, "frame-pacing %s: %s error=%lu\n", okay ? "PASS" : "FAIL", what, GetLastError()); line(text);
}
static void pump(DWORD milliseconds)
{
    DWORD start = GetTickCount();
    do { MSG message; while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); } Sleep(1); }
    while (GetTickCount() - start < milliseconds);
}
/* The title saves a complete numbered PPM after its displayed flip; the broker ends its phase as it takes it. */
static BOOL capture_display(unsigned number, const char *phase)
{
    char stem[MAX_PATH], path[MAX_PATH], request[MAX_PATH], text[160], *dot = NULL;
    wsprintfA(text, "frame-pacing phase number=%u name=%s\n", number, phase); line(text);
    if (!capture_base[0] || lstrlenA(capture_base) >= MAX_PATH - 24) return FALSE;
    lstrcpyA(stem, capture_base);
    for (char *c = stem; *c; ++c) if (*c == '.') dot = c;
    if (dot) *dot = 0;
    wsprintfA(path, "%s-%02u.ppm", stem, number); wsprintfA(request, "%s.broker-request", capture_base);
    DeleteFileA(path);
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
static LONGLONG now_us(void)
{
    LARGE_INTEGER t, f; QueryPerformanceCounter(&t); QueryPerformanceFrequency(&f);
    return t.QuadPart / f.QuadPart * 1000000 + t.QuadPart % f.QuadPart * 1000000 / f.QuadPart;
}
/* Messages until the next 1/60 s of a grid that starts at from. */
static void pump_to_frame(LONGLONG from)
{
    const LONGLONG due = from + ((now_us() - from) * 60 / 1000000 + 1) * 1000000 / 60;
    do { MSG message; while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
         if (now_us() >= due) break; Sleep(1); }
    while (1);
}
/* A 6 s sweep: a move, then messages until the next 1/60 s; its cadence as a note. */
static void sweep(const char *name, void (*move)(void *, unsigned), void *context)
{
    char text[256]; unsigned moves = 0, over20 = 0; LONGLONG period_sum = 0, period_max = 0, call_sum = 0, call_max = 0, last = 0;
    const LONGLONG from = now_us();
    while (now_us() - from < 6000000)
    {
        const LONGLONG before = now_us();
        move(context, moves);
        const LONGLONG after = now_us(), period = before - last;
        if (moves) { period_sum += period; if (period > period_max) period_max = period; over20 += period > 20000; }
        last = before; call_sum += after - before; if (after - before > call_max) call_max = after - before;
        ++moves;
        pump_to_frame(from);
    }
    wsprintfA(text, "frame-pacing note: %s moves=%u in %u ms period_avg_us=%u period_max_us=%u periods_over20=%u "
              "call_avg_us=%u call_max_us=%u\n", name, moves, (unsigned)((now_us() - from) / 1000),
              moves > 1 ? (unsigned)(period_sum / (moves - 1)) : 0u, (unsigned)period_max, over20,
              moves ? (unsigned)(call_sum / moves) : 0u, (unsigned)call_max);
    line(text);
}
static void drag_move(void *window, unsigned moves)
{
    const int step = (int)(moves % 40), y = 700 + (step < 20 ? step : 40 - step) * 15; /* down to 985 and back up */
    SetWindowPos((HWND)window, NULL, 300, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
static void mouse(DWORD flags, int x, int y)
{
    INPUT input = {0};
    input.type = INPUT_MOUSE; input.mi.dx = x * 65535 / 1919; input.mi.dy = y * 65535 / 1079;
    input.mi.dwFlags = flags | MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &input, sizeof(input));
}
static void hover_move(void *unused, unsigned moves)
{
    const int step = (int)(moves % 40), y = 410 + (step < 20 ? step : 40 - step) * 13; /* 410 to 670 and back */
    (void)unused; mouse(0, 700, y);
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
struct Browser { DWORD pid; HWND window; };
static BOOL CALLBACK find_browser(HWND window, LPARAM parameter)
{
    struct Browser *browser = (struct Browser *)parameter; DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (pid == browser->pid && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) browser->window = window;
    return TRUE;
}
/* File Explorer started as the desktop-session probe starts it, measured from its first visible window, then closed. */
static void explorer_opening(unsigned number)
{
    STARTUPINFOA startup = {0}; PROCESS_INFORMATION process = {0}; char command[] = "explorer.exe";
    startup.cb = sizeof(startup);
    BOOL started = CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process);
    check(started, "start File Explorer");
    struct Browser browser = {started ? process.dwProcessId : 0, NULL};
    DWORD begun = GetTickCount();
    while (started && !browser.window && GetTickCount() - begun < 15000) { pump(20); EnumWindows(find_browser, (LPARAM)&browser); }
    check(browser.window != NULL, "explorer-opening");
    pump(500); check(capture_display(number, "explorer-opening"), "explorer-opening");
    if (browser.window) PostMessageW(browser.window, WM_CLOSE, 0, 0);
    if (started) { WaitForSingleObject(process.hProcess, 5000); CloseHandle(process.hProcess); CloseHandle(process.hThread); }
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_PAINT)
    {
        PAINTSTRUCT paint; HDC dc = BeginPaint(window, &paint);
        HBRUSH brush = CreateSolidBrush(RGB(240, 180, 30)); FillRect(dc, &paint.rcPaint, brush); DeleteObject(brush);
        EndPaint(window, &paint); return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
/* A surface held open: its opening phase (300 ms) and its held phase (6 s), then dismissed. */
static void held(HWND button, BOOL post_click, const WCHAR *cls, unsigned number, const char *name)
{
    char opening[64], open[64];
    check(button != NULL, name);
    if (button && post_click) PostMessageW(button, BM_CLICK, 0, 0);
    else if (button) { PostMessageW(button, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(4, 4)); PostMessageW(button, WM_LBUTTONUP, 0, MAKELPARAM(4, 4)); }
    HWND window = button ? wait_window(cls, TRUE, 5000) : NULL;
    wsprintfA(opening, "%s-opening", name); wsprintfA(open, "%s-held", name);
    check(window != NULL, opening);
    pump(300); check(capture_display(number, opening), opening);
    pump(6000); check(capture_display(number + 1, open), open);
    if (window) { PostMessageW(window, WM_CLOSE, 0, 0); wait_window(cls, FALSE, 3000); }
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
    SetCursorPos(1910, 8);
    pump(2000); check(capture_display(1, "start-up"), "start-up");
    pump(8000); check(capture_display(2, "idle"), "idle");
    if (tray) EnumChildWindows(tray, find_start, (LPARAM)&start);
    held(start, TRUE, L"MihawkStart", 3, "start");
    /* the taskbar's status button (patch 1140, 0xffe3) opens Quick Settings */
    held(tray ? GetDlgItem(tray, 0xffe3) : NULL, FALSE, L"MihawkQuickSettings", 5, "quicksettings");
    WNDCLASSW type = {0}; type.lpfnWndProc = window_proc; type.hInstance = GetModuleHandleW(NULL);
    type.lpszClassName = L"MihawkFramePacing"; type.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&type);
    HWND moving = CreateWindowW(L"MihawkFramePacing", L"Frame pacing", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                300, 700, 500, 300, NULL, NULL, GetModuleHandleW(NULL), NULL);
    check(moving != NULL, "window to drag");
    pump(500); check(capture_display(7, "window-open"), "window-open");
    if (moving) sweep("drag", drag_move, moving);
    check(capture_display(8, "drag"), "drag");
    if (moving) DestroyWindow(moving);
    /* the desktop's menu (Wine patch 1190), its plates following the pointer over its 8 rows from 403 */
    pump(500);
    mouse(0, 600, 400); pump(60); mouse(MOUSEEVENTF_RIGHTDOWN, 600, 400); pump(60); mouse(MOUSEEVENTF_RIGHTUP, 600, 400);
    HWND menu = wait_window(L"#32768", TRUE, 5000);
    check(menu != NULL, "desktop menu");
    pump(300); check(capture_display(9, "menu-opening"), "menu-opening");
    if (menu) sweep("menu-hover", hover_move, NULL);
    check(capture_display(10, "menu-hover"), "menu-hover");
    if (menu) { mouse(MOUSEEVENTF_LEFTDOWN, 1910, 8); mouse(MOUSEEVENTF_LEFTUP, 1910, 8); wait_window(L"#32768", FALSE, 3000); }
    SetCursorPos(1910, 8);
    explorer_opening(11);
    wsprintfA(text, "PS5_PROTON_FRAME_PACING_PROBE %s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures); line(text);
    if (IsWindow(shell)) PostMessageW(shell, WM_CLOSE, 0, 0);
    ExitProcess(failures ? 1 : 0);
}
