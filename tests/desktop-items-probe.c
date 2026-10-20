/* SPDX-License-Identifier: GPL-3.0-or-later
 * What a person does with the desktop's items, and what the desktop shows over time (the user's report of 2026-10-12: the launchers go
 * transparent or black now and then, a right click has no Open, some shortcuts do not run). Run inside the Mihawk session:
 *
 *   desktop-items-probe [--capture=PATH] [--soak=N]
 *
 * It lists the menu a right click opens on each kind of item, double clicks a shortcut and looks for its window, resolves every shortcut
 * on both Desktops (target, folder and icon must exist), then takes N pictures 20 s apart and measures how much of the icon column
 * changed between them. Prints PS5_PROTON_DESKTOP_ITEMS PASS/FAIL checks=N failures=M. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#define FILES 0
static unsigned checks, failures;
static char capture_base[MAX_PATH];

static HWND flicker_window;
static void wiggle(void);

static void line(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void check(BOOL okay, const char *name)
{
    char text[200];
    ++checks;
    failures += !okay;
    wsprintfA(text, "desktop-items %s: %s error=%lu\n", okay ? "PASS" : "FAIL", name, GetLastError());
    line(text);
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

/* The owner saves a numbered picture of the next displayed frame when asked (as tests/desktop-session-probe.c does). */
static BOOL capture_display(unsigned number)
{
    char stem[MAX_PATH], path[MAX_PATH], request[MAX_PATH];
    char *dot = NULL, *c;
    HANDLE file;
    DWORD started;
    unsigned complete_ticks = 0;

    if (!capture_base[0]) return TRUE;
    if (lstrlenA(capture_base) >= MAX_PATH - 24) return FALSE;
    lstrcpyA(stem, capture_base);
    for (c = stem; *c; ++c) if (*c == '.') dot = c;
    if (dot) *dot = 0;
    wsprintfA(path, "%s-%02u.ppm", stem, number);
    wsprintfA(request, "%s.broker-request", capture_base);
    if (flicker_window) ShowWindow(flicker_window, SW_HIDE);
    DeleteFileA(path);
    /* a still desktop presents no new frame, and the owner saves the next one: ask for a repaint */
    RedrawWindow(GetShellWindow(), NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    pump(300);
    file = CreateFileA(request, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle(file);
    started = GetTickCount();
    while (GetTickCount() - started < 10000) {
        WIN32_FILE_ATTRIBUTE_DATA image;
        BOOL complete = GetFileAttributesExA(path, GetFileExInfoStandard, &image) && !image.nFileSizeHigh &&
                        image.nFileSizeLow == 1920 * 1080 * 3 + 17;
        if (complete && ++complete_ticks == 3) return TRUE;
        if (!complete) complete_ticks = 0;
        if (GetTickCount() - started > 600 && (GetTickCount() - started) % 500 < 30) wiggle();
        pump(20);
    }
    {
        WIN32_FILE_ATTRIBUTE_DATA image;
        char text[300];
        BOOL there = GetFileAttributesExA(path, GetFileExInfoStandard, &image);
        wsprintfA(text, "desktop-items note: picture %u not seen: file %s size %lu, request %s\n", number, there ? "exists" : "absent",
                  there ? image.nFileSizeLow : 0, GetFileAttributesA(request) != INVALID_FILE_ATTRIBUTES ? "still there" : "taken");
        line(text);
    }
    /* how late does it come, if it comes? */
    {
        DWORD waited = GetTickCount();
        char text[160];
        while (GetTickCount() - waited < 40000) {
            WIN32_FILE_ATTRIBUTE_DATA image;
            if (GetFileAttributesExA(path, GetFileExInfoStandard, &image) && image.nFileSizeLow == 1920 * 1080 * 3 + 17) break;
            pump(100);
        }
        wsprintfA(text, "desktop-items note: picture %u %s after %lu ms more\n", number,
                  GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? "appeared" : "never appeared", GetTickCount() - waited);
        line(text);
    }
    DeleteFileA(request);
    return FALSE;
}

/* Pointer and keys, as a person's hands would send them. */
static void mouse(DWORD flags, int x, int y)
{
    INPUT input = {0};
    input.type = INPUT_MOUSE;
    input.mi.dx = x * 65535 / 1919;
    input.mi.dy = y * 65535 / 1079;
    input.mi.dwFlags = flags | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &input, sizeof(input));
}

static void move(int x, int y) { mouse(MOUSEEVENTF_MOVE, x, y); pump(40); }

/* The owner saves the next frame that is displayed, and a still desktop shows none (a pointer that moves does not make one): a 4 pixel
 * window in the top right corner appears and disappears, which changes real pixels. */
static void wiggle(void)
{
    if (!flicker_window)
        flicker_window = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "STATIC", NULL, WS_POPUP | SS_WHITERECT,
                                         1914, 2, 4, 4, NULL, NULL, NULL, NULL);
    if (flicker_window) ShowWindow(flicker_window, IsWindowVisible(flicker_window) ? SW_HIDE : SW_SHOWNA);
}

