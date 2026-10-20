/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A program opened from File Explorer starts in its own folder, as on Windows.
 *
 * A Direct3D 12 game opened from a File Explorer window stayed on a black window while loading: it started in Explorer's
 * folder, so vkd3d-proton found no "vkd3d-proton.cache" beside the game ("Failed to map read-only cache") and the game
 * built every pipeline again (2026-10-18, docs/PLAN.md). The shell view's open path gave ShellExecuteEx no directory.
 *
 * "shell-open-probe.exe --open": the probe moves itself to the Windows folder, hosts an ExplorerBrowser (what
 * explorer.exe's windows host) on its own folder, selects itself and opens it both ways File Explorer does: the view's
 * own open, as Enter or a double click does (NM_RETURN, ShellView_OpenSelectedItems), then the item menu's default
 * command, as Open in its right-click menu does (InvokeCommand, the menu's verb). Each copy so started, with no
 * arguments, writes its working folder and its own folder to %TEMP%\pw-shell-open.txt; the first compares them.
 * Verdict: "PS5_PROTON_SHELL_OPEN PASS|FAIL view=... menu=... folder=...". Without shell32 1184 the menu's copy
 * started in the Windows folder (upstream Wine as well); the view's already started in its own.
 *
 * "shell-open-probe.exe --open C:\path\program.exe": opens that program the same way, as a person opening it from
 * its folder in File Explorer would, and ends ("PS5_PROTON_SHELL_OPEN opened ..."): a game started by File Explorer's
 * own path, for a run to watch. */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

/* Its folder, without the trailing backslash except at a drive's root. */
static void module_folder(WCHAR *folder)
{
    WCHAR *slash;
    GetModuleFileNameW(NULL, folder, MAX_PATH);
    slash = wcsrchr(folder, '\\');
    if (slash) slash[slash - folder <= 2 ? 1 : 0] = 0;
}

static void result_path(WCHAR *path)
{
    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"pw-shell-open.txt");
}

/* The opened copy: its working folder, then its own folder, one per line. */
static int report(void)
{
    WCHAR cwd[MAX_PATH], folder[MAX_PATH], path[MAX_PATH], text[2 * MAX_PATH + 4];
    HANDLE file;
    DWORD written;
    GetCurrentDirectoryW(MAX_PATH, cwd);
    module_folder(folder);
    wsprintfW(text, L"%s\n%s\n", cwd, folder);
    result_path(path);
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return 2;
    WriteFile(file, text, lstrlenW(text) * sizeof(WCHAR), &written, NULL);
    CloseHandle(file);
    return 0;
}

/* Waits for the opened copy's report: its working folder in cwd, its own folder in seen; FALSE when none came. */
static BOOL opened_in(const WCHAR *path, WCHAR *cwd, WCHAR *seen);

static BOOL same_folder(const WCHAR *a, const WCHAR *b)
{
    WCHAR x[MAX_PATH], y[MAX_PATH];
    int n;
    lstrcpynW(x, a, MAX_PATH);
    lstrcpynW(y, b, MAX_PATH);
    for (WCHAR *s = x; s; s = s == x ? y : NULL)
        if ((n = lstrlenW(s)) > 3 && s[n - 1] == '\\') s[n - 1] = 0;
    return !lstrcmpiW(x, y);
}

static void pump(DWORD ms)
{
    DWORD start = GetTickCount();
    MSG msg;
    while (GetTickCount() - start < ms) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

static int open_item(const WCHAR *target)
{
    WCHAR folder[MAX_PATH], self[MAX_PATH], windows[MAX_PATH], path[MAX_PATH], *name;
    WCHAR view_cwd[MAX_PATH] = {0}, view_seen[MAX_PATH] = {0}, menu_cwd[MAX_PATH] = {0}, menu_seen[MAX_PATH] = {0};
    char line[4 * MAX_PATH];
    IExplorerBrowser *browser = NULL;
    IFolderView *view = NULL;
    IShellView *shell_view = NULL;
    PIDLIST_ABSOLUTE root = NULL;
    FOLDERSETTINGS settings = {FVM_DETAILS, 0};
    RECT rect = {0, 0, 640, 480};
    HWND parent, view_window = NULL, list;
    HRESULT hr;
    int count = 0, found = -1;

    if (target) {
        lstrcpynW(self, target, MAX_PATH);
        lstrcpynW(folder, target, MAX_PATH);
        if (!wcsrchr(folder, '\\')) return 2;
        *wcsrchr(folder, '\\') = 0;
        if (lstrlenW(folder) == 2) lstrcatW(folder, L"\\");
    } else {
        module_folder(folder);
        GetModuleFileNameW(NULL, self, MAX_PATH);
    }
    name = wcsrchr(self, '\\') + 1;
    GetWindowsDirectoryW(windows, MAX_PATH);
    SetCurrentDirectoryW(windows);          /* a folder the opened copy must not inherit */
    result_path(path);
    DeleteFileW(path);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    parent = CreateWindowExW(0, L"STATIC", L"shell-open", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL, NULL, NULL);
    hr = CoCreateInstance(&CLSID_ExplorerBrowser, NULL, CLSCTX_INPROC_SERVER, &IID_IExplorerBrowser, (void **)&browser);
    if (SUCCEEDED(hr)) hr = IExplorerBrowser_Initialize(browser, parent, &rect, &settings);
    if (SUCCEEDED(hr)) hr = SHParseDisplayName(folder, NULL, &root, 0, NULL);
    if (SUCCEEDED(hr)) hr = IExplorerBrowser_BrowseToIDList(browser, root, SBSP_ABSOLUTE);
    pump(500);
    if (SUCCEEDED(hr)) hr = IExplorerBrowser_GetCurrentView(browser, &IID_IFolderView, (void **)&view);
    if (SUCCEEDED(hr)) hr = IFolderView_ItemCount(view, SVGIO_ALLVIEW, &count);
    for (int i = 0; SUCCEEDED(hr) && i < count && found < 0; i++) {
        PITEMID_CHILD item = NULL;
        STRRET strret;
        WCHAR shown[MAX_PATH];
        IShellFolder *parent_folder = NULL;
        if (FAILED(IFolderView_Item(view, i, &item))) continue;
        if (SUCCEEDED(IFolderView_GetFolder(view, &IID_IShellFolder, (void **)&parent_folder)) &&
            SUCCEEDED(IShellFolder_GetDisplayNameOf(parent_folder, item, SHGDN_INFOLDER | SHGDN_FORPARSING, &strret)) &&
            SUCCEEDED(StrRetToBufW(&strret, item, shown, MAX_PATH)) && !lstrcmpiW(shown, name))
            found = i;
        if (parent_folder) IShellFolder_Release(parent_folder);
        CoTaskMemFree(item);
    }
    if (found >= 0) hr = IFolderView_SelectItem(view, found, SVSI_SELECT | SVSI_FOCUSED | SVSI_DESELECTOTHERS);
    else if (SUCCEEDED(hr)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = IFolderView_QueryInterface(view, &IID_IShellView, (void **)&shell_view);
    if (SUCCEEDED(hr)) hr = IShellView_GetWindow(shell_view, &view_window);
    list = SUCCEEDED(hr) ? FindWindowExW(view_window, NULL, WC_LISTVIEWW, NULL) : NULL;
    if (list) {
        /* What Enter in the view sends its parent; the view opens the selection (ShellView_OpenSelectedItems). */
        NMHDR notify = {list, (UINT_PTR)GetDlgCtrlID(list), NM_RETURN};
        SendMessageW(view_window, WM_NOTIFY, notify.idFrom, (LPARAM)&notify);
    }
    if (!target && list) {
        BOOL view_ok = opened_in(path, view_cwd, view_seen);
        IContextMenu *menu = NULL;
        DeleteFileW(path);
        /* What Open in the item's right-click menu does: the view's menu default command (ShellView_DoContextMenu). */
        if (SUCCEEDED(IShellView_GetItemObject(shell_view, SVGIO_SELECTION, &IID_IContextMenu, (void **)&menu))) {
            HMENU popup = CreatePopupMenu();
            if (SUCCEEDED(IContextMenu_QueryContextMenu(menu, popup, 0, FCIDM_SHVIEWFIRST, FCIDM_SHVIEWLAST, CMF_NORMAL))) {
                UINT command = GetMenuDefaultItem(popup, FALSE, GMDI_GOINTOPOPUPS);
                CMINVOKECOMMANDINFO invoke = {sizeof(invoke)};
                invoke.hwnd = GetParent(view_window);
                invoke.lpVerb = MAKEINTRESOURCEA(command);
                invoke.nShow = SW_SHOWNORMAL;
                if (command != (UINT)-1) IContextMenu_InvokeCommand(menu, &invoke);
            }
            DestroyMenu(popup);
            IContextMenu_Release(menu);
        }
        if (view_ok && opened_in(path, menu_cwd, menu_seen))
            wsprintfA(line, "PS5_PROTON_SHELL_OPEN %s view=%ls menu=%ls folder=%ls launcher=%ls\r\n",
                      same_folder(view_cwd, view_seen) && same_folder(menu_cwd, menu_seen) ? "PASS" : "FAIL",
                      view_cwd, menu_cwd, view_seen, windows);
        else
            wsprintfA(line, "PS5_PROTON_SHELL_OPEN FAIL no report view=%d menu=%ls\r\n", view_ok, menu_cwd);
        say(line);
        goto done;
    }
    if (target) {
        pump(2000);
        wsprintfA(line, "PS5_PROTON_SHELL_OPEN opened %ls from its folder: %s\r\n", target, list ? "sent" : "not found");
        say(line);
        goto done;
    }
    wsprintfA(line, "PS5_PROTON_SHELL_OPEN FAIL setup hr=0x%08lx found=%d count=%d list=%d\r\n",
              (unsigned long)hr, found, count, list != NULL);
    say(line);
done:
    if (shell_view) IShellView_Release(shell_view);
    if (view) IFolderView_Release(view);
    if (browser) {
        IExplorerBrowser_Destroy(browser);
        IExplorerBrowser_Release(browser);
    }
    CoTaskMemFree(root);
    DestroyWindow(parent);
    CoUninitialize();
    return strstr(line, " PASS ") || (target && list) ? 0 : 1;
}

static BOOL opened_in(const WCHAR *path, WCHAR *cwd, WCHAR *seen)
{
    WCHAR text[2 * MAX_PATH + 4] = {0}, *second;
    DWORD read = 0;
    HANDLE file;
    for (int waited = 0; waited < 150 && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES; waited++) pump(100);
    pump(200);
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ReadFile(file, text, sizeof(text) - sizeof(WCHAR), &read, NULL);
    CloseHandle(file);
    if (!(second = wcschr(text, '\n'))) return FALSE;
    *second++ = 0;
    if (wcschr(second, '\n')) *wcschr(second, '\n') = 0;
    lstrcpynW(cwd, text, MAX_PATH);
    lstrcpynW(seen, second, MAX_PATH);
    return TRUE;
}

void mainCRTStartup(void)
{
    const WCHAR *line = GetCommandLineW(), *open = wcsstr(line, L"--open");
    WCHAR target[MAX_PATH];
    if (!open) ExitProcess(report());
    open += 6;
    while (*open == ' ' || *open == '"') open++;
    lstrcpynW(target, open, MAX_PATH);
    for (int n = lstrlenW(target); n > 0 && (target[n - 1] == ' ' || target[n - 1] == '"'); n--) target[n - 1] = 0;
    ExitProcess(open_item(target[0] ? target : NULL));
}
