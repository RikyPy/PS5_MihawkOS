/* SPDX-License-Identifier: GPL-3.0-or-later
 * Run as mihawk.exe <this PE64 executable> (Explorer's Default desktop).
 * Both architecture children inherit the shell desktop and prefix. Closing
 * them must leave Explorer alive; WM_CLOSE on the shell window (Explorer's
 * Progman; the desktop window is the system's, as on Windows) then ends the
 * root normally. No game data, forced termination or timing-based success verdict. */
#include <windows.h>
#include <dwmapi.h>
#include <string.h>

static unsigned checks, failures, paints;
static char capture_base[MAX_PATH];
static BOOL dark_theme;

static void check(BOOL okay, const char *what)
{
    char line[256]; DWORD written;
    checks++;
    if (!okay) failures++;
    wsprintfA(line, "desktop-session %s: %s error=%lu\n", okay ? "PASS" : "FAIL", what, GetLastError());
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, lstrlenA(line), &written, NULL);
    OutputDebugStringA(line);
}

/* Explorer's visible bounds (DWM's: inside the invisible resize border), whether it is active, the colour win32u
 * paints its frame and bar in, and the pointer, for the frame verdict in tools/run-desktop-session.py. */
static void log_frame(HWND window, const char *what)
{
    RECT r = {0}, c = {0}; POINT origin = {0, 0}, pointer = {0, 0}; char line[256]; DWORD written;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(window, &r);
    GetClientRect(window, &c); ClientToScreen(window, &origin); GetCursorPos(&pointer);
    const int active = GetForegroundWindow() == window;
    const DWORD bar = GetSysColor(active ? COLOR_ACTIVECAPTION : COLOR_INACTIVECAPTION);
    wsprintfA(line, "desktop-session %s frame=%d,%d,%d,%d client=%d,%d,%d,%d active=%d bar=%u,%u,%u pointer=%d,%d\n",
              what, (int)r.left, (int)r.top, (int)r.right, (int)r.bottom, (int)origin.x, (int)origin.y, (int)(origin.x + c.right),
              (int)(origin.y + c.bottom), active, GetRValue(bar), GetGValue(bar), GetBValue(bar), (int)pointer.x, (int)pointer.y);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, lstrlenA(line), &written, NULL);
    OutputDebugStringA(line);
}

/* --dark: the managed theme's Dark variant for this run, applied as the appearance app applies it (uxtheme's
 * OpenThemeFile, ApplyTheme, CloseThemeFile: ordinals 2, 4, 3), and the prior theme put back after, so the shared
 * probe prefix stays as it was. */
typedef HRESULT (WINAPI *open_theme_fn)(const WCHAR *, const WCHAR *, const WCHAR *, HANDLE *, DWORD);
typedef HRESULT (WINAPI *apply_theme_fn)(HANDLE, char *, HWND);
typedef HRESULT (WINAPI *close_theme_fn)(HANDLE);
typedef HRESULT (WINAPI *theme_name_fn)(WCHAR *, int, WCHAR *, int, WCHAR *, int);
static WCHAR prior_file[MAX_PATH], prior_color[64], prior_size[64];
static BOOL prior_theme;
static HRESULT apply_theme(const WCHAR *path, const WCHAR *color, const WCHAR *size)
{
    HMODULE themes = LoadLibraryA("uxtheme.dll");
    open_theme_fn open_theme = themes ? (open_theme_fn)GetProcAddress(themes, MAKEINTRESOURCEA(2)) : NULL;
    close_theme_fn close_theme = themes ? (close_theme_fn)GetProcAddress(themes, MAKEINTRESOURCEA(3)) : NULL;
    apply_theme_fn apply = themes ? (apply_theme_fn)GetProcAddress(themes, MAKEINTRESOURCEA(4)) : NULL;
    HANDLE file = NULL; HRESULT result;
    if (!open_theme || !close_theme || !apply) return E_NOTIMPL;
    if (!path) return apply(NULL, NULL, NULL);
    result = open_theme(path, color, size, &file, 0);
    if (SUCCEEDED(result)) { result = apply(file, NULL, NULL); close_theme(file); }
    return result;
}
static void select_dark(void)
{
    HMODULE themes = LoadLibraryA("uxtheme.dll");
    theme_name_fn name = themes ? (theme_name_fn)GetProcAddress(themes, "GetCurrentThemeName") : NULL;
    WCHAR path[MAX_PATH];
    prior_theme = name && SUCCEEDED(name(prior_file, MAX_PATH, prior_color, 64, prior_size, 64));
    GetWindowsDirectoryW(path, MAX_PATH);
    lstrcatW(path, L"\\resources\\themes\\light\\light.msstyles");
    check(SUCCEEDED(apply_theme(path, L"Dark", L"NormalSize")), "apply the managed theme's Dark variant");
}
static void restore_theme(void)
{
    check(SUCCEEDED(prior_theme ? apply_theme(prior_file, prior_color, prior_size) : apply_theme(NULL, NULL, NULL)),
          "put the prior theme back");
}

