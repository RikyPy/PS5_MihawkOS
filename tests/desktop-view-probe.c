/* SPDX-License-Identifier: GPL-3.0-or-later
 * The Mihawk session's desktop as the shell's folder view (docs/WINDOWS11_PLAN.md, phase 1; patch 1100): run inside the session, it
 * drives the desktop with the same input a person has (SendInput) and checks what the shell did, then asks the owner for pictures of
 * what was displayed. It makes disposable files on the user's Desktop and removes them.
 *
 *   desktop-view-probe [--capture=PATH]
 *
 * Prints PS5_PROTON_DESKTOP_VIEW PASS/FAIL checks=N failures=M. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winreg.h>
#include <shlwapi.h>

#define FILES 6
static unsigned checks, failures;
static char capture_base[MAX_PATH];
static WCHAR desktop_dir[MAX_PATH], public_dir[MAX_PATH];

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
    wsprintfA(text, "desktop-view %s: %s error=%lu\n", okay ? "PASS" : "FAIL", name, GetLastError());
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
        wsprintfA(text, "desktop-view note: picture %u not seen: file %s size %lu, request %s\n", number, there ? "exists" : "absent",
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
        wsprintfA(text, "desktop-view note: picture %u %s after %lu ms more\n", number,
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
static void drag(int x0, int y0, int x1, int y1)
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

static void say_focus(const char *where)
{
    char text[200], foreground[64] = "none", focus[64] = "none";
    HWND window = GetForegroundWindow(), inner = GetFocus();
    GUITHREADINFO info;
    info.cbSize = sizeof(info);
    if (window) GetClassNameA(window, foreground, sizeof(foreground));
    info.hwndFocus = NULL;
    if (GetGUIThreadInfo(0, &info) && info.hwndFocus) GetClassNameA(info.hwndFocus, focus, sizeof(focus));
    wsprintfA(text, "desktop-view note: %s foreground=%s focus=%s (this thread %p)\n", where, foreground, focus, inner);
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

static BOOL wait_items(HWND list, int wanted, DWORD milliseconds)
{
    DWORD start = GetTickCount();
    while (GetTickCount() - start < milliseconds) {
        if (item_count(list) >= wanted) return TRUE;
        pump(100);
    }
    return item_count(list) >= wanted;
}

static BOOL wait_selected(HWND list, int wanted, DWORD milliseconds)
{
    DWORD start = GetTickCount();
    while (GetTickCount() - start < milliseconds) {
        if (selected_count(list) == wanted) return TRUE;
        pump(80);
    }
    return selected_count(list) == wanted;
}

static WCHAR *probe_path(WCHAR *buffer, unsigned number, BOOL renamed)
{
    wsprintfW(buffer, L"%ls\\PWProbe%s%u.txt", desktop_dir, renamed ? L"Renamed" : L"", number);
    return buffer;
}

static WCHAR *public_path(WCHAR *buffer, unsigned number, BOOL renamed)
{
    wsprintfW(buffer, L"%ls\\PWPublic%s%u.txt", public_dir, renamed ? L"Renamed" : L"", number);
    return buffer;
}

static void make_one(const WCHAR *path)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, "probe", 5, &written, NULL);
        CloseHandle(file);
    }
}

static void make_files(void)
{
    unsigned i;
    WCHAR path[MAX_PATH];
    for (i = 1; i <= 2; ++i) make_one(public_path(path, i, FALSE));
    for (i = 1; i <= FILES; ++i) {
        HANDLE file = CreateFileW(probe_path(path, i, FALSE), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(file, "probe", 5, &written, NULL);
            CloseHandle(file);
        }
    }
}

static void remove_files(void)
{
    unsigned i;
    WCHAR path[MAX_PATH];
    for (i = 1; i <= 2; ++i) {
        DeleteFileW(public_path(path, i, FALSE));
        DeleteFileW(public_path(path, i, TRUE));
    }
    for (i = 1; i <= FILES; ++i) {
        DeleteFileW(probe_path(path, i, FALSE));
        DeleteFileW(probe_path(path, i, TRUE));
    }
}

/* The icon positions the shell keeps (HKCU\\Software\\Mihawk\\Shell\\Desktop, IconPositions, patch 1102): a version, then entries of x, y,
 * name length and name; x and y are pixels in version 1, whole cells of the prototype's grid in 2, 1/256 of a cell in 3. */