static void key(WORD vk, BOOL down)
{
    INPUT input = {0};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(input));
}

static void tap(WORD vk) { key(vk, TRUE); pump(30); key(vk, FALSE); pump(60); }

/* A drag from (x0,y0) to (x1,y1) in steps, the button held. */
__attribute__((unused)) static void drag(int x0, int y0, int x1, int y1)
{
    int step;
    move(x0, y0);
    mouse(MOUSEEVENTF_LEFTDOWN, x0, y0);
    pump(80);
    for (step = 1; step <= 24; ++step) move(x0 + (x1 - x0) * step / 24, y0 + (y1 - y0) * step / 24);
    pump(120);
    mouse(MOUSEEVENTF_LEFTUP, x1, y1);
    pump(200);
}

__attribute__((unused)) static void say_focus(const char *where)
{
    char text[200], foreground[64] = "none", focus[64] = "none";
    HWND window = GetForegroundWindow(), inner = GetFocus();
    GUITHREADINFO info;
    info.cbSize = sizeof(info);
    if (window) GetClassNameA(window, foreground, sizeof(foreground));
    info.hwndFocus = NULL;
    if (GetGUIThreadInfo(0, &info) && info.hwndFocus) GetClassNameA(info.hwndFocus, focus, sizeof(focus));
    wsprintfA(text, "desktop-items note: %s foreground=%s focus=%s (this thread %p)\n", where, foreground, focus, inner);
    line(text);
}

static void click(int x, int y, BOOL right)
{
    move(x, y);
    mouse(right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN, x, y);
    pump(60);
    mouse(right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP, x, y);
    pump(200);
}

static HWND find_progman(void)
{
    DWORD start = GetTickCount();
    HWND window;
    while (!(window = FindWindowW(L"Progman", NULL)) && GetTickCount() - start < 30000) pump(100);
    return window;
}

static int item_count(HWND list) { return (int)SendMessageW(list, LVM_GETITEMCOUNT, 0, 0); }
static int selected_count(HWND list) { return (int)SendMessageW(list, LVM_GETSELECTEDCOUNT, 0, 0); }

__attribute__((unused)) static BOOL wait_items(HWND list, int wanted, DWORD milliseconds)
{
    DWORD start = GetTickCount();
    while (GetTickCount() - start < milliseconds) {
        if (item_count(list) >= wanted) return TRUE;
        pump(100);
    }
    return item_count(list) >= wanted;
}

__attribute__((unused)) static BOOL wait_selected(HWND list, int wanted, DWORD milliseconds)
{
    DWORD start = GetTickCount();
    while (GetTickCount() - start < milliseconds) {
        if (selected_count(list) == wanted) return TRUE;
        pump(80);
    }
    return selected_count(list) == wanted;
}
static BOOL starts_with(const char *text, const char *prefix)
{
    while (*prefix) if (*text++ != *prefix++) return FALSE;
    return TRUE;
}

static BOOL popup_menu_present(void) { return FindWindowW(L"#32768", NULL) != NULL; }

/* The list is Explorer's, in another process, so its items' text cannot be read from here (LVM_GETITEMTEXT carries a pointer). The shell
 * lays out a fresh desktop in column order on the prototype's grid (patch 1102, medium icons): This PC, the Recycle Bin, then the names
 * as sorted, 91 px apart, which is what this uses: an item's place is its position in that order. A wrong guess shows as the wrong menu
 * or window, and the pictures say which. The names are neutral fixtures, sorted as the shell sorts: a desktop holding other names
 * needs this list to match it. */