static void pump(DWORD milliseconds)
{
    DWORD start = GetTickCount();
    do {
        MSG message;
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        Sleep(10);
    } while (GetTickCount() - start < milliseconds);
}

/* Capture the actual displayed phase, never a fixed offset from shell startup.
 * The owner saves a complete numbered PPM after its displayed flip. Keep the
 * window alive and pumping until that file exists, with a bounded deadline. */
static BOOL capture_display(unsigned number, HWND window)
{
    if(!capture_base[0])return TRUE;
    char stem[MAX_PATH],path[MAX_PATH],request[MAX_PATH];
    if(lstrlenA(capture_base)>=MAX_PATH-24)return FALSE;
    lstrcpyA(stem,capture_base);char *dot=NULL;
    for(char *c=stem;*c;++c)if(*c=='.')dot=c;
    if(dot)*dot=0;
    wsprintfA(path,"%s-%02u.ppm",stem,number);
    wsprintfA(request,"%s.broker-request",capture_base);
    DeleteFileA(path);
    RedrawWindow(window,NULL,NULL,RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW|RDW_ALLCHILDREN);
    pump(100);
    HANDLE file=CreateFileA(request,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE)return FALSE;
    if(!CloseHandle(file))return FALSE;
    DWORD started=GetTickCount();unsigned complete_ticks=0;
    while(GetTickCount()-started<10000) {
        WIN32_FILE_ATTRIBUTE_DATA image;
        BOOL complete=GetFileAttributesExA(path,GetFileExInfoStandard,&image) && !image.nFileSizeHigh &&
                      image.nFileSizeLow==1920*1080*3+17;
        if(complete && ++complete_ticks==3)return TRUE;
        if(!complete)complete_ticks=0;
        pump(20);
    }
    DeleteFileA(request);return FALSE;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        HBRUSH brush = CreateSolidBrush(sizeof(void *) == 4 ? RGB(30, 180, 70) : RGB(30, 110, 210));
        FillRect(dc, &paint.rcPaint, brush);
        DeleteObject(brush);
        SetTextColor(dc, RGB(255,255,255));
        SetBkMode(dc, TRANSPARENT);
        const char *text = sizeof(void *) == 4 ? "Mihawk desktop: 32-bit child" : "Mihawk desktop: 64-bit child";
        TextOutA(dc, 20, 30, text, lstrlenA(text));
        EndPaint(window, &paint);
        paints++;
        return 0;
    }
    return DefWindowProcA(window, message, w, l);
}

static DWORD child(void)
{
    WNDCLASSA type = {0}; char path[MAX_PATH], value[64];
    type.lpfnWndProc = window_proc;
    type.hInstance = GetModuleHandleA(NULL);
    type.lpszClassName = "MihawkDesktopSessionProbe";
    check(IsWindow(GetShellWindow()), "child inherits live shell desktop");
    check(GetEnvironmentVariableA("MIHAWK_SESSION_PROBE", value, sizeof(value)) &&
          !lstrcmpA(value, "shared-session"), "child environment inherited");
    check(GetEnvironmentVariableA("MIHAWK_SESSION_FILE", path, sizeof(path)) &&
          GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES, "child sees shared C drive file");
    check(RegisterClassA(&type) != 0, "register child window");
    HWND window = CreateWindowExA(0, type.lpszClassName, "Mihawk session probe",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 300, 220, 700, 450, NULL, NULL, type.hInstance, NULL);
    check(window != NULL, "create visible child window");
    if (window) {
        check(SetForegroundWindow(window) && GetForegroundWindow()==window,"child becomes foreground");
        UpdateWindow(window);
        check(capture_display(sizeof(void*)==8?1:2,window),"child displayed capture completes");
        check(paints > 0 && IsWindowVisible(window), "child paints and remains visible");
        DestroyWindow(window);
    }
    return failures ? 1 : 23;
}