static BYTE positions_blob[16384];

static int same_bytes(const void *a, const void *b, SIZE_T n)
{
    const BYTE *x = a, *y = b;
    while (n--) { if (*x != *y) return *x - *y; ++x; ++y; }
    return 0;
}


static DWORD read_positions(void)
{
    DWORD size = sizeof(positions_blob), type = 0;
    HKEY key;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Mihawk\\Shell\\Desktop", 0, KEY_READ, &key);
    if (status) return 0;
    status = RegQueryValueExW(key, L"IconPositions", NULL, &type, positions_blob, &size);
    RegCloseKey(key);
    return status || type != REG_BINARY ? 0 : size;
}

static void forget_positions(void)
{
    HKEY key;
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Mihawk\\Shell\\Desktop", 0, KEY_SET_VALUE, &key)) {
        RegDeleteValueW(key, L"IconPositions");
        RegCloseKey(key);
    }
}

/* Entries in the saved blob, the smallest x among them in pixels (a cell's left at medium icons: 75 px cells from 2), and whether one
 * is named name. */
static int parse_positions(DWORD size, int *smallest_x, const WCHAR *name)
{
    DWORD *data = (DWORD *)positions_blob + 1, *end = (DWORD *)(positions_blob + size), version = size >= 8 ? *(DWORD *)positions_blob : 0;
    int entries = 0;
    *smallest_x = 1 << 30;
    if (version < 1 || version > 3) return -1;
    while (data + 3 <= end && data[2] < MAX_PATH) {
        WCHAR *saved = (WCHAR *)(data + 3);
        int x = version == 1 ? (int)data[0] : version == 2 ? 2 + 75 * (int)data[0] : 2 + MulDiv((int)data[0], 75, 256);
        if (x < *smallest_x) *smallest_x = x;
        if (name && (int)data[2] == lstrlenW(name) && !same_bytes(saved, name, data[2] * sizeof(WCHAR))) entries += 1000;
        entries++;
        data += 3 + (data[2] * sizeof(WCHAR) + 3) / sizeof(DWORD);
    }
    return entries;
}

/* What the shell says a shortcut's icon is: the icon location of the Desktop item whose name starts with prefix (the same question the
 * folder view asks to draw it). 0 when there is no such item. */
static BOOL shortcut_icon_location(const WCHAR *prefix, WCHAR *file, DWORD size, int *index)
{
    IShellFolder *desktop = NULL;
    IEnumIDList *items = NULL;
    LPITEMIDLIST pidl;
    BOOL found = FALSE;

    file[0] = 0;
    *index = 0;
    CoInitialize(NULL);
    if (FAILED(SHGetDesktopFolder(&desktop))) return FALSE;
    if (SUCCEEDED(IShellFolder_EnumObjects(desktop, NULL, SHCONTF_NONFOLDERS, &items))) {
        while (!found && IEnumIDList_Next(items, 1, &pidl, NULL) == S_OK) {
            STRRET name;
            WCHAR text[MAX_PATH];
            if (SUCCEEDED(IShellFolder_GetDisplayNameOf(desktop, pidl, SHGDN_INFOLDER, &name)) &&
                SUCCEEDED(StrRetToBufW(&name, pidl, text, MAX_PATH)) && !StrCmpNIW(text, prefix, lstrlenW(prefix))) {
                IExtractIconW *icon = NULL;
                UINT flags = 0;
                if (SUCCEEDED(IShellFolder_GetUIObjectOf(desktop, NULL, 1, (LPCITEMIDLIST *)&pidl, &IID_IExtractIconW, NULL, (void **)&icon))) {
                    found = SUCCEEDED(IExtractIconW_GetIconLocation(icon, GIL_FORSHELL, file, size, index, &flags));
                    IExtractIconW_Release(icon);
                }
            }
            CoTaskMemFree(pidl);
        }
        IEnumIDList_Release(items);
    }
    IShellFolder_Release(desktop);
    return found;
}

/* What the shell link object itself says of one shortcut file, read as the shell reads it: the next run on a console where
 * the shell's icon location is the generic one (base PS5 13.40, 2026-10-20) shows which step lost it. */