static const WCHAR *const known_names[] = { L"This PC", L"Recycle Bin", L"Appearance", L"Client Launcher", L"Demo Game - DirectX", L"Demo Game - OpenGL",
    L"Example Game", L"Example Online Game", L"Fixture Game", L"Java Game", L"Online Store Page", L"Sample Game", L"Store Launcher", L"The Native Game" };

static BOOL item_by_name(HWND list, const WCHAR *prefix, int *x, int *y, WCHAR *name, int name_size)
{
    unsigned i;
    (void)list;
    for (i = 0; i < sizeof(known_names) / sizeof(known_names[0]); i++) {
        if (StrCmpNIW(known_names[i], prefix, lstrlenW(prefix))) continue;
        *x = 38;                   /* the icon's middle: plate 2 + 13 + 24 */
        *y = 29 + 91 * (int)i;
        lstrcpynW(name, known_names[i], name_size);
        return TRUE;
    }
    return FALSE;
}

/* The menu belongs to Explorer's process, so its entries cannot be read from here; the picture taken while it is open shows them. */
static void right_click_menu(HWND list, const WCHAR *prefix, const char *what, unsigned picture)
{
    int x, y;
    WCHAR name[MAX_PATH];
    char text[300];
    if (!item_by_name(list, prefix, &x, &y, name, MAX_PATH)) {
        wsprintfA(text, "no desktop item starts with %ls", prefix);
        check(FALSE, text);
        return;
    }
    click(x, y, TRUE);
    pump(500);
    wsprintfA(text, "a right click on %s opens a menu", what);
    check(popup_menu_present(), text);
    wsprintfA(text, "picture %u: the menu of %s", picture, what);
    check(capture_display(picture), text);
    tap(VK_ESCAPE);
    pump(300);
}

/* Every shortcut on both Desktops: does what it points at exist. */
static void resolve_shortcuts(const WCHAR *folder, const char *tag)
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE search;
    int bad = 0, total = 0;
    char text[400];

    wsprintfW(pattern, L"%ls\\*.lnk", folder);
    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        IShellLinkW *link = NULL;
        IPersistFile *file = NULL;
        WCHAR full[MAX_PATH], target[MAX_PATH], directory[MAX_PATH], arguments[300], icon[MAX_PATH];
        int icon_index = 0;
        HRESULT hr;
        target[0] = directory[0] = arguments[0] = icon[0] = 0;
        wsprintfW(full, L"%ls\\%ls", folder, found.cFileName);
        hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
        if (SUCCEEDED(hr)) hr = IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file);
        if (SUCCEEDED(hr)) hr = IPersistFile_Load(file, full, STGM_READ);
        if (SUCCEEDED(hr)) {
            IShellLinkW_Resolve(link, NULL, SLR_NO_UI | SLR_NOUPDATE | SLR_NOSEARCH);
            IShellLinkW_GetPath(link, target, MAX_PATH, NULL, 0);
            IShellLinkW_GetWorkingDirectory(link, directory, MAX_PATH);
            IShellLinkW_GetArguments(link, arguments, 300);
            IShellLinkW_GetIconLocation(link, icon, MAX_PATH, &icon_index);
        }
        total++;
        {
            BOOL target_ok = target[0] && GetFileAttributesW(target) != INVALID_FILE_ATTRIBUTES;
            BOOL directory_ok = !directory[0] || GetFileAttributesW(directory) != INVALID_FILE_ATTRIBUTES;
            BOOL icon_ok = !icon[0] || GetFileAttributesW(icon) != INVALID_FILE_ATTRIBUTES;
            if (!SUCCEEDED(hr) || !target_ok || !directory_ok || !icon_ok) bad++;
            wsprintfA(text, "desktop-items note: %s shortcut '%ls': load=%#lx target='%ls' %s, folder '%ls' %s, icon '%ls' %s, arguments '%.60ls'\n", tag,
                      found.cFileName, hr, target, target_ok ? "exists" : "MISSING", directory, directory_ok ? "exists" : "MISSING", icon,
                      icon_ok ? "exists" : "MISSING", arguments);
            line(text);
        }
        if (file) IPersistFile_Release(file);
        if (link) IShellLinkW_Release(link);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    wsprintfA(text, "every shortcut on the %s Desktop points at things that exist (%d of %d do not)", tag, bad, total);
    check(bad == 0, text);
}

typedef struct { HWND found; const WCHAR *needle; } WINDOW_SEARCH;