static BOOL run_child(char *command, DWORD expected)
{
    STARTUPINFOA startup = {0}; PROCESS_INFORMATION process = {0}; DWORD code = 0;
    startup.cb = sizeof(startup);
    BOOL started = CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process);
    check(started, "create desktop child");
    if (!started) return FALSE;
    DWORD wait = WaitForSingleObject(process.hProcess, 30000);
    check(wait == WAIT_OBJECT_0, "child exits within deadline");
    BOOL okay = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code) && code == expected;
    check(okay, "child exit code");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return okay;
}

struct BrowserWindows { DWORD pid; HWND window; };
static BOOL CALLBACK find_browser(HWND window, LPARAM parameter);
static HWND shell;
static BOOL CALLBACK find_browser(HWND window, LPARAM parameter)
{
    struct BrowserWindows *browser = (struct BrowserWindows *)parameter;
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (pid == browser->pid && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) browser->window = window;
    return TRUE;
}

/* A key with up to two modifiers (0: none) as a user presses it: File Explorer's shortcuts, a menu's keys. */
static void press(WORD key, WORD modifier, WORD second)
{
    INPUT keys[6]; WORD codes[3]; int n = 0;
    if (modifier) codes[n++] = modifier;
    if (second) codes[n++] = second;
    codes[n++] = key;
    memset(keys, 0, sizeof(keys));
    for (int i = 0; i < n; i++) {
        keys[i].type = keys[2 * n - 1 - i].type = INPUT_KEYBOARD;
        keys[i].ki.wVk = keys[2 * n - 1 - i].ki.wVk = codes[i];
        keys[2 * n - 1 - i].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    check(SendInput(2 * n, keys, sizeof(INPUT)) == (UINT)(2 * n), "press a File Explorer shortcut");
}

/* A left click at a screen point, as a person's hand sends it. */
static void mouse(DWORD flags, int x, int y)
{
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dx = x * 65535 / 1919;
    input.mi.dy = y * 65535 / 1079;
    input.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &input, sizeof(input));
}
static void click(int x, int y)
{
    mouse(MOUSEEVENTF_MOVE, x, y); pump(40);
    mouse(MOUSEEVENTF_LEFTDOWN, x, y); pump(60);
    mouse(MOUSEEVENTF_LEFTUP, x, y); pump(200);
}
/* Filter's menu, from its button where the prototype has it (the window at 410,166): its second kind, Documents. */
static void filter_documents(void)
{
    click(411 + 517 + 43, 167 + 103 + 16);
    press(VK_DOWN, 0, 0); press(VK_DOWN, 0, 0); press(VK_RETURN, 0, 0);
    pump(1000);
}

static BOOL titled(HWND window, const char *expected)
{
    char title[MAX_PATH];
    return GetWindowTextA(window, title, sizeof(title)) && !lstrcmpA(title, expected);
}

/* Start File Explorer with a command line, place it where the prototype's File Explorer is (1100 x 700 at 410,166 on
 * its 1920 x 1080 desktop, so the capture compares with the prototype pixel for pixel: the window rect adds the
 * invisible frame), log its frame as "desktop-session <what> frame=...", capture the display and close it. */