static void shortcut_load_notes(int folder, const WCHAR *name)
{
    WCHAR path[MAX_PATH], target[MAX_PATH], icon[MAX_PATH];
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    int index = 0;
    char text[600];
    HRESULT created, loaded = E_FAIL, got_icon = E_FAIL, got_path = E_FAIL;
    DWORD attributes, size = 0;
    HANDLE handle;

    path[0] = target[0] = icon[0] = 0;
    if (FAILED(SHGetFolderPathW(NULL, folder, NULL, 0, path))) return;
    PathAppendW(path, name);
    attributes = GetFileAttributesW(path);
    handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (handle != INVALID_HANDLE_VALUE) { size = GetFileSize(handle, NULL); CloseHandle(handle); }
    created = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
    if (SUCCEEDED(created) && SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file))) {
        loaded = IPersistFile_Load(file, path, STGM_READ);
        if (SUCCEEDED(loaded)) {
            got_icon = IShellLinkW_GetIconLocation(link, icon, MAX_PATH, &index);
            got_path = IShellLinkW_GetPath(link, target, MAX_PATH, NULL, 0);
        }
        IPersistFile_Release(file);
    }
    {
        /* The registry the shell consults for a .lnk file's icon: its class, the class's own default icon and handler. */
        static const WCHAR *const keys[] = { L".lnk", L"lnkfile", L"lnkfile\\DefaultIcon", L"lnkfile\\shellex\\IconHandler" };
        for (int i = 0; i < 4; ++i) {
            WCHAR value[260];
            DWORD bytes = sizeof(value), type = 0;
            HKEY key;
            LONG opened, status = -1;
            value[0] = 0;
            opened = RegOpenKeyExW(HKEY_CLASSES_ROOT, keys[i], 0, KEY_READ, &key);
            if (!opened) {
                status = RegQueryValueExW(key, NULL, NULL, &type, (BYTE *)value, &bytes);
                RegCloseKey(key);
            }
            wsprintfA(text, "desktop-view note: HKCR\\%ls open=%ld default status=%ld type=%lu '%ls'\n", keys[i], (long)opened, (long)status,
                      (unsigned long)type, value);
            line(text);
        }
    }
    wsprintfA(text, "desktop-view note: link %ls attributes=%08lx size=%lu create=%08lx load=%08lx icon=%08lx '%ls' index %d path=%08lx '%ls'\n",
              name, attributes, size, (unsigned long)created, (unsigned long)loaded, (unsigned long)got_icon, icon, index,
              (unsigned long)got_path, target);
    line(text);
    if (link) IShellLinkW_Release(link);
}

/* Whether the icon a shortcut names can be taken out of its file, the step after the location (what the view does to draw it). */
static void icon_extraction_notes(const WCHAR *file, int index)
{
    HICON big_icon = NULL, little_icon = NULL;
    UINT got;
    DWORD attributes = GetFileAttributesW(file);
    char text[512];

    got = ExtractIconExW(file, index, &big_icon, &little_icon, 1);
    wsprintfA(text, "desktop-view note: icon file %ls attributes=%08lx ExtractIconExW(index %d) returned %u large=%d small=%d error=%lu\n",
              file, attributes, index, got, big_icon != NULL, little_icon != NULL, GetLastError());
    line(text);
    if (big_icon) DestroyIcon(big_icon);
    if (little_icon) DestroyIcon(little_icon);
    big_icon = little_icon = NULL;
    {
        static const int sizes[] = {16, 32, 48, 256};
        unsigned i;
        for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            big_icon = NULL;
            got = PrivateExtractIconsW(file, index, sizes[i], sizes[i], &big_icon, NULL, 1, 0);
            wsprintfA(text, "desktop-view note: PrivateExtractIconsW(%dx%d) returned %u icon=%d error=%lu\n", sizes[i], sizes[i], got,
                      big_icon != NULL, GetLastError());
            line(text);
            if (big_icon) DestroyIcon(big_icon);
        }
    }
}