static BOOL CALLBACK find_titled(HWND window, LPARAM parameter)
{
    WINDOW_SEARCH *search = (WINDOW_SEARCH *)parameter;
    WCHAR title[200];
    if (IsWindowVisible(window) && GetWindowTextW(window, title, 200) && StrStrIW(title, search->needle)) { search->found = window; return FALSE; }
    return TRUE;
}

static void double_click_runs(HWND list, const WCHAR *prefix, const WCHAR *window_title)
{
    int x, y;
    WCHAR name[MAX_PATH];
    WINDOW_SEARCH search = { NULL, window_title };
    DWORD start;
    char text[300];
    if (!item_by_name(list, prefix, &x, &y, name, MAX_PATH)) { wsprintfA(text, "no desktop item starts with %ls", prefix); check(FALSE, text); return; }
    move(x, y);
    mouse(MOUSEEVENTF_LEFTDOWN, x, y); pump(40); mouse(MOUSEEVENTF_LEFTUP, x, y); pump(60);
    mouse(MOUSEEVENTF_LEFTDOWN, x, y); pump(40); mouse(MOUSEEVENTF_LEFTUP, x, y);
    start = GetTickCount();
    while (!search.found && GetTickCount() - start < 20000) { pump(200); EnumWindows(find_titled, (LPARAM)&search); }
    wsprintfA(text, "a double click on %ls opens a window titled %ls (after %lu ms)", name, window_title, GetTickCount() - start);
    check(search.found != NULL, text);
    if (search.found) { PostMessageW(search.found, WM_CLOSE, 0, 0); pump(1500); }
}


static BOOL keep_window;
static HWND kept_window;

typedef struct { HWND seen[256]; int count; HWND fresh; WCHAR title[160]; WCHAR class_name[80]; } WINDOW_SET;

static BOOL CALLBACK collect_window(HWND window, LPARAM parameter)
{
    WINDOW_SET *set = (WINDOW_SET *)parameter;
    if (IsWindowVisible(window) && set->count < 256) set->seen[set->count++] = window;
    return TRUE;
}

static BOOL CALLBACK find_fresh(HWND window, LPARAM parameter)
{
    WINDOW_SET *set = (WINDOW_SET *)parameter;
    int i;
    RECT rect;
    if (!IsWindowVisible(window)) return TRUE;
    for (i = 0; i < set->count; i++) if (set->seen[i] == window) return TRUE;
    GetWindowRect(window, &rect);
    if (rect.right - rect.left < 50 || rect.bottom - rect.top < 30) return TRUE;
    set->fresh = window;
    GetWindowTextW(window, set->title, 160);
    GetClassNameW(window, set->class_name, 80);
    return FALSE;
}

/* Double click an item and say what came of it: the first new window of any size, with its title, within the time given. */
static void double_click_any_window(HWND list, const WCHAR *prefix, DWORD milliseconds)
{
    int x, y;
    WCHAR name[MAX_PATH];
    WINDOW_SET set;
    DWORD start;
    char text[400];
    set.count = 0;
    set.fresh = NULL;
    set.title[0] = set.class_name[0] = 0;
    if (!item_by_name(list, prefix, &x, &y, name, MAX_PATH)) { wsprintfA(text, "no desktop item starts with %ls", prefix); check(FALSE, text); return; }
    EnumWindows(collect_window, (LPARAM)&set);
    move(x, y);
    mouse(MOUSEEVENTF_LEFTDOWN, x, y); pump(40); mouse(MOUSEEVENTF_LEFTUP, x, y); pump(60);
    mouse(MOUSEEVENTF_LEFTDOWN, x, y); pump(40); mouse(MOUSEEVENTF_LEFTUP, x, y);
    start = GetTickCount();
    while (!set.fresh && GetTickCount() - start < milliseconds) { pump(250); EnumWindows(find_fresh, (LPARAM)&set); }
    wsprintfA(text, "a double click on %ls opens a window (%ls / %ls, after %lu ms)", name, set.fresh ? set.title : L"none", set.fresh ? set.class_name : L"-",
              GetTickCount() - start);
    check(set.fresh != NULL, text);
    if (set.fresh && keep_window) kept_window = set.fresh;
    else if (set.fresh) {
        pump(2000);
        PostMessageW(set.fresh, WM_CLOSE, 0, 0);
        pump(4000);
    }
}