static BOOL browse(const char *line, unsigned capture, const char *what)
{
    STARTUPINFOA startup = {0}; PROCESS_INFORMATION process = {0}; char command[2 * MAX_PATH]; BOOL clean = FALSE;
    startup.cb = sizeof(startup);
    lstrcpynA(command, line, sizeof(command));
    BOOL started = CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process);
    check(started, "start real File Explorer");
    if (!started) return FALSE;
    struct BrowserWindows browser = {process.dwProcessId, NULL};
    DWORD start = GetTickCount();
    do { pump(100); EnumWindows(find_browser, (LPARAM)&browser); }
    while (!browser.window && GetTickCount() - start < 15000);
    check(browser.window != NULL, "File Explorer has a visible window");
    if (browser.window) {
        RECT window, visible;
        GetWindowRect(browser.window, &window);
        if (FAILED(DwmGetWindowAttribute(browser.window, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible)))) visible = window;
        SetWindowPos(browser.window, HWND_TOP, 410 - (visible.left - window.left), 166 - (visible.top - window.top),
                     1100 + (window.right - window.left) - (visible.right - visible.left),
                     700 + (window.bottom - window.top) - (visible.bottom - visible.top), SWP_SHOWWINDOW);
        SetForegroundWindow(browser.window);
    }
    check(browser.window && GetForegroundWindow()==browser.window,"File Explorer becomes foreground");
    if (browser.window) log_frame(browser.window, what);
    check(browser.window && capture_display(capture,browser.window),"File Explorer displayed capture completes");
    /* A folder, then in large icons (capture 5), small icons (6) and list (7) by Ctrl+Shift+2, 4, 5; then a new tab
     * on Home beside it (Ctrl+T, capture 10), back to the folder's tab (Ctrl+Tab) and that tab closed (Ctrl+W). */
    static const char *const views[] = {"large", "small", "list"};
    for (int i = 0; browser.window && !lstrcmpA(what, "folder") && i < 3; i++) {
        press("245"[i], VK_CONTROL, VK_SHIFT);
        pump(1000);
        log_frame(browser.window, views[i]);
        check(capture_display(capture + 1 + i, browser.window), "File Explorer view capture completes");
    }
    /* Details view again with the details pane (Alt+Shift+P) and Filter's Documents, which none of the folder's items
     * is (capture 8), then the filter off (Documents again) and the first item clicked (capture 9), the pane closed. */
    if (browser.window && !lstrcmpA(what, "folder")) {
        press('6', VK_CONTROL, VK_SHIFT);
        press('P', VK_MENU, VK_SHIFT);
        pump(800);
        filter_documents();
        log_frame(browser.window, "filter");
        check(capture_display(capture + 4, browser.window), "File Explorer filter capture completes");
        filter_documents();
        click(411 + 177 + 60, 167 + 178 + 16);
        pump(800);
        log_frame(browser.window, "pane");
        check(capture_display(capture + 5, browser.window), "File Explorer details pane capture completes");
        press('P', VK_MENU, VK_SHIFT);
        mouse(MOUSEEVENTF_MOVE, 100, 1000); /* off the window: no plate under the pointer in the next capture */
        pump(500);
    }
    if (browser.window && !lstrcmpA(what, "folder")) {
        press('T', VK_CONTROL, 0);
        pump(1000);
        check(titled(browser.window, "Home - File Explorer"), "Ctrl+T opens a tab on Home");
        log_frame(browser.window, "tabs");
        check(capture_display(capture + 6, browser.window), "File Explorer tabs capture completes");
        press(VK_TAB, VK_CONTROL, 0);
        pump(500);
        check(titled(browser.window, "Documents - File Explorer"), "Ctrl+Tab shows the folder's tab");
        press('W', VK_CONTROL, 0);
        pump(500);
        check(IsWindow(browser.window) && titled(browser.window, "Home - File Explorer"), "Ctrl+W closes the tab, not the window");
        /* From that tab's Home to Documents (the navigation pane's row), then up a folder (Alt+Up): a folder to folder
         * navigation, its first frame the prototype's (capture 11; Explorer logs each new view as hidden until styled). */
        click(411 + 74, 167 + 309);
        mouse(MOUSEEVENTF_MOVE, 100, 1000);
        pump(800);
        check(titled(browser.window, "Documents - File Explorer"), "the navigation pane opens Documents");
        press(VK_UP, VK_MENU, 0);
        pump(800);
        log_frame(browser.window, "parent");
        check(capture_display(capture + 7, browser.window), "File Explorer parent folder capture completes");
    }
    if (browser.window) PostMessageA(browser.window, WM_CLOSE, 0, 0);
    DWORD wait = WaitForSingleObject(process.hProcess, 10000), code = 1;
    clean = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code) && !code;
    check(clean, "File Explorer closes normally");
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    check(IsWindow(shell) && GetShellWindow() == shell, "shell survives File Explorer exit");
    return clean;
}