/* What the system image list gives for a Desktop shortcut: its index, and where the drawn 32 x 32 icon has pixels. */
static BOOL shell_icon_notes(int folder, const WCHAR *name)
{
    WCHAR path[MAX_PATH];
    SHFILEINFOW info;
    char text[400];
    HDC dc;
    BITMAPINFO bmi;
    DWORD *pixels = NULL;
    HBITMAP bitmap;
    int x, y, left = 99, top = 99, right = -1, bottom = -1, covered = 0;
    int result;

    if (FAILED(SHGetFolderPathW(NULL, folder, NULL, 0, path))) return FALSE;
    PathAppendW(path, name);
    {
        volatile char *bytes = (volatile char *)&info;
        for (x = 0; x < (int)sizeof(info); x++) bytes[x] = 0;
    }
    result = SHGetFileInfoW(path, 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON | SHGFI_SYSICONINDEX);
    wsprintfA(text, "desktop-view note: SHGetFileInfo %ls returned %d system index %d icon=%d\n", name, result, info.iIcon, info.hIcon != NULL);
    line(text);
    if (!info.hIcon) return FALSE;
    {
        volatile char *bytes = (volatile char *)&bmi;
        for (x = 0; x < (int)sizeof(bmi); x++) bytes[x] = 0;
    }
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = 32;
    bmi.bmiHeader.biHeight = -32;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    dc = CreateCompatibleDC(NULL);
    bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
    SelectObject(dc, bitmap);
    for (x = 0; x < 32 * 32; x++) pixels[x] = 0x00ff00ff;
    DrawIconEx(dc, 0, 0, info.hIcon, 32, 32, 0, NULL, DI_NORMAL);
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++)
            if ((pixels[y * 32 + x] & 0x00ffffff) != 0x00ff00ff) {
                covered++;
                if (x < left) left = x;
                if (x > right) right = x;
                if (y < top) top = y;
                if (y > bottom) bottom = y;
            }
    wsprintfA(text, "desktop-view note: that icon drawn at 32 x 32 covers %d pixels in x %d..%d, y %d..%d\n", covered, left, right, top, bottom);
    line(text);
    DeleteObject(bitmap);
    DeleteDC(dc);
    DestroyIcon(info.hIcon);
    /* the arrow alone fills the bottom-left 16 x 16 corner: a picture reaches beyond it */
    return top < 16 && covered > 300;
}

/* The probe's own Public Desktop shortcut, to notepad.exe, whose icon is a picture. The check used a launcher's shortcut and failed in
 * any prefix without that launcher (v2-backend-gate-2026-10-15, a console just set up). */
#define PUBLIC_LINK_NAME L"Probe Notepad"
static WCHAR *public_link_path(WCHAR *path)
{
    lstrcpyW(path, public_dir);
    PathAppendW(path, PUBLIC_LINK_NAME L".lnk");
    return path;
}

static BOOL make_public_link(void)
{
    WCHAR path[MAX_PATH], target[MAX_PATH];
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    BOOL made = FALSE;

    CoInitialize(NULL);
    GetSystemDirectoryW(target, MAX_PATH);
    PathAppendW(target, L"notepad.exe");
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) {
        /* the icon named as an installer names it (a launcher's shortcut did): the location is the program's, not shell32's */
        if (SUCCEEDED(IShellLinkW_SetPath(link, target)) && SUCCEEDED(IShellLinkW_SetIconLocation(link, target, 0)) &&
            SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file))) {
            made = SUCCEEDED(IPersistFile_Save(file, public_link_path(path), TRUE));
            IPersistFile_Release(file);
        }
        IShellLinkW_Release(link);
    }
    return made;
}

static BOOL starts_with(const char *text, const char *prefix)
{
    while (*prefix) if (*text++ != *prefix++) return FALSE;
    return TRUE;
}

static BOOL popup_menu_present(void)
{
    return FindWindowW(L"#32768", NULL) != NULL;
}