void mainCRTStartup(void)
{
    char command[MAX_PATH * 2];
    HWND progman, view, list;
    WCHAR desktop_folder[MAX_PATH], public_folder[MAX_PATH];
    const char *c;
    int soak = 0, round;
    BOOL launches = FALSE;
    WCHAR chosen[8][64];
    int chosen_count = 0;

    lstrcpynA(command, GetCommandLineA(), sizeof(command));
    for (c = command; *c; ++c) {
        if ((c == command || c[-1] == ' ') && starts_with(c, "--capture=")) {
            const char *end = c + 10;
            while (*end && *end != ' ') ++end;
            lstrcpynA(capture_base, c + 10, (int)(end - c - 10) + 1);
        }
        if ((c == command || c[-1] == ' ') && starts_with(c, "--launch")) {
            launches = TRUE;
            if (c[8] == '=') {
                const char *at = c + 9;
                while (*at && *at != ' ' && chosen_count < 8) {
                    int length = 0;
                    while (*at && *at != ' ' && *at != ',' && length < 63) chosen[chosen_count][length++] = (WCHAR)*at++;
                    chosen[chosen_count++][length] = 0;
                    if (*at == ',') at++;
                }
            }
        }
        if ((c == command || c[-1] == ' ') && starts_with(c, "--soak=")) {
            const char *digit = c + 7;
            while (*digit >= '0' && *digit <= '9') soak = soak * 10 + (*digit++ - '0');
        }
    }
    CoInitialize(NULL);
    SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, desktop_folder);
    SHGetFolderPathW(NULL, CSIDL_COMMON_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, public_folder);

    progman = find_progman();
    view = progman ? FindWindowExW(progman, NULL, L"SHELLDLL_DefView", NULL) : NULL;
    list = view ? FindWindowExW(view, NULL, L"SysListView32", NULL) : NULL;
    check(list != NULL, "the desktop has its folder view");
    if (!list) goto done;
    pump(1500);
    {
        char text[100];
        wsprintfA(text, "desktop-items note: %d items on the desktop\n", item_count(list));
        line(text);
    }

    resolve_shortcuts(desktop_folder, "user");
    resolve_shortcuts(public_folder, "public");

    if (launches && !soak) {
        static const WCHAR *const names[] = { L"Demo Game - DirectX", L"Demo Game - OpenGL", L"Sample", L"The Native", L"Example Game", L"Fixture", L"Example Online", L"Client Launcher" };
        unsigned i;
        if (chosen_count) for (i = 0; i < (unsigned)chosen_count; i++) double_click_any_window(list, chosen[i], 60000);
        else for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) double_click_any_window(list, names[i], 40000);
    } else if (soak) {
        /* a launcher left open: its window as pictured and as measured, again and again */
        for (round = 1; round <= soak; round++) {
            char text[200];
            RECT rect = {0, 0, 0, 0};
            if (round == 1 && chosen_count) {
                keep_window = TRUE;
                double_click_any_window(list, chosen[0], 90000);
                pump(10000);
            }
            if (kept_window) GetWindowRect(kept_window, &rect);
            wsprintfA(text, "desktop-items note: soak %d at %lu ms: window %p alive=%d rect %ld %ld %ld %ld\n", round, GetTickCount(), (void *)kept_window,
                      kept_window ? IsWindow(kept_window) : 0, rect.left, rect.top, rect.right, rect.bottom);
            line(text);
            wsprintfA(text, "picture %d of the open launcher", round);
            check(capture_display((unsigned)round), text);
            pump(40000);
        }
    } else if (!soak) {
        right_click_menu(list, L"Appearance", "a shortcut in the user's Desktop (Appearance)", 1);
        right_click_menu(list, L"Store Launcher", "a shortcut in the Public Desktop (a launcher's)", 2);
        right_click_menu(list, L"Recycle Bin", "the Recycle Bin", 3);
        right_click_menu(list, L"Online Store", "an internet shortcut (a store page)", 4);
        check(capture_display(5), "picture 5: the desktop before any action");
        double_click_runs(list, L"Appearance", L"Appearance");
        check(capture_display(6), "picture 6: the desktop after a shortcut ran");
    }

done:
    {
        char text[100];
        wsprintfA(text, "PS5_PROTON_DESKTOP_ITEMS %s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
        line(text);
    }
    ExitProcess(failures != 0);
}