static void parent(void)
{
    char module[MAX_PATH], command[2 * MAX_PATH], path[MAX_PATH], line[256]; DWORD written;
    shell = NULL;
    DWORD shell_start = GetTickCount();
    do { pump(100); shell = GetShellWindow(); }
    while (!IsWindow(shell) && GetTickCount() - shell_start < 15000);
    check(IsWindow(shell), "Explorer is the persistent shell");
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    char input_name[128], thread_name[128];
    BOOL same_desktop = input &&
        GetUserObjectInformationA(input, UOI_NAME, input_name, sizeof(input_name), NULL) &&
        GetUserObjectInformationA(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, thread_name, sizeof(thread_name), NULL) &&
        !lstrcmpA(input_name, thread_name);
    check(same_desktop, "shell desktop is the input desktop");
    if (input) CloseDesktop(input);
    check(GetSystemMetrics(SM_CXSCREEN) == 1920 && GetSystemMetrics(SM_CYSCREEN) == 1080, "1920x1080 desktop");
    GetTempPathA(sizeof(path), path);
    char filename[64]; wsprintfA(filename, "mihawk-session-%lu.tmp", GetCurrentProcessId());
    lstrcatA(path, filename);
    DeleteFileA(path); /* a run that ended early leaves its file, and the next process may get the same number */
    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    check(file != INVALID_HANDLE_VALUE, "create session file in prefix");
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    SetEnvironmentVariableA("MIHAWK_SESSION_PROBE", "shared-session");
    SetEnvironmentVariableA("MIHAWK_SESSION_FILE", path);
    GetModuleFileNameA(NULL, module, sizeof(module));
    wsprintfA(command, "\"%s\" --child", module);
    BOOL clean = run_child(command, 23);
    check(IsWindow(shell) && GetShellWindow() == shell, "shell survives PE64 child exit");
    char *name = module;
    for (char *p = module; *p; p++) if (*p == '\\' || *p == '/') name = p + 1;
    lstrcpyA(name, "desktop-session32-probe.exe");
    wsprintfA(command, "\"%s\" --child", module);
    if (clean) clean = run_child(command, 23);
    check(IsWindow(shell) && GetShellWindow() == shell, "shell survives PE32 child exit");
    if (dark_theme) { select_dark(); pump(1000); }
    /* File Explorer on Home (capture 3), then on the user's Documents (capture 4), each where the prototype's is. */
    if (clean) clean = browse("explorer.exe", 3, "explorer");
    if (clean) {
        char documents[MAX_PATH + 32] = "explorer.exe \"";
        DWORD length = GetEnvironmentVariableA("USERPROFILE", documents + 14, MAX_PATH);
        check(length && length < MAX_PATH, "the user's profile folder is known");
        if (length && length < MAX_PATH) {
            lstrcatA(documents, "\\Documents\"");
            clean = browse(documents, 4, "folder");
        }
    }
    if (dark_theme) restore_theme();
    DeleteFileA(path);
    wsprintfA(line, "PS5_PROTON_DESKTOP_SESSION_PROBE %s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, lstrlenA(line), &written, NULL);
    OutputDebugStringA(line);
    /* If a child is still alive, leave the owner intact for its bounded runner
     * to stop through the existing normal path rather than abandoning it. */
    if (clean && IsWindow(shell)) PostMessageA(shell, WM_CLOSE, 0, 0);
}

void mainCRTStartup(void)
{
    const char *command = GetCommandLineA(); BOOL is_child = FALSE;
    for (; *command; command++) if (!lstrcmpA(command, "--child")) { is_child = TRUE; break; }
    if(is_child) {
        GetEnvironmentVariableA("MIHAWK_SESSION_CAPTURE_BASE",capture_base,sizeof(capture_base));
        ExitProcess(child());
    }
    const char *arguments=GetCommandLineA();
    for(const char *c=arguments+1;*arguments && *c;++c)if(c[-1]==' ' && !strncmp(c,"--dark",6) && (!c[6] || c[6]==' '))dark_theme=TRUE;
    for(const char *c=arguments;*c;++c)if((c==arguments || c[-1]==' ') && !strncmp(c,"--capture=",10)) {
        const char *value=c+10,*end=value;while(*end && *end!=' ')++end;
        if(end-value>=MAX_PATH-24)ExitProcess(2);
        lstrcpynA(capture_base,value,(int)(end-value)+1);
        SetEnvironmentVariableA("MIHAWK_SESSION_CAPTURE_BASE",capture_base);break;
    }
    parent();
    ExitProcess(failures ? 1 : 0);
}