void mainCRTStartup(void)
{
    char command[MAX_PATH * 2];
    HWND progman, view, list;
    int baseline, total;
    WCHAR path[MAX_PATH], renamed[MAX_PATH];
    char text[160];
    const char *c;

    lstrcpynA(command, GetCommandLineA(), sizeof(command));
    for (c = command; *c; ++c)
        if ((c == command || c[-1] == ' ') && starts_with(c, "--capture=")) {
            const char *end = c + 10;
            while (*end && *end != ' ') ++end;
            lstrcpynA(capture_base, c + 10, (int)(end - c - 10) + 1);
            break;
        }
    SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, desktop_dir);
    SHGetFolderPathW(NULL, CSIDL_COMMON_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, public_dir);

    forget_positions();

    /* 1. the desktop is the shell's folder view inside Progman */
    progman = find_progman();
    check(progman != NULL, "Progman exists");
    view = progman ? FindWindowExW(progman, NULL, L"SHELLDLL_DefView", NULL) : NULL;
    check(view != NULL, "Progman holds a SHELLDLL_DefView");
    list = view ? FindWindowExW(view, NULL, L"SysListView32", NULL) : NULL;
    check(list != NULL, "the view holds a SysListView32");
    if (!list) goto done;
    check(IsWindowVisible(list), "the list is visible");
    {
        RECT area, rect;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0);
        GetWindowRect(list, &rect);
        check(rect.left == area.left && rect.top == area.top && rect.right == area.right && rect.bottom == area.bottom,
              "the list covers the work area, not the taskbar");
    }

    /* 1b. a shortcut's icon is its target's, not the generic arrow: asked of the shell as the folder view asks */
    {
        WCHAR file[MAX_PATH];
        int index = 0;
        char text[400];
        BOOL found = shortcut_icon_location(L"Appearance", file, MAX_PATH, &index);
        wsprintfA(text, "desktop-view note: Appearance icon location %ls index %d\n", file, index);
        line(text);
        check(found && file[0] && !StrStrIW(file, L"shell32"), "a shortcut on the Desktop has its own icon location");
        shortcut_load_notes(CSIDL_DESKTOPDIRECTORY, L"Appearance.lnk");
        icon_extraction_notes(file, index);
        check(shell_icon_notes(CSIDL_DESKTOPDIRECTORY, L"Appearance.lnk"), "a Desktop shortcut's icon shows its picture and not the arrow alone");
        {
            const int before = item_count(list);
            const DWORD start = GetTickCount();
            WCHAR link[MAX_PATH];

            check(make_public_link(), "the probe's shortcut was made on the Public Desktop");
            check(shell_icon_notes(CSIDL_COMMON_DESKTOPDIRECTORY, PUBLIC_LINK_NAME L".lnk"),
                  "a Public Desktop shortcut's icon shows its picture and not the arrow alone");
            found = shortcut_icon_location(PUBLIC_LINK_NAME, file, MAX_PATH, &index);
            wsprintfA(text, "desktop-view note: Public Desktop shortcut icon location %ls index %d\n", file, index);
            line(text);
            check(found && file[0] && !StrStrIW(file, L"shell32"), "a shortcut on the Public Desktop has its own icon location");
            icon_extraction_notes(file, index);
            /* gone again before the counts below, which compare exact totals */
            DeleteFileW(public_link_path(link));
            while (GetTickCount() - start < 10000 && item_count(list) > before) pump(100);
        }
    }

    /* 2. a file made by a program that sends no notification appears (the watcher) */
    baseline = item_count(list);
    make_files();
    check(wait_items(list, baseline + FILES + 2, 15000), "six files on the Desktop and two on the Public Desktop appear without a notification");
    total = item_count(list);
    check(capture_display(1), "picture 1 of the displayed desktop was saved");

    /* 3. rubber-band selection: a drag from empty ground selects what it crosses */
    click(1880, 1000, FALSE);
    check(wait_selected(list, 0, 2000), "a click on empty ground selects nothing");
    drag(1880, 1010, 4, 4);
    check(wait_selected(list, total, 3000), "a rubber band over the whole screen selects every item");
    check(capture_display(2), "picture 2 of the displayed desktop was saved");
    click(1880, 1000, FALSE);
    check(wait_selected(list, 0, 2000), "a click on empty ground clears the selection");

    /* 3b. dragging an icon of a selection moves the selection, and the places are remembered */
    drag(1880, 1010, 4, 4);
    check(wait_selected(list, total, 3000), "everything is selected again");
    /* the second item of the first column by its plate's top edge, 400 right and 100 down: the prototype's grid (patch 1102)
     * puts its plate at 2,93 (73 x 73, the 48 px icon at 15,96), and the whole plate is the item (patch 1105; before, the list
     * counted only the icon and the label, so a press here started a rubber band: gate-2026-10-17-shell-batch-01) */
    drag(37, 95, 437, 195);
    pump(600);
    {
        DWORD size = read_positions();
        int smallest = 0, entries = size ? parse_positions(size, &smallest, L"PWProbe1.txt") : -1;
        check(entries >= total, "the moved icons' places were saved");
        check(entries >= 1000, "the saved places include a probe file by name");
        check(smallest >= 300, "every selected icon moved right by the drag (smallest saved x >= 300)");
    }
    check(capture_display(3), "picture 3 of the displayed desktop was saved");
    click(1880, 1000, FALSE);
    check(wait_selected(list, 0, 2000), "a click on empty ground clears the selection after the move");

    /* 4. the desktop menu opens on a right click, and closes on Escape */
    click(1880, 1000, TRUE);
    pump(400);
    check(popup_menu_present(), "a right click on empty ground opens a menu");
    check(capture_display(4), "picture 4 of the displayed desktop was saved");
    tap(VK_ESCAPE);
    pump(300);
    check(!popup_menu_present(), "Escape closes the menu");

    /* 5. type-ahead selects an item, F2 renames it, the file is renamed */
    click(1880, 1000, FALSE);
    say_focus("before typing");
    tap('P'); tap('W'); tap('P'); tap('R'); tap('O'); tap('B'); tap('E'); tap('3');
    check(wait_selected(list, 1, 3000), "typing a name selects that item");
    tap(VK_F2);
    pump(600);
    {
        HWND edit = FindWindowExW(list, NULL, L"Edit", NULL);
        check(edit != NULL, "F2 opens an edit box on the item");
        if (edit) {
            SendMessageW(edit, EM_SETSEL, 0, -1);
            SendMessageW(edit, WM_SETTEXT, 0, (LPARAM)L"PWProbeRenamed3.txt");
            tap(VK_RETURN);
            pump(800);
        }
    }
    check(GetFileAttributesW(probe_path(renamed, 3, TRUE)) != INVALID_FILE_ATTRIBUTES &&
          GetFileAttributesW(probe_path(path, 3, FALSE)) == INVALID_FILE_ATTRIBUTES, "the file was renamed on disk");

    /* 5b. an item that lives in the Public Desktop is found, renamed and opened as any other */
    click(1880, 1000, FALSE);
    tap('P'); tap('W'); tap('P'); tap('U'); tap('B'); tap('L'); tap('I'); tap('C'); tap('1');
    check(wait_selected(list, 1, 3000), "typing a Public Desktop item's name selects it");
    tap(VK_F2);
    pump(600);
    {
        HWND edit = FindWindowExW(list, NULL, L"Edit", NULL);
        if (edit) {
            SendMessageW(edit, EM_SETSEL, 0, -1);
            SendMessageW(edit, WM_SETTEXT, 0, (LPARAM)L"PWPublicRenamed1.txt");
            tap(VK_RETURN);
            pump(800);
        }
    }
    check(GetFileAttributesW(public_path(renamed, 1, TRUE)) != INVALID_FILE_ATTRIBUTES &&
          GetFileAttributesW(public_path(path, 1, FALSE)) == INVALID_FILE_ATTRIBUTES, "the Public Desktop file was renamed on disk");

    /* 6. a file deleted outside the view leaves it */
    DeleteFileW(probe_path(path, 4, FALSE));
    {
        DWORD start = GetTickCount();
        while (GetTickCount() - start < 10000 && item_count(list) > total - 1) pump(100);
    }
    check(item_count(list) == total - 1, "a file removed on disk leaves the desktop");
    check(capture_display(5), "picture 5 of the displayed desktop was saved");

done:
    remove_files();
    DeleteFileW(public_link_path(path));
    wsprintfA(text, "PS5_PROTON_DESKTOP_VIEW %s: checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
    line(text);
    /* end the session through its normal path, as tests/desktop-session-probe.c does */
    if (IsWindow(GetShellWindow())) PostMessageA(GetShellWindow(), WM_CLOSE, 0, 0);
    ExitProcess(failures != 0);
}
