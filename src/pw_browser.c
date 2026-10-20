/* SPDX-License-Identifier: GPL-3.0-or-later
 * MihawkBrowser (docs/MIHAWK_BROWSER.md): Mihawk OS's web browser, a Win32 window around one CEF 150 browser.
 *
 * One executable is every process: the browser (this window, CEF, the network service and the GPU as threads) and the
 * one jitless renderer CEF starts from it with --type=renderer. The chrome is native Win32 (§4), so a hung page cannot
 * take it down and it costs no renderer: until the first navigation there is no CEF browser at all, only the start
 * view. Decisions that need no Windows or CEF live in pw_browser_policy.c and are host-tested.
 *
 * CEF's C API: every struct argument a callback receives carries a reference the callee releases (release()); the
 * handler structs here are static and live as long as the process, so their reference counts are no-ops.
 *
 * Evidence (§3.4) goes to stderr as "browser: ..." lines that carry no query, path, user or address (rule 5):
 *   browser: process type=<browser|renderer|gpu-process|utility> pid=N
 *   browser: window ms=N | created ms=N
 *   browser: load scheme= host= path= status=N ms=N | load-error code=N ... | refused ... | certificate-error code=N ...
 *   browser: download start|done|canceled|interrupted scheme= host= path= bytes=N ms=N [sha256=hex]
 *   browser: open child=N | handoff child=N window_ms=N exiting
 *   browser: step i/n <verb> ...                       (MIHAWK_BROWSER_SCRIPT, below)
 *
 * MIHAWK_BROWSER_SCRIPT="load URL; download URL; open; wait MS; exit" drives the same paths the buttons
 * do, one step after the last one's event (load end, download end, the installer's first window), for runs that end in
 * a verdict; "begin URL" starts a download without waiting for it, "downloads" opens the flyout and "snapshot NAME"
 * writes the chrome as it paints it (snapshot()). */
#define CEF_API_VERSION 15000
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <uxtheme.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "include/cef_api_hash.h"
#include "include/internal/cef_string_list.h"
#include "include/capi/cef_app_capi.h"
#include "include/capi/cef_browser_capi.h"
#include "include/capi/cef_browser_process_handler_capi.h"
#include "include/capi/cef_client_capi.h"
#include "include/capi/cef_command_line_capi.h"
#include "include/capi/cef_download_handler_capi.h"
#include "include/capi/cef_download_item_capi.h"
#include "include/capi/cef_frame_capi.h"
#include "include/capi/cef_life_span_handler_capi.h"
#include "include/capi/cef_load_handler_capi.h"
#include "include/capi/cef_display_handler_capi.h"
#include "include/capi/cef_request_capi.h"
#include "include/capi/cef_request_handler_capi.h"
#include "pw_browser_chrome.h"
#include "pw_browser_policy.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
int _fltused; /* floating point without the C runtime (pw_browser_chrome.c's plates) */
#define RELEASE(object) do { if (object) (object)->base.release(&(object)->base); } while (0)
/* Windows 11's values, newer than the pinned build Wine's headers (dwmapi.h, uxtheme.h) */
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xffffffff
#endif
#ifndef BPPF_ERASE
#define BPPF_ERASE 0x0001
#endif

enum {
    TOOLBAR = 40, BUTTON = 32, GAP = 4, ADDRESS_X = 112, ADDRESS_PAD = 11, ADDRESS_LOCK = 34,
    ID_BACK = 1, ID_FORWARD, ID_RELOAD, ID_ADDRESS, ID_DOWNLOADS,
    ID_LAUNCHER = 100, TIMER_TICK = 1, MAX_DOWNLOADS = 32, URL_MAX = 8192,
    /* PS5_DWMSBT_FLYOUT (Wine patch 1160): the compositor draws the prototype's acrylic, flyout stroke and shadow */
    FLYOUT_BACKDROP = 7, FLYOUT_RIGHT = 4, FLYOUT_BELOW = 48,
    /* Mihawk icons (.deps/mihawk-icons/icons.h): ORIGINAL_APP_BROWSER, ORIGINAL_APP_GENERIC, LIGHT_LOCK_CLOSED */
    ICON_APP_BROWSER = 1014, ICON_APP_GENERIC = 1015, ICON_LOCK = 2036,
};
enum { ACTION_CANCEL, ACTION_OPEN, ACTION_FOLDER };

/* The direct installer URLs of the start view (decision 15): data, re-checked at each CEF re-pin. */
static const struct { const wchar_t *name; const char *url; } launchers[] = {
    {L"Epic Games", "https://launcher-public-service-prod06.ol.epicgames.com/launcher/api/installer/download/EpicGamesLauncherInstaller.msi"},
    {L"GOG Galaxy", "https://webinstallers.gog-statics.com/download/GOG_Galaxy_2.0.exe"},
    {L"EA app", "https://origin-a.akamaihd.net/EA-Desktop-Client-Download/installer-releases/EAappInstaller.exe"},
    {L"Ubisoft Connect", "https://static3.cdn.ubi.com/orbit/launcher_installer/UbisoftConnectInstaller.exe"},
};

/* FOLDERID_Downloads (knownfolders.h), defined here rather than through initguid.h and every GUID it would bring. */
static const GUID downloads_folder = {0x374de290, 0x123f, 0x4565, {0x91, 0x64, 0x39, 0xc4, 0x92, 0x5e, 0x46, 0x7b}};

typedef enum { DL_ACTIVE, DL_DONE, DL_CANCELED, DL_INTERRUPTED } DownloadState;
typedef struct Download {
    uint32_t id;
    DownloadState state;
    int cancel;
    long long received, total;
    ULONGLONG started;
    wchar_t path[MAX_PATH], name[MAX_PATH];
    char where[256];  /* pw_browser_log_url's words for the URL */
} Download;

/* A button of the Downloads flyout, where its last paint put it. */
typedef struct FlyoutHit { RECT box; uint32_t download; int action; } FlyoutHit;

static struct {
    HWND main, back, forward, reload, address, downloads, start, error, flyout;
    HFONT body, strong, caption, subtitle;   /* the prototype's type ramp: Body, Body Strong, Caption, Subtitle */
    HICON glyphs[6];                 /* back, forward, reload, stop, downloads, lock: Fluent glyphs from the Mihawk icons */
    HICON app_icon, app_icon_small, file_icon;
    const PwChromeTheme *theme;
    HBRUSH background, edit_fill, edit_focused_fill;
    HWND hot;                        /* the owner-drawn button under the pointer */
    cef_browser_t *browser;
    int closing, loading, page_shown, secure;   /* secure: a loaded https page, which the lock shows */
    wchar_t error_title[128], error_detail[192];
    FlyoutHit hits[MAX_DOWNLOADS * 2];
    int hit_count, flyout_hot, flyout_pressed, flyout_focus, flyout_scroll, label_widths[3];
    int edit_offset;                 /* the EDIT's top below its line box, so its baseline is Blink's */
    ULONGLONG flyout_closed;         /* when light dismiss closed it: the click on Downloads that did so reopens nothing */
    ULONGLONG close_after;            /* a close waiting for its downloads to cancel, until this time */
    ULONGLONG process_start, navigation_start;
    Download downloads_list[MAX_DOWNLOADS];
    int download_count;
    DWORD child;                     /* the installer Open started, until its first window */
    ULONGLONG child_started;
    char script[4096], *step;        /* MIHAWK_BROWSER_SCRIPT and the step after the current one */
    int step_index, step_count;
    enum { WAIT_NONE, WAIT_LOAD, WAIT_DOWNLOAD, WAIT_WINDOW, WAIT_TIME } wait;
    ULONGLONG wait_until;
} app_state;

static ULONGLONG now_ms(void) { return GetTickCount64() - app_state.process_start; }

static void say(const char *format, ...)
{
    char line[1200];
    va_list args;
    va_start(args, format);
    int n = _vsnprintf(line, sizeof line - 2, format, args);
    va_end(args);
    if (n < 0 || n > (int)sizeof line - 2) n = sizeof line - 2;
    memcpy(line + n, "\n", 2);
    char whole[1220] = "browser: ";
    lstrcatA(whole, line);
    DWORD written;
    /* A process started without a standard error handle (by explorer, by Start) says it to the debugger instead. */
    if (!WriteFile(GetStdHandle(STD_ERROR_HANDLE), whole, n + 10, &written, NULL)) OutputDebugStringA(whole);
}

/* CEF strings are UTF-16, as wchar_t is here; the policy speaks UTF-8. */
static void cef_set_utf8(cef_string_t *out, const char *text) { cef_string_utf8_to_utf16(text, strlen(text), out); }
static void cef_set_wide(cef_string_t *out, const wchar_t *text) { cef_string_wide_to_utf16(text, wcslen(text), out); }

static void utf8_of(const cef_string_t *text, char *out, size_t capacity)
{
    out[0] = 0;
    if (!text || !text->str) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)text->str, (int)text->length, out, (int)capacity - 1, NULL, NULL);
    out[n > 0 ? n : 0] = 0;
}

static void utf8_of_userfree(cef_string_userfree_t text, char *out, size_t capacity)
{
    utf8_of(text, out, capacity);
    if (text) cef_string_userfree_utf16_free(text);
}

static void where_of(const char *url, char *out, size_t capacity)
{
    if (pw_browser_log_url(url, out, capacity) != PW_OK) lstrcpynA(out, "scheme=unloggable", (int)capacity);
}

/* Static handler structs: CEF holds them for the process's life. */
static void CEF_CALLBACK no_add_ref(cef_base_ref_counted_t *self) { (void)self; }
static int CEF_CALLBACK no_release(cef_base_ref_counted_t *self) { (void)self; return 0; }
static int CEF_CALLBACK one_ref(cef_base_ref_counted_t *self) { (void)self; return 1; }
#define STATIC_BASE(type) {sizeof(type), no_add_ref, no_release, one_ref, one_ref}

static void layout(void);
static void navigate(const char *url);
static void place_flyout(void);
static void show_error(const wchar_t *title, const wchar_t *detail);
static void snapshot(const char *name);
static void script_next(void);
static void draw_icon_button(const DRAWITEMSTRUCT *item);

/* ---- downloads ---- */

static void downloads_changed(void);
static void show_downloads(int show);

static Download *download_by_id(uint32_t id)
{
    for (int i = 0; i < app_state.download_count; ++i)
        if (app_state.downloads_list[i].id == id) return &app_state.downloads_list[i];
    return NULL;
}

static int downloads_active(void)
{
    for (int i = 0; i < app_state.download_count; ++i) if (app_state.downloads_list[i].state == DL_ACTIVE) return 1;
    return 0;
}

typedef struct HashJob { wchar_t path[MAX_PATH]; char where[256]; long long bytes; ULONGLONG ms; unsigned char block[1 << 20]; } HashJob;

/* The finished file's SHA-256, off the UI thread: a launcher installer can be 260 MB. */
static DWORD WINAPI hash_thread(void *parameter)
{
    HashJob *job = parameter;
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[32];
    char hex[65] = "unread";
    HANDLE file = CreateFileW(job->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (file != INVALID_HANDLE_VALUE && !BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) &&
            !BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0)) {
        DWORD n;
        while (ReadFile(file, job->block, sizeof job->block, &n, NULL) && n) BCryptHashData(hash, job->block, n, 0);
        if (!BCryptFinishHash(hash, digest, 32, 0))
            for (int i = 0; i < 32; ++i) sprintf(hex + 2 * i, "%02x", digest[i]);
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    say("download done %s bytes=%lld ms=%llu sha256=%s", job->where, job->bytes, job->ms, hex);
    HeapFree(GetProcessHeap(), 0, job);
    return 0;
}

static int CEF_CALLBACK on_before_download(cef_download_handler_t *self, cef_browser_t *browser, cef_download_item_t *item,
                                           const cef_string_t *suggested, cef_before_download_callback_t *callback)
{
    (void)self;
    char suggested8[MAX_PATH * 3], name8[MAX_PATH * 3], candidate8[MAX_PATH * 3], url8[URL_MAX];
    utf8_of(suggested, suggested8, sizeof suggested8);
    pw_browser_download_name(suggested8, name8, sizeof name8);
    wchar_t *folder = NULL, path[MAX_PATH] = L"", name[MAX_PATH] = L"";
    HRESULT found = SHGetKnownFolderPath(&downloads_folder, KF_FLAG_CREATE, NULL, &folder);
    if (FAILED(found)) say("download refused: no Downloads folder hr=0x%08lx", (unsigned long)found);
    if (SUCCEEDED(found)) {
        for (unsigned n = 0; n < 1000; ++n) {
            pw_browser_download_candidate(name8, n, candidate8, sizeof candidate8);
            MultiByteToWideChar(CP_UTF8, 0, candidate8, -1, name, MAX_PATH);
            _snwprintf(path, MAX_PATH, L"%s\\%s", folder, name);
            path[MAX_PATH - 1] = 0;
            if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) break;
        }
        CoTaskMemFree(folder);
    }
    if (path[0] && app_state.download_count == MAX_DOWNLOADS) {
        /* The list is full: the oldest finished row makes room (an active one is never dropped). */
        for (int i = 0; i < MAX_DOWNLOADS; ++i) {
            if (app_state.downloads_list[i].state == DL_ACTIVE) continue;
            memmove(&app_state.downloads_list[i], &app_state.downloads_list[i + 1],
                    (MAX_DOWNLOADS - 1 - i) * sizeof app_state.downloads_list[0]);
            --app_state.download_count;
            break;
        }
    }
    if (path[0] && app_state.download_count < MAX_DOWNLOADS) {
        Download *d = &app_state.downloads_list[app_state.download_count++];
        memset(d, 0, sizeof *d);
        d->id = item->get_id(item);
        d->total = item->get_total_bytes(item);
        d->started = now_ms();
        lstrcpynW(d->path, path, MAX_PATH);
        lstrcpynW(d->name, name, MAX_PATH);
        utf8_of_userfree(item->get_url(item), url8, sizeof url8);
        where_of(url8, d->where, sizeof d->where);
        say("download start %s total=%lld", d->where, d->total);
        cef_string_t target = {0};
        cef_set_wide(&target, path);
        callback->cont(callback, &target, 0);
        cef_string_clear(&target);
        show_downloads(1);  /* as the prototype's launcher click opens it */
        downloads_changed();
    }
    RELEASE(callback);
    RELEASE(item);
    RELEASE(browser);
    return 1;  /* handled: when no path was found the download is dropped, never saved elsewhere */
}

static void CEF_CALLBACK on_download_updated(cef_download_handler_t *self, cef_browser_t *browser, cef_download_item_t *item,
                                             cef_download_item_callback_t *callback)
{
    (void)self;
    Download *d = download_by_id(item->get_id(item));
    if (d && d->state == DL_ACTIVE) {
        d->received = item->get_received_bytes(item);
        d->total = item->get_total_bytes(item);
        if (d->cancel && item->is_in_progress(item)) callback->cancel(callback);
        DownloadState state = item->is_complete(item) ? DL_DONE : item->is_canceled(item) ? DL_CANCELED
                            : item->is_interrupted(item) ? DL_INTERRUPTED : DL_ACTIVE;
        if (state != DL_ACTIVE) {
            d->state = state;
            ULONGLONG ms = now_ms() - d->started;
            if (state == DL_DONE) {
                HashJob *job = HeapAlloc(GetProcessHeap(), 0, sizeof *job);
                HANDLE thread = NULL;
                if (job) {
                    lstrcpynW(job->path, d->path, MAX_PATH);
                    lstrcpynA(job->where, d->where, sizeof job->where);
                    job->bytes = d->received;
                    job->ms = ms;
                    thread = CreateThread(NULL, 0, hash_thread, job, 0, NULL);
                }
                if (thread) CloseHandle(thread);
                else {
                    say("download done %s bytes=%lld ms=%llu sha256=unread", d->where, d->received, ms);
                    HeapFree(GetProcessHeap(), 0, job);
                }
            } else {
                say("download %s %s bytes=%lld ms=%llu reason=%d", state == DL_CANCELED ? "canceled" : "interrupted",
                    d->where, d->received, ms, (int)item->get_interrupt_reason(item));
            }
            if (app_state.wait == WAIT_DOWNLOAD) script_next();
        }
        downloads_changed();
    }
    RELEASE(callback);
    RELEASE(item);
    RELEASE(browser);
}

/* A navigation that turns into a download asks here first. Left unset, CEF's C wrapper answers false and the download
 * is dropped without another callback (browser-host-27). */
static int CEF_CALLBACK can_download(cef_download_handler_t *self, cef_browser_t *browser, const cef_string_t *url,
                                     const cef_string_t *method)
{
    (void)self; (void)url; (void)method;
    RELEASE(browser);
    return 1;
}

static cef_download_handler_t download_handler = {
    .base = STATIC_BASE(cef_download_handler_t),
    .can_download = can_download,
    .on_before_download = on_before_download,
    .on_download_updated = on_download_updated,
};

/* Open: the installer runs through the shell, as Start runs a program (wine/ps5/pw_shell_start.c). */
static void download_open(Download *d)
{
    if (!d || d->state != DL_DONE) return;
    SHELLEXECUTEINFOW info = {sizeof info};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.hwnd = app_state.main;
    info.lpFile = d->path;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) { say("open failed error=%lu", GetLastError()); return; }
    DWORD child = info.hProcess ? GetProcessId(info.hProcess) : 0;
    if (info.hProcess) CloseHandle(info.hProcess);
    char name8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, d->name, -1, name8, sizeof name8, NULL, NULL);
    say("open child=%lu installer=%d", child, pw_browser_is_installer(name8));
    if (child && pw_browser_is_installer(name8)) { app_state.child = child; app_state.child_started = now_ms(); }
}

/* The installer's first visible window, from the process Open started or one it started (a self-extractor, msiexec). */
static BOOL CALLBACK find_child_window(HWND window, LPARAM parameter)
{
    const DWORD *family = (const DWORD *)parameter;
    DWORD pid = 0;
    RECT rect;
    GetWindowThreadProcessId(window, &pid);
    if (!IsWindowVisible(window) || !GetWindowRect(window, &rect) || rect.right - rect.left < 64) return TRUE;
    for (int i = 1; i <= (int)family[0]; ++i) if (family[i] == pid) { SetLastError(1); return FALSE; }
    return TRUE;
}

static int child_has_window(DWORD child)
{
    DWORD family[65] = {1, child};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    for (int pass = 0; pass < 3 && snapshot != INVALID_HANDLE_VALUE; ++pass) {
        PROCESSENTRY32W entry = {sizeof entry};
        for (BOOL more = Process32FirstW(snapshot, &entry); more && family[0] < 64; more = Process32NextW(snapshot, &entry)) {
            int parent_known = 0, known = 0;
            for (int i = 1; i <= (int)family[0]; ++i) {
                parent_known |= family[i] == entry.th32ParentProcessID;
                known |= family[i] == entry.th32ProcessID;
            }
            if (parent_known && !known) family[++family[0]] = entry.th32ProcessID;
        }
    }
    if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
    SetLastError(0);
    EnumWindows(find_child_window, (LPARAM)family);
    return GetLastError() == 1;
}

/* ---- the browser's events ---- */

static void CEF_CALLBACK on_after_created(cef_life_span_handler_t *self, cef_browser_t *browser)
{
    (void)self;
    say("created ms=%llu", now_ms());
    RELEASE(browser);  /* app_state.browser holds create_browser_sync's reference */
}

static int CEF_CALLBACK do_close(cef_life_span_handler_t *self, cef_browser_t *browser)
{
    (void)self;
    /* The next WM_CLOSE closes the window (CEF's "Example 2"). CEF's own WM_CLOSE never reached the main window
     * under Wine (browser-host-18), so it is posted here too; a second one finds the window gone. */
    app_state.closing = 1;
    say("close asked ms=%llu", now_ms());
    PostMessageW(app_state.main, WM_CLOSE, 0, 0);
    RELEASE(browser);
    return 0;
}

static void CEF_CALLBACK on_before_close(cef_life_span_handler_t *self, cef_browser_t *browser)
{
    (void)self;
    say("close done ms=%llu", now_ms());
    RELEASE(browser);
    RELEASE(app_state.browser);
    app_state.browser = NULL;
    cef_quit_message_loop();
}

/* Popups load in the one view, and only from a user's click (§3.5). */
static int CEF_CALLBACK on_before_popup(cef_life_span_handler_t *self, cef_browser_t *browser, cef_frame_t *frame, int popup_id,
                                        const cef_string_t *target_url, const cef_string_t *target_frame_name,
                                        cef_window_open_disposition_t disposition, int user_gesture,
                                        const cef_popup_features_t *features, cef_window_info_t *window_info,
                                        cef_client_t **client, cef_browser_settings_t *settings,
                                        cef_dictionary_value_t **extra_info, int *no_javascript_access)
{
    (void)self; (void)popup_id; (void)target_frame_name; (void)disposition; (void)features; (void)window_info;
    (void)client; (void)settings; (void)extra_info; (void)no_javascript_access;
    char url[URL_MAX], where[256];
    utf8_of(target_url, url, sizeof url);
    where_of(url, where, sizeof where);
    say("popup %s gesture=%d", where, user_gesture);
    if (user_gesture) navigate(url);
    RELEASE(frame);
    RELEASE(browser);
    return 1;
}

static cef_life_span_handler_t life_span_handler = {
    .base = STATIC_BASE(cef_life_span_handler_t),
    .on_before_popup = on_before_popup,
    .on_after_created = on_after_created,
    .do_close = do_close,
    .on_before_close = on_before_close,
};

static void CEF_CALLBACK on_loading_state_change(cef_load_handler_t *self, cef_browser_t *browser, int loading, int back,
                                                 int forward)
{
    (void)self;
    app_state.loading = loading;
    EnableWindow(app_state.back, back);
    EnableWindow(app_state.forward, forward);
    InvalidateRect(app_state.reload, NULL, FALSE);  /* the glyph is reload or stop (WM_DRAWITEM) */
    RELEASE(browser);
}

static void CEF_CALLBACK on_load_start(cef_load_handler_t *self, cef_browser_t *browser, cef_frame_t *frame,
                                       cef_transition_type_t transition)
{
    (void)self; (void)transition;
    if (frame->is_main(frame)) {
        app_state.navigation_start = now_ms();
        ShowWindow(app_state.error, SW_HIDE);
        app_state.page_shown = 1;
        app_state.secure = 0;  /* the prototype shows no lock while a page loads */
        layout();
    }
    RELEASE(frame);
    RELEASE(browser);
}

static void CEF_CALLBACK on_load_end(cef_load_handler_t *self, cef_browser_t *browser, cef_frame_t *frame, int status)
{
    (void)self;
    if (frame->is_main(frame)) {
        char url[URL_MAX], where[256];
        utf8_of_userfree(frame->get_url(frame), url, sizeof url);
        where_of(url, where, sizeof where);
        say("load %s status=%d ms=%llu at=%llu", where, status, now_ms() - app_state.navigation_start, now_ms());
        app_state.secure = !strncmp(url, "https://", 8) && !IsWindowVisible(app_state.error);
        layout();
        if (app_state.wait == WAIT_LOAD) script_next();
    }
    RELEASE(frame);
    RELEASE(browser);
}

/* Load errors show in a native panel, not Chromium's error page, which would take a renderer of its own (§3.1). */
static void CEF_CALLBACK on_load_error(cef_load_handler_t *self, cef_browser_t *browser, cef_frame_t *frame,
                                       cef_errorcode_t code, const cef_string_t *text, const cef_string_t *failed_url)
{
    (void)self;
    if (frame->is_main(frame) && code != ERR_ABORTED) {
        char url[URL_MAX], where[256];
        utf8_of(failed_url, url, sizeof url);
        where_of(url, where, sizeof where);
        say("load-error code=%d %s ms=%llu", (int)code, where, now_ms() - app_state.navigation_start);
        /* "ERR_NAME_NOT_RESOLVED (-105)", as the prototype words it: CEF's text without its "net::" */
        const wchar_t *name = text && text->str ? (const wchar_t *)text->str : L"";
        int length = text ? (int)text->length : 0;
        if (length > 5 && !wcsncmp(name, L"net::", 5)) name += 5, length -= 5;
        wchar_t detail[192];
        _snwprintf(detail, ARRAY_SIZE(detail), L"%.*s (%d)", min(length, 150), name, (int)code);
        detail[ARRAY_SIZE(detail) - 1] = 0;
        show_error(L"This page can't be reached.", detail);
        if (app_state.wait == WAIT_LOAD) script_next();
    }
    RELEASE(frame);
    RELEASE(browser);
}

static cef_load_handler_t load_handler = {
    .base = STATIC_BASE(cef_load_handler_t),
    .on_loading_state_change = on_loading_state_change,
    .on_load_start = on_load_start,
    .on_load_end = on_load_end,
    .on_load_error = on_load_error,
};

static void CEF_CALLBACK on_address_change(cef_display_handler_t *self, cef_browser_t *browser, cef_frame_t *frame,
                                           const cef_string_t *url)
{
    (void)self;
    if (frame->is_main(frame) && GetFocus() != app_state.address) {
        wchar_t text[URL_MAX];
        lstrcpynW(text, url && url->str ? (const wchar_t *)url->str : L"", (int)(url ? min(url->length + 1, URL_MAX) : 1));
        SetWindowTextW(app_state.address, text);
    }
    RELEASE(frame);
    RELEASE(browser);
}

static void CEF_CALLBACK on_title_change(cef_display_handler_t *self, cef_browser_t *browser, const cef_string_t *title)
{
    (void)self;
    wchar_t text[512];
    _snwprintf(text, 512, L"%.*s - MihawkBrowser", title ? (int)min(title->length, 400) : 0,
               title && title->str ? (const wchar_t *)title->str : L"");
    SetWindowTextW(app_state.main, text);
    RELEASE(browser);
}

static cef_display_handler_t display_handler = {
    .base = STATIC_BASE(cef_display_handler_t),
    .on_address_change = on_address_change,
    .on_title_change = on_title_change,
};

/* HTTPS only (§1): http is upgraded, every other scheme refused, in every frame. */
static int CEF_CALLBACK on_before_browse(cef_request_handler_t *self, cef_browser_t *browser, cef_frame_t *frame,
                                         cef_request_t *request, int user_gesture, int is_redirect)
{
    (void)self; (void)user_gesture;
    char url[URL_MAX], upgraded[URL_MAX], where[256];
    utf8_of_userfree(request->get_url(request), url, sizeof url);
    PwBrowserNavigation verdict = pw_browser_navigation(url, upgraded, sizeof upgraded);
    if (verdict != PW_BROWSER_NAV_ALLOW) {
        where_of(url, where, sizeof where);
        say("%s %s redirect=%d main=%d", verdict == PW_BROWSER_NAV_UPGRADE ? "upgrade" : "refused", where, is_redirect,
            frame->is_main(frame));
        if (verdict == PW_BROWSER_NAV_UPGRADE) {
            cef_string_t target = {0};
            cef_set_utf8(&target, upgraded);
            frame->load_url(frame, &target);
            cef_string_clear(&target);
        }
    }
    RELEASE(request);
    RELEASE(frame);
    RELEASE(browser);
    return verdict != PW_BROWSER_NAV_ALLOW;
}

/* No certificate exception, ever: the request fails and on_load_error names the error. */
static int CEF_CALLBACK on_certificate_error(cef_request_handler_t *self, cef_browser_t *browser, cef_errorcode_t code,
                                             const cef_string_t *request_url, cef_sslinfo_t *ssl_info, cef_callback_t *callback)
{
    (void)self;
    char url[URL_MAX], where[256];
    utf8_of(request_url, url, sizeof url);
    where_of(url, where, sizeof where);
    say("certificate-error code=%d %s", (int)code, where);
    RELEASE(callback);
    RELEASE(ssl_info);
    RELEASE(browser);
    return 0;
}

static cef_request_handler_t request_handler = {
    .base = STATIC_BASE(cef_request_handler_t),
    .on_before_browse = on_before_browse,
    .on_certificate_error = on_certificate_error,
};

static cef_life_span_handler_t *CEF_CALLBACK get_life_span_handler(cef_client_t *self) { (void)self; return &life_span_handler; }
static cef_load_handler_t *CEF_CALLBACK get_load_handler(cef_client_t *self) { (void)self; return &load_handler; }
static cef_display_handler_t *CEF_CALLBACK get_display_handler(cef_client_t *self) { (void)self; return &display_handler; }
static cef_download_handler_t *CEF_CALLBACK get_download_handler(cef_client_t *self) { (void)self; return &download_handler; }
static cef_request_handler_t *CEF_CALLBACK get_request_handler(cef_client_t *self) { (void)self; return &request_handler; }

static cef_client_t client = {
    .base = STATIC_BASE(cef_client_t),
    .get_display_handler = get_display_handler,
    .get_download_handler = get_download_handler,
    .get_life_span_handler = get_life_span_handler,
    .get_load_handler = get_load_handler,
    .get_request_handler = get_request_handler,
};

/* ---- navigation and layout ---- */

static HWND browser_window(void)
{
    if (!app_state.browser) return NULL;
    cef_browser_host_t *host = app_state.browser->get_host(app_state.browser);
    HWND window = host->get_window_handle(host);
    RELEASE(host);
    return window;
}

static void navigate(const char *url)
{
    char where[256];
    where_of(url, where, sizeof where);
    say("navigate %s", where);
    cef_string_t target = {0};
    cef_set_utf8(&target, url);
    if (!app_state.browser) {
        /* The first navigation makes the browser, and with it the renderer (§3.1). */
        RECT client_rect;
        GetClientRect(app_state.main, &client_rect);
        cef_window_info_t info = {0};
        info.size = sizeof info;
        info.style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP;
        info.parent_window = app_state.main;
        info.bounds.y = TOOLBAR;
        info.bounds.width = client_rect.right;
        info.bounds.height = max(client_rect.bottom - TOOLBAR, 1);
        info.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
        cef_browser_settings_t settings = {0};
        settings.size = sizeof settings;
        app_state.browser = cef_browser_host_create_browser_sync(&info, &client, &target, &settings, NULL, NULL);
        if (!app_state.browser) say("create-browser failed");
        layout();
    } else {
        cef_frame_t *frame = app_state.browser->get_main_frame(app_state.browser);
        frame->load_url(frame, &target);
        RELEASE(frame);
    }
    cef_string_clear(&target);
}

static void navigate_address(void)
{
    wchar_t text[URL_MAX];
    char text8[URL_MAX * 3], url[URL_MAX];
    GetWindowTextW(app_state.address, text, URL_MAX);
    WideCharToMultiByte(CP_UTF8, 0, text, -1, text8, sizeof text8, NULL, NULL);
    int result = pw_browser_address(text8, url, sizeof url);
    if (result == PW_OK) { navigate(url); if (app_state.browser) SetFocus(browser_window()); }
    else if (result == PW_ERR_UNSUPPORTED) show_error(L"MihawkBrowser opens https:// addresses only.", L"");
}

/* The address box (a WinUI TextBox, §4.1): the frame is the main window's paint; the EDIT is its text line, 11 px in
 * from the left (34 with the lock), 6 px down, 20 px tall. */
static RECT address_box(void)
{
    RECT r;
    GetClientRect(app_state.main, &r);
    RECT box = {ADDRESS_X, GAP, max(r.right - BUTTON - 2 * GAP, ADDRESS_X + 32), GAP + BUTTON};
    return box;
}

static void layout(void)
{
    static int inside;
    if (inside) { say("layout re-entered"); return; }
    inside = 1;
    RECT r;
    GetClientRect(app_state.main, &r);
    int width = r.right, height = r.bottom, x = GAP, y = (TOOLBAR - BUTTON) / 2;
    MoveWindow(app_state.back, x, y, BUTTON, BUTTON, TRUE); x += BUTTON + GAP;
    MoveWindow(app_state.forward, x, y, BUTTON, BUTTON, TRUE); x += BUTTON + GAP;
    MoveWindow(app_state.reload, x, y, BUTTON, BUTTON, TRUE);
    RECT box = address_box();
    int text = box.left + (app_state.secure ? ADDRESS_LOCK : ADDRESS_PAD);
    MoveWindow(app_state.address, text, box.top + 6 + app_state.edit_offset, max(box.right - ADDRESS_PAD - text, 8), 20, TRUE);
    InvalidateRect(app_state.main, &box, FALSE);
    MoveWindow(app_state.downloads, width - BUTTON - GAP, y, BUTTON, BUTTON, TRUE);
    /* The Downloads flyout lies over the page (§4.1), so the page keeps the whole width. */
    int below = max(height - TOOLBAR, 1);
    HWND view = browser_window();
    if (view) {
        SetWindowPos(view, NULL, 0, TOOLBAR, width, below, SWP_NOZORDER);
        ShowWindow(view, app_state.page_shown ? SW_SHOW : SW_HIDE);
    }
    MoveWindow(app_state.start, 0, TOOLBAR, width, below, TRUE);
    ShowWindow(app_state.start, app_state.page_shown ? SW_HIDE : SW_SHOW);
    MoveWindow(app_state.error, 0, TOOLBAR, width, below, TRUE);
    place_flyout();
    inside = 0;
}

static void show_error(const wchar_t *title, const wchar_t *detail)
{
    lstrcpynW(app_state.error_title, title, ARRAY_SIZE(app_state.error_title));
    lstrcpynW(app_state.error_detail, detail, ARRAY_SIZE(app_state.error_detail));
    app_state.secure = 0;
    layout();
    InvalidateRect(app_state.error, NULL, TRUE);
    ShowWindow(app_state.error, SW_SHOW);
    SetWindowPos(app_state.error, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

/* ---- MIHAWK_BROWSER_SCRIPT ---- */

static void script_next(void)
{
    app_state.wait = WAIT_NONE;
    while (app_state.step && *app_state.step && app_state.wait == WAIT_NONE) {
        char *step = app_state.step, *end = strchr(step, ';');
        if (end) *end = 0;
        app_state.step = end ? end + 1 : NULL;
        while (*step == ' ') ++step;
        if (!*step) continue;
        char *argument = strchr(step, ' ');
        if (argument) *argument++ = 0; else argument = (char *)"";
        while (*argument == ' ') ++argument;
        say("step %d/%d %s at=%llu", ++app_state.step_index, app_state.step_count, step, now_ms());
        char url[URL_MAX];
        if (!strcmp(step, "load")) {
            /* What the address box would do with the same text. */
            if (pw_browser_address(argument, url, sizeof url) != PW_OK) { say("step refused"); continue; }
            app_state.wait = WAIT_LOAD;
            app_state.wait_until = now_ms() + 90000;
            navigate(url);
            if (app_state.browser) SetFocus(browser_window());
        }
        else if (!strcmp(step, "download")) {
            app_state.wait = WAIT_DOWNLOAD;
            app_state.wait_until = now_ms() + 900000;
            navigate(argument);  /* as a click on a download link does: a navigation that becomes a download */
        } else if (!strcmp(step, "begin")) navigate(argument);  /* a download that goes on while the next steps run */
        else if (!strcmp(step, "open")) {
            Download *last = NULL;
            for (int i = 0; i < app_state.download_count; ++i)
                if (app_state.downloads_list[i].state == DL_DONE) last = &app_state.downloads_list[i];
            download_open(last);
            if (app_state.child) { app_state.wait = WAIT_WINDOW; app_state.wait_until = now_ms() + 120000; }
        } else if (!strcmp(step, "wait")) { app_state.wait = WAIT_TIME; app_state.wait_until = now_ms() + (ULONGLONG)atoi(argument); }
        else if (!strcmp(step, "exit")) PostMessageW(app_state.main, WM_CLOSE, 0, 0);
        else if (!strcmp(step, "snapshot")) snapshot(argument);
        else if (!strcmp(step, "downloads")) show_downloads(1);
        else say("step unknown");
    }
}

static void on_tick(void)
{
    if (app_state.close_after && app_state.close_after != ~0ull &&
            (!downloads_active() || now_ms() >= app_state.close_after)) {
        app_state.close_after = ~0ull;  /* the next WM_CLOSE goes on */
        PostMessageW(app_state.main, WM_CLOSE, 0, 0);
    }
    if (app_state.child && child_has_window(app_state.child)) {
        ULONGLONG window_ms = now_ms() - app_state.child_started;
        DWORD child = app_state.child;
        app_state.child = 0;
        /* Hand-off (decision 4): the installer's first window is up and nothing is downloading, so the browser goes. */
        if (!downloads_active()) {
            say("handoff child=%lu window_ms=%llu exiting", child, window_ms);
            PostMessageW(app_state.main, WM_CLOSE, 0, 0);
            return;
        }
        say("handoff child=%lu window_ms=%llu staying downloads=active", child, window_ms);
        if (app_state.wait == WAIT_WINDOW) script_next();
    }
    if (app_state.wait != WAIT_NONE && now_ms() >= app_state.wait_until) {
        if (app_state.wait != WAIT_TIME) say("step timeout wait=%d", (int)app_state.wait);
        if (app_state.wait == WAIT_WINDOW) app_state.child = 0;
        script_next();
    }
}

/* ---- windows: drawing ---- */

/* Every colour is a token of pw_browser_chrome.c over what it lies on: the window's solid surface, or the flyout's
 * acrylic, which the compositor draws (glass: premultiplied pixels, transparent black where it shows). */
static const PwRgba no_colour = {0, 0, 0, 0};

static COLORREF colour(PwRgba c, PwRgba under)
{
    PwRgba out = pw_chrome_blend(c, under);
    return RGB(out.r, out.g, out.b);
}

/* w x h pixels from paint, put at x, y: opaque, or premultiplied over what is there. */
static void put_pixels(HDC dc, int x, int y, int w, int h, int opaque,
                       void (*paint)(uint32_t *bits, int w, int h, const void *how), const void *how)
{
    if (w <= 0 || h <= 0) return;
    BITMAPINFO info = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
    void *bits = NULL;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    HDC source = bitmap ? CreateCompatibleDC(dc) : NULL;
    if (source && bits) {
        paint(bits, w, h, how);
        HGDIOBJ old = SelectObject(source, bitmap);
        BLENDFUNCTION over = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (opaque) BitBlt(dc, x, y, w, h, source, 0, 0, SRCCOPY);
        else GdiAlphaBlend(dc, x, y, w, h, source, 0, 0, w, h, over);
        SelectObject(source, old);
    }
    if (source) DeleteDC(source);
    if (bitmap) DeleteObject(bitmap);
}

typedef struct Plate { float radius; PwRgba fill, stroke, bottom; int bottom_width; const PwRgba *under; int percent; } Plate;

static void paint_plate(uint32_t *bits, int w, int h, const void *how)
{
    const Plate *p = how;
    pw_chrome_plate(bits, w, h, p->radius, p->fill, p->stroke, p->bottom, p->bottom_width, p->under);
}

static void paint_progress(uint32_t *bits, int w, int h, const void *how)
{
    const Plate *p = how;
    (void)h;
    pw_chrome_progress(bits, w, p->percent, p->stroke, p->fill, p->under);
}

/* A WinUI plate in box: opaque over *under, or premultiplied over glass when under is NULL. */
static void draw_plate(HDC dc, RECT box, float radius, PwRgba fill, PwRgba stroke, PwRgba bottom, int bottom_width,
                       const PwRgba *under)
{
    Plate p = {radius, fill, stroke, bottom, bottom_width, under, 0};
    put_pixels(dc, box.left, box.top, box.right - box.left, box.bottom - box.top, under != NULL, paint_plate, &p);
}

static void draw_progress(HDC dc, int x, int y, int w, int percent, const PwRgba *under)
{
    Plate p = {0, app_state.theme->accent, app_state.theme->progress_track, no_colour, 0, under, percent};
    put_pixels(dc, x, y, w, 3, under != NULL, paint_progress, &p);
}

/* Text as the prototype's Chromium lays it out: each glyph at its design advance (GDI would round every advance to a
 * whole pixel, 8 px too wide over a 40-character line), the baseline where Blink puts it in the line box (half the
 * leading of the font's hhea ascent and descent above), drawn in grayscale and laid on as coverage: the same on the
 * window's surface and over the flyout's glass. Scripts that need shaping keep their characters' own advances. */
enum { TEXT_MAX = 512, DESIGN_SCALE = 64 };

static int text_layout_design(HFONT font, const wchar_t *text, int count, int *dx, int *design)
{
    LOGFONTW large;
    GetObjectW(font, sizeof large, &large);
    large.lfHeight *= DESIGN_SCALE;  /* hinting is lost in the size: the advances are the design's */
    HDC dc = CreateCompatibleDC(NULL);
    HFONT measure = CreateFontIndirectW(&large);
    HGDIOBJ old = SelectObject(dc, measure);
    int x = 0, total = 0;
    for (int i = 0; i < count; ++i) {
        INT width = 0;
        GetCharWidth32W(dc, text[i], text[i], &width);
        total += width;
        int next = (total + DESIGN_SCALE / 2) / DESIGN_SCALE;
        dx[i] = next - x;
        x = next;
    }
    SelectObject(dc, old);
    DeleteObject(measure);
    DeleteDC(dc);
    if (design) *design = total;  /* in 1/DESIGN_SCALE px */
    return x;
}

static int text_layout(HFONT font, const wchar_t *text, int count, int *dx)
{
    return text_layout_design(font, text, count, dx, NULL);
}

/* Blink's baseline in a line box line px tall: half the leading over the hhea ascent and descent; and GDI's ascent,
 * where an EDIT puts its baseline. */
static int line_baseline(HFONT font, int line, int *gdi_ascent)
{
    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(dc, font);
    OUTLINETEXTMETRICW metrics = {sizeof metrics};
    TEXTMETRICW plain;
    GetTextMetricsW(dc, &plain);
    int ascent = plain.tmAscent, descent = plain.tmDescent;
    if (GetOutlineTextMetricsW(dc, sizeof metrics, &metrics) && metrics.otmMacAscent)
        ascent = metrics.otmMacAscent, descent = -metrics.otmMacDescent;
    if (gdi_ascent) *gdi_ascent = plain.tmAscent;
    SelectObject(dc, old);
    DeleteDC(dc);
    return (line - ascent - descent) / 2 + ascent;
}

static void draw_line(HDC dc, HFONT font, const wchar_t *text, RECT box, int line, COLORREF ink, UINT flags)
{
    wchar_t shown[TEXT_MAX];
    int dx[TEXT_MAX], count = min(lstrlenW(text), TEXT_MAX - 2);
    memcpy(shown, text, count * sizeof *shown);
    int design, width = text_layout_design(font, shown, count, dx, &design), room = box.right - box.left;
    while ((flags & DT_END_ELLIPSIS) && width > room && count > 0) {  /* the name's end gives way to an ellipsis */
        shown[count - 1] = 0x2026;
        width = text_layout_design(font, shown, count, dx, &design);
        if (width > room) --count;
    }
    if (!count) return;
    /* centred on the design width, as Chromium centres it, and the origin rounded: GDI has no fractional one */
    int x = flags & DT_CENTER ? box.left + (room * DESIGN_SCALE - design + DESIGN_SCALE) / (2 * DESIGN_SCALE)
          : flags & DT_RIGHT ? box.right - width : box.left;
    HDC layer = CreateCompatibleDC(NULL);
    HGDIOBJ old_font = SelectObject(layer, font);
    int baseline = line_baseline(font, line, NULL), pad = 2, w = width + 2 * pad;
    BITMAPINFO info = {{sizeof(BITMAPINFOHEADER), w, -line, 1, 32, BI_RGB}};
    uint32_t *bits = NULL;
    HBITMAP bitmap = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (bitmap && bits) {
        HGDIOBJ old_bitmap = SelectObject(layer, bitmap);
        memset(bits, 0, (size_t)w * line * 4);
        SetTextColor(layer, RGB(255, 255, 255));
        SetBkMode(layer, TRANSPARENT);
        SetTextAlign(layer, TA_BASELINE | TA_LEFT);
        ExtTextOutW(layer, pad, baseline, 0, NULL, shown, count, dx);
        GdiFlush();
        for (int i = 0; i < w * line; ++i) {  /* grayscale coverage to premultiplied ink */
            uint32_t c = bits[i] & 0xff;
            bits[i] = c << 24 | (GetRValue(ink) * c / 255) << 16 | (GetGValue(ink) * c / 255) << 8 | GetBValue(ink) * c / 255;
        }
        BLENDFUNCTION over = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        GdiAlphaBlend(dc, x - pad, box.top, w, line, layer, 0, 0, w, line, over);
        SelectObject(layer, old_bitmap);
    }
    if (bitmap) DeleteObject(bitmap);
    SelectObject(layer, old_font);
    DeleteDC(layer);
}

static RECT rect_of(PwBox b) { RECT r = {b.x, b.y, b.x + b.w, b.y + b.h}; return r; }

/* A glyph drawn in TextFillColorPrimary, brought to an ink over fill (secondary, or the disabled text) with a veil. */
static void veil(HDC dc, int x, int y, int size, PwRgba fill, PwRgba ink)
{
    PwRgba primary = pw_chrome_blend(app_state.theme->text, fill), target = pw_chrome_blend(ink, fill);
    int span = (int)primary.r - fill.r, alpha = span ? 255 * ((int)primary.r - target.r) / span : 0;
    HDC layer = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, size, size);
    HGDIOBJ old = SelectObject(layer, bitmap);
    RECT all = {0, 0, size, size};
    SetDCBrushColor(layer, RGB(fill.r, fill.g, fill.b));
    FillRect(layer, &all, GetStockObject(DC_BRUSH));
    BLENDFUNCTION over = {AC_SRC_OVER, 0, (BYTE)max(0, min(alpha, 255)), 0};
    GdiAlphaBlend(dc, x, y, size, size, layer, 0, 0, size, size, over);
    SelectObject(layer, old);
    DeleteObject(bitmap);
    DeleteDC(layer);
}

/* ---- windows: theme ---- */

static wchar_t icons_path[MAX_PATH];

/* The prototype's font stack falls to Proton UI, the Selawik derivative every prefix carries for Segoe UI
 * (themes/windows11/README.md), whatever the prefix's menu font is. Its Semibold is a family of its own to GDI ("Proton
 * UI" at 600 finds Bold). */
static HFONT ramp_font(int px, int weight)
{
    LOGFONTW font = {-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Proton UI"};
    if (weight == FW_SEMIBOLD) lstrcpyW(font.lfFaceName, L"Proton UI Semibold");
    return CreateFontIndirectW(&font);
}

static void free_theme(void)
{
    HGDIOBJ objects[] = {app_state.background, app_state.edit_fill, app_state.edit_focused_fill, app_state.body,
                         app_state.strong, app_state.caption, app_state.subtitle};
    for (size_t i = 0; i < ARRAY_SIZE(objects); ++i) if (objects[i]) DeleteObject(objects[i]);
    for (size_t i = 0; i < ARRAY_SIZE(app_state.glyphs); ++i) if (app_state.glyphs[i]) DestroyIcon(app_state.glyphs[i]);
    memset(app_state.glyphs, 0, sizeof app_state.glyphs);
}

/* Light or dark follows AppsUseLightTheme, as the shell's surfaces do. */
static void load_theme(void)
{
    DWORD light = 1, size = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size);
    int dark = !light;
    free_theme();
    const PwChromeTheme *t = app_state.theme = pw_chrome_theme(dark);
    app_state.background = CreateSolidBrush(RGB(t->surface.r, t->surface.g, t->surface.b));
    app_state.edit_fill = CreateSolidBrush(colour(t->control, t->surface));
    app_state.edit_focused_fill = CreateSolidBrush(colour(t->input_focused, t->surface));
    app_state.body = ramp_font(14, FW_NORMAL);
    app_state.strong = ramp_font(14, FW_SEMIBOLD);
    app_state.caption = ramp_font(12, FW_NORMAL);
    app_state.subtitle = ramp_font(20, FW_SEMIBOLD);
    /* PW_ICON_{LIGHT,DARK}_ARROW_LEFT, ARROW_RIGHT, ARROW_CLOCKWISE, X, DOWNLOAD and LOCK_CLOSED */
    static const int ids[6] = {2003, 2002, 2035, 2008, 2022, ICON_LOCK};
    GetWindowsDirectoryW(icons_path, MAX_PATH);
    lstrcatW(icons_path, L"\\resources\\mihawk\\icons.dll");
    HMODULE icons = LoadLibraryExW(icons_path, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    for (int i = 0; icons && i < 6; ++i)
        app_state.glyphs[i] = LoadImageW(icons, MAKEINTRESOURCEW(ids[i] + (dark ? 1000 : 0)), IMAGE_ICON, 16, 16, 0);
    if (icons && !app_state.app_icon) {
        app_state.app_icon = LoadImageW(icons, MAKEINTRESOURCEW(ICON_APP_BROWSER), IMAGE_ICON, 32, 32, 0);
        app_state.app_icon_small = LoadImageW(icons, MAKEINTRESOURCEW(ICON_APP_BROWSER), IMAGE_ICON, 16, 16, 0);
        app_state.file_icon = LoadImageW(icons, MAKEINTRESOURCEW(ICON_APP_GENERIC), IMAGE_ICON, 32, 32, 0);
    }
    if (!icons) say("no Mihawk icons at %ls", icons_path);
    else FreeLibrary(icons);
    int gdi_ascent;
    app_state.edit_offset = line_baseline(app_state.body, 20, &gdi_ascent) - gdi_ascent;
    /* the flyout's button labels, in Body, as draw_line lays them out */
    static const wchar_t *const labels[3] = {L"Cancel", L"Open", L"Show in folder"};
    for (int i = 0; i < 3; ++i) {
        int dx[TEXT_MAX];
        app_state.label_widths[i] = text_layout(app_state.body, labels[i], lstrlenW(labels[i]), dx);
    }
}

/* ---- windows: the command row ---- */

static WNDPROC button_proc;

/* Hot tracking for the owner-drawn buttons: the one under the pointer repaints with its hover fill. */
static LRESULT CALLBACK button_subclass(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_MOUSEMOVE && app_state.hot != window) {
        HWND old = app_state.hot;
        app_state.hot = window;
        TRACKMOUSEEVENT track = {sizeof track, TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        if (old) InvalidateRect(old, NULL, FALSE);
        InvalidateRect(window, NULL, FALSE);
    } else if (message == WM_MOUSELEAVE && app_state.hot == window) {
        app_state.hot = NULL;
        InvalidateRect(window, NULL, FALSE);
    }
    return CallWindowProcW(button_proc, window, message, wparam, lparam);
}

static HWND owner_button(HWND parent, const wchar_t *name, DWORD style, int id)
{
    HWND window = CreateWindowExW(0, L"BUTTON", name, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | style, 0, 0, 0, 0,
                                  parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    button_proc = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)button_subclass);
    return window;
}

static int flyout_shown(void) { return app_state.flyout && IsWindowVisible(app_state.flyout); }

static int percent_of(long long received, long long total)
{
    return total > 0 ? (int)(received * 100 / total) : 0;
}

static int downloads_percent(void)
{
    long long received = 0, total = 0;
    for (int i = 0; i < app_state.download_count; ++i)
        if (app_state.downloads_list[i].state == DL_ACTIVE && app_state.downloads_list[i].total > 0) {
            received += app_state.downloads_list[i].received;
            total += app_state.downloads_list[i].total;
        }
    return percent_of(received, total);
}

/* A 32 x 32 subtle command button (the prototype's command row): no plate at rest, a 4 px rounded fill on hover and
 * press (and on Downloads while its flyout is open), the 16 px glyph centred, at the disabled text opacity when
 * disabled; Downloads has a 16 px ProgressBar under its glyph while a download runs. */
static void draw_icon_button(const DRAWITEMSTRUCT *item)
{
    const PwChromeTheme *t = app_state.theme;
    RECT r = item->rcItem;
    HDC dc = item->hDC;
    int disabled = item->itemState & ODS_DISABLED, downloads = item->CtlID == ID_DOWNLOADS;
    int pressed = !disabled && item->itemState & ODS_SELECTED;
    int hot = !disabled && (app_state.hot == item->hwndItem || (downloads && flyout_shown()));
    PwRgba fill = pressed ? t->subtle_pressed : hot ? t->subtle_hover : no_colour;
    draw_plate(dc, r, 4, fill, no_colour, no_colour, 1, &t->surface);
    PwRgba under = pw_chrome_blend(fill, t->surface);
    int glyph = item->CtlID == ID_BACK ? 0 : item->CtlID == ID_FORWARD ? 1 : downloads ? 4 : app_state.loading ? 3 : 2;
    int x = r.left + (r.right - r.left - 16) / 2, y = r.top + (r.bottom - r.top - 16) / 2;
    if (app_state.glyphs[glyph]) DrawIconEx(dc, x, y, app_state.glyphs[glyph], 16, 16, 0, NULL, DI_NORMAL);
    if (disabled) veil(dc, x, y, 16, under, (PwRgba){t->text.r, t->text.g, t->text.b, 0.36f});  /* TextFillColorDisabled */
    if (downloads && downloads_active()) draw_progress(dc, r.left + 8, r.top + 25, 16, downloads_percent(), &under);
    if (item->itemState & ODS_FOCUS && !(item->itemState & ODS_NOFOCUSRECT)) DrawFocusRect(dc, &r);
}

/* ---- windows: the address box ---- */

static WNDPROC address_proc;

/* The placeholder in TextFillColorSecondary while the box is empty, focused or not, as the prototype's input shows it. */
static void paint_placeholder(HDC dc)
{
    if (GetWindowTextLengthW(app_state.address)) return;
    const PwChromeTheme *t = app_state.theme;
    PwRgba fill = pw_chrome_blend(GetFocus() == app_state.address ? t->input_focused : t->control, t->surface);
    RECT r;
    GetClientRect(app_state.address, &r);
    r.top -= app_state.edit_offset;  /* the line box, where the EDIT's own text lines up */
    draw_line(dc, app_state.body, L"Search DuckDuckGo or type an address", r, 20, colour(t->text_secondary, fill), DT_LEFT);
}

static LRESULT CALLBACK address_subclass(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_KEYDOWN && wparam == VK_RETURN) { navigate_address(); return 0; }
    if (message == WM_CHAR && (wparam == '\r' || wparam == 27)) return 0;
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE && app_state.browser) {
        char url[URL_MAX];
        cef_frame_t *frame = app_state.browser->get_main_frame(app_state.browser);
        utf8_of_userfree(frame->get_url(frame), url, sizeof url);
        RELEASE(frame);
        wchar_t text[URL_MAX];
        MultiByteToWideChar(CP_UTF8, 0, url, -1, text, URL_MAX);
        SetWindowTextW(window, text);
        return 0;
    }
    if (message == WM_LBUTTONUP && GetFocus() == window) {
        DWORD from = 0, to = 0;
        SendMessageW(window, EM_GETSEL, (WPARAM)&from, (LPARAM)&to);
        LRESULT result = CallWindowProcW(address_proc, window, message, wparam, lparam);
        if (from == to) SendMessageW(window, EM_SETSEL, 0, -1);
        return result;
    }
    LRESULT result = CallWindowProcW(address_proc, window, message, wparam, lparam);
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        /* the frame's focus line and the fill change with focus */
        RECT box = address_box();
        InvalidateRect(app_state.main, &box, FALSE);
        InvalidateRect(window, NULL, TRUE);
    } else if (message == WM_PAINT) {
        HDC dc = GetDC(window);
        paint_placeholder(dc);
        ReleaseDC(window, dc);
    }
    return result;
}

/* The TextBox's frame, and the lock on a loaded https page (16 px, 11 px in). */
static void paint_main(HDC dc)
{
    const PwChromeTheme *t = app_state.theme;
    RECT box = address_box();
    int focused = GetFocus() == app_state.address;
    PwRgba fill = focused ? t->input_focused : t->control;
    draw_plate(dc, box, 4, fill, t->input_stroke, focused ? t->accent : t->input_stroke_bottom, focused ? 2 : 1,
               &t->surface);
    if (app_state.secure && app_state.glyphs[5]) {
        PwRgba under = pw_chrome_blend(fill, t->surface);
        DrawIconEx(dc, box.left + ADDRESS_PAD, box.top + 8, app_state.glyphs[5], 16, 16, 0, NULL, DI_NORMAL);
        veil(dc, box.left + ADDRESS_PAD, box.top + 8, 16, under, t->text_secondary);
    }
}

/* ---- windows: the start view and the error panel ---- */

/* Where a Button's 20 px line of Body starts: .btn centres it below a 1 px border and 4 px of padding, above 6 px and
 * the bottom border (prototypes/windows11-shell/css/ui.css). */
static int button_line(int height) { return 1 + 4 + (height - 12 - 20) / 2; }

/* A launcher: a WinUI Button, 200 x 40, its name in Body. */
static void draw_launcher(const DRAWITEMSTRUCT *item)
{
    const PwChromeTheme *t = app_state.theme;
    RECT r = item->rcItem;
    int pressed = item->itemState & ODS_SELECTED, hot = app_state.hot == item->hwndItem;
    PwRgba fill = pressed ? t->control_pressed : hot ? t->control_hover : t->control;
    draw_plate(item->hDC, r, 4, fill, t->control_stroke, t->control_stroke_bottom, 1, &t->surface);
    wchar_t name[64];
    GetWindowTextW(item->hwndItem, name, ARRAY_SIZE(name));
    RECT line = {r.left, r.top + button_line(r.bottom - r.top), r.right, r.bottom};
    draw_line(item->hDC, app_state.body, name, line, 20, colour(t->text, pw_chrome_blend(fill, t->surface)), DT_CENTER);
    if (item->itemState & ODS_FOCUS && !(item->itemState & ODS_NOFOCUSRECT)) {
        InflateRect(&r, -3, -3);
        DrawFocusRect(item->hDC, &r);
    }
}

/* Two lines of Body from 40 px down, centred. */
static void paint_start(HDC dc)
{
    RECT r;
    GetClientRect(app_state.start, &r);
    FillRect(dc, &r, app_state.background);
    COLORREF ink = colour(app_state.theme->text, app_state.theme->surface);
    RECT first = {0, 40, r.right, 60}, second = {0, 60, r.right, 80};
    draw_line(dc, app_state.body, L"Search the web or type an address above.", first, 20, ink, DT_CENTER);
    draw_line(dc, app_state.body, L"Or get a game launcher:", second, 20, ink, DT_CENTER);
}

static LRESULT CALLBACK start_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_COMMAND && LOWORD(wparam) >= ID_LAUNCHER && LOWORD(wparam) < ID_LAUNCHER + ARRAY_SIZE(launchers)) {
        navigate(launchers[LOWORD(wparam) - ID_LAUNCHER].url);
        return 0;
    }
    if (message == WM_DRAWITEM) { draw_launcher((const DRAWITEMSTRUCT *)lparam); return TRUE; }
    if (message == WM_SIZE) {
        int width = LOWORD(lparam), columns = 3, w = 200, h = 40;
        int left = (width - columns * w - (columns - 1) * 8) / 2, top = 120;
        for (int i = 0; i < (int)ARRAY_SIZE(launchers); ++i)
            MoveWindow(GetDlgItem(window, ID_LAUNCHER + i), left + i % columns * (w + 8), top + i / columns * (h + 8), w, h, TRUE);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        paint_start(dc);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

/* The error panel: its title in Subtitle and the error in Body secondary, 8 px apart, centred in the page area. */
static void paint_error(HDC dc)
{
    const PwChromeTheme *t = app_state.theme;
    RECT r;
    GetClientRect(app_state.error, &r);
    FillRect(dc, &r, app_state.background);
    int detail = app_state.error_detail[0] != 0, height = 28 + (detail ? 8 + 20 : 0), top = (r.bottom - height) / 2;
    RECT title = {24, top, r.right - 24, top + 28}, line = {24, top + 36, r.right - 24, top + 56};
    draw_line(dc, app_state.subtitle, app_state.error_title, title, 28, colour(t->text, t->surface), DT_CENTER);
    if (detail)
        draw_line(dc, app_state.body, app_state.error_detail, line, 20, colour(t->text_secondary, t->surface), DT_CENTER);
}

static LRESULT CALLBACK error_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        paint_error(dc);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wparam, lparam);
}

/* ---- windows: the Downloads flyout ---- */

static PwRowState row_state(const Download *d)
{
    return d->state == DL_ACTIVE ? PW_ROW_ACTIVE : d->state == DL_DONE ? PW_ROW_DONE
         : d->state == DL_CANCELED ? PW_ROW_CANCELED : PW_ROW_FAILED;
}

static void paint_row(HDC dc, const Download *d, PwRowState state, const PwRowBoxes *b, int first_hit)
{
    const PwChromeTheme *t = app_state.theme;
    COLORREF primary = colour(t->text, t->flyout), secondary = colour(t->text_secondary, t->flyout);
    draw_plate(dc, (RECT){b->row.x, b->row.y, b->row.x + b->row.w, b->row.y + 1}, 0, t->divider, t->divider, t->divider, 1, NULL);
    if (app_state.file_icon) DrawIconEx(dc, b->icon.x, b->icon.y, app_state.file_icon, 32, 32, 0, NULL, DI_NORMAL);
    draw_line(dc, app_state.body, d->name, rect_of(b->name), 20, state == PW_ROW_CANCELED ? secondary : primary,
              DT_LEFT | DT_END_ELLIPSIS);
    if (state == PW_ROW_ACTIVE) draw_progress(dc, b->progress.x, b->progress.y, b->progress.w, percent_of(d->received, d->total), NULL);
    char received[32], total[32], status8[80];
    pw_chrome_size(d->received, received, sizeof received);
    pw_chrome_size(d->total, total, sizeof total);
    if (state == PW_ROW_ACTIVE) _snprintf(status8, sizeof status8, d->total > 0 ? "%s of %s" : "%s", received, total);
    else lstrcpynA(status8, state == PW_ROW_DONE ? received : state == PW_ROW_CANCELED ? "Canceled" : "Failed", sizeof status8);
    status8[sizeof status8 - 1] = 0;
    wchar_t status[80];
    MultiByteToWideChar(CP_UTF8, 0, status8, -1, status, ARRAY_SIZE(status));
    draw_line(dc, app_state.caption, status, rect_of(b->status), 16, secondary, DT_LEFT);
    char name8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, d->name, -1, name8, sizeof name8, NULL, NULL);
    static const wchar_t *const labels[3] = {L"Cancel", L"Open", L"Show in folder"};
    for (int a = 0; a < b->actions; ++a) {
        int hit = first_hit + a, action = app_state.hits[hit].action;
        int hot = app_state.flyout_hot == hit, pressed = hot && app_state.flyout_pressed == hit;
        int accent = action == ACTION_OPEN && pw_browser_is_installer(name8);  /* Open is the accent for an installer */
        PwRgba fill = accent ? (hot ? t->accent_hover : t->accent) : pressed ? t->control_pressed : hot ? t->control_hover : t->control;
        RECT box = rect_of(b->action[a]);
        draw_plate(dc, box, 4, fill, accent ? t->accent_stroke : t->control_stroke,
                   accent ? t->accent_stroke_bottom : t->control_stroke_bottom, 1, NULL);
        RECT line = {box.left, box.top + button_line(box.bottom - box.top), box.right, box.bottom};
        draw_line(dc, app_state.body, labels[action], line, 20, accent ? colour(t->on_accent, t->accent) : primary, DT_CENTER);
        if (app_state.flyout_focus == hit) {  /* the focus visual: a 2 px ring 3 px out */
            for (int ring = 3; ring >= 2; --ring) {
                RECT outer = box;
                InflateRect(&outer, ring, ring);
                draw_plate(dc, outer, 4 + ring, no_colour, t->text, t->text, 1, NULL);
            }
        }
    }
}

/* Lays the rows out newest first where the scroll puts them, recording their buttons; paints them when dc is given.
 * Returns the rows' height. */
static int flyout_walk(HDC dc, int height)
{
    int top = 1 + PW_FLYOUT_HEAD - app_state.flyout_scroll, content = 0;
    app_state.hit_count = 0;
    for (int i = app_state.download_count; i-- > 0;) {
        const Download *d = &app_state.downloads_list[i];
        PwRowState state = row_state(d);
        int labels[2] = {app_state.label_widths[state == PW_ROW_ACTIVE ? ACTION_CANCEL : ACTION_OPEN],
                         app_state.label_widths[ACTION_FOLDER]};
        PwRowBoxes b;
        pw_chrome_row(state, top, labels, &b);
        int first_hit = app_state.hit_count;
        for (int a = 0; a < b.actions; ++a)
            app_state.hits[app_state.hit_count++] = (FlyoutHit){rect_of(b.action[a]), d->id,
                state == PW_ROW_ACTIVE ? ACTION_CANCEL : a ? ACTION_FOLDER : ACTION_OPEN};
        if (dc && top < height && top + b.row.h > 1 + PW_FLYOUT_HEAD) paint_row(dc, d, state, &b, first_hit);
        top += b.row.h;
        content += b.row.h;
    }
    return content;
}

static void paint_flyout(HDC dc, int width, int height)
{
    const PwChromeTheme *t = app_state.theme;
    RECT head = {1 + 16, 1 + 12, width - 17, 1 + 32};
    draw_line(dc, app_state.strong, L"Downloads", head, 20, colour(t->text, t->flyout), DT_LEFT);
    if (!app_state.download_count) {
        RECT empty = {1 + 16, 1 + PW_FLYOUT_HEAD + 4, width - 17, 1 + PW_FLYOUT_HEAD + 24};
        draw_line(dc, app_state.body, L"No downloads yet", empty, 20, colour(t->text_secondary, t->flyout), DT_LEFT);
        return;
    }
    HRGN list = CreateRectRgn(1, 1 + PW_FLYOUT_HEAD, width - 1, height - 1);
    SelectClipRgn(dc, list);
    flyout_walk(dc, height);
    SelectClipRgn(dc, NULL);
    DeleteObject(list);
}

static int flyout_hit(int x, int y)
{
    RECT client;
    GetClientRect(app_state.flyout, &client);
    if (y < 1 + PW_FLYOUT_HEAD || y >= client.bottom - 1) return -1;
    POINT at = {x, y};
    for (int i = 0; i < app_state.hit_count; ++i) if (PtInRect(&app_state.hits[i].box, at)) return i;
    return -1;
}

static void flyout_act(int hit)
{
    if (hit < 0 || hit >= app_state.hit_count) return;
    Download *d = download_by_id(app_state.hits[hit].download);
    if (!d) return;
    switch (app_state.hits[hit].action) {
    case ACTION_CANCEL: if (d->state == DL_ACTIVE) d->cancel = 1; break;
    case ACTION_OPEN: download_open(d); break;
    case ACTION_FOLDER: {
        wchar_t parameters[MAX_PATH + 16];
        _snwprintf(parameters, MAX_PATH + 16, L"/select,\"%s\"", d->path);
        parameters[MAX_PATH + 15] = 0;
        ShellExecuteW(app_state.main, NULL, L"explorer.exe", parameters, NULL, SW_SHOWNORMAL);
        break;
    }
    }
}

/* Under Downloads, right edges 4 px apart, as tall as its rows up to 48 px short of the window's client. */
static void place_flyout(void)
{
    if (!app_state.flyout) return;
    RECT client;
    GetClientRect(app_state.main, &client);
    PwRowState states[MAX_DOWNLOADS];
    for (int i = 0; i < app_state.download_count; ++i) states[i] = row_state(&app_state.downloads_list[i]);
    int height = pw_chrome_flyout_height(states, app_state.download_count, max(client.bottom - FLYOUT_BELOW, 82));
    int rows = height - 2 - PW_FLYOUT_HEAD - PW_FLYOUT_FOOT, content = flyout_walk(NULL, height);
    app_state.flyout_scroll = max(0, min(app_state.flyout_scroll, content - rows));
    POINT at = {client.right - FLYOUT_RIGHT - PW_FLYOUT_WIDTH, TOOLBAR};
    ClientToScreen(app_state.main, &at);
    SetWindowPos(app_state.flyout, NULL, at.x, at.y, PW_FLYOUT_WIDTH, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The flyout's surface (wine/ps5/pw_shell_flyouts.c, pw_flyout_appearance): rounded, the theme's mode, a sheet of glass
 * the compositor fills with backdrop 7. */
static void flyout_surface(void)
{
    DWORD corner = DWMWCP_ROUND, dark = app_state.theme == pw_chrome_theme(1), backdrop = FLYOUT_BACKDROP;
    DWORD border = DWMWA_COLOR_DEFAULT;
    MARGINS sheet = {-1, -1, -1, -1};
    DwmSetWindowAttribute(app_state.flyout, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof corner);
    DwmSetWindowAttribute(app_state.flyout, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    DwmSetWindowAttribute(app_state.flyout, DWMWA_BORDER_COLOR, &border, sizeof border);
    DwmExtendFrameIntoClientArea(app_state.flyout, &sheet);
    DwmSetWindowAttribute(app_state.flyout, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof backdrop);
}

static void show_downloads(int show)
{
    if (!show) {
        if (flyout_shown()) ShowWindow(app_state.flyout, SW_HIDE);
        InvalidateRect(app_state.downloads, NULL, FALSE);
        return;
    }
    if (!app_state.flyout) {
        app_state.flyout = CreateWindowExW(WS_EX_TOOLWINDOW, L"MihawkBrowserDownloads", L"Downloads", WS_POPUP, 0, 0, 0, 0,
                                           app_state.main, NULL, GetModuleHandleW(NULL), NULL);
        if (!app_state.flyout) { say("downloads flyout not made error=%lu", GetLastError()); return; }
        flyout_surface();
    }
    app_state.flyout_scroll = 0;
    app_state.flyout_hot = app_state.flyout_pressed = app_state.flyout_focus = -1;
    place_flyout();
    if (!flyout_shown()) {
        ShowWindow(app_state.flyout, SW_SHOW);
        say("downloads shown rows=%d", app_state.download_count);
    }
    InvalidateRect(app_state.downloads, NULL, FALSE);
}

static void downloads_changed(void)
{
    place_flyout();
    if (app_state.flyout) InvalidateRect(app_state.flyout, NULL, TRUE);
    InvalidateRect(app_state.downloads, NULL, FALSE);
}

static LRESULT CALLBACK flyout_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_ACTIVATE:
        /* light dismiss, as the prototype's transient surfaces close */
        if (LOWORD(wparam) == WA_INACTIVE && flyout_shown()) { app_state.flyout_closed = GetTickCount64(); show_downloads(0); }
        return 0;
    case WM_ERASEBKGND: {
        RECT box;
        HDC dc = (HDC)wparam;
        if (GetClipBox(dc, &box) != ERROR) PatBlt(dc, box.left, box.top, box.right - box.left, box.bottom - box.top, BLACKNESS);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint), buffer = NULL;
        RECT client;
        GetClientRect(window, &client);
        BP_PAINTPARAMS params = {sizeof params, BPPF_ERASE};
        HPAINTBUFFER glass = BeginBufferedPaint(dc, &client, BPBF_TOPDOWNDIB, &params, &buffer);
        if (glass) {
            paint_flyout(buffer, client.right, client.bottom);
            EndBufferedPaint(glass, TRUE);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int hit = flyout_hit((short)LOWORD(lparam), (short)HIWORD(lparam));
        TRACKMOUSEEVENT track = {sizeof track, TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        if (hit != app_state.flyout_hot) { app_state.flyout_hot = hit; InvalidateRect(window, NULL, TRUE); }
        return 0;
    }
    case WM_MOUSELEAVE: app_state.flyout_hot = -1; InvalidateRect(window, NULL, TRUE); return 0;
    case WM_LBUTTONDOWN:
        app_state.flyout_pressed = flyout_hit((short)LOWORD(lparam), (short)HIWORD(lparam));
        SetCapture(window);
        InvalidateRect(window, NULL, TRUE);
        return 0;
    case WM_LBUTTONUP: {
        int hit = flyout_hit((short)LOWORD(lparam), (short)HIWORD(lparam)), pressed = app_state.flyout_pressed;
        ReleaseCapture();
        app_state.flyout_pressed = -1;
        InvalidateRect(window, NULL, TRUE);
        if (hit >= 0 && hit == pressed) flyout_act(hit);
        return 0;
    }
    case WM_MOUSEWHEEL:
        app_state.flyout_scroll -= GET_WHEEL_DELTA_WPARAM(wparam) * 40 / WHEEL_DELTA;
        place_flyout();  /* clamps the scroll */
        InvalidateRect(window, NULL, TRUE);
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) { show_downloads(0); SetFocus(app_state.address); return 0; }
        if (wparam == VK_TAB && app_state.hit_count) {
            int step = GetKeyState(VK_SHIFT) < 0 ? app_state.hit_count - 1 : 1;
            app_state.flyout_focus = (app_state.flyout_focus + step + app_state.hit_count) % app_state.hit_count;
            if (app_state.flyout_focus < 0) app_state.flyout_focus = 0;
            InvalidateRect(window, NULL, TRUE);
            return 0;
        }
        if ((wparam == VK_RETURN || wparam == VK_SPACE) && app_state.flyout_focus >= 0) { flyout_act(app_state.flyout_focus); return 0; }
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

/* ---- snapshots ---- */

static void save_bitmap(const char *folder, const char *name, const char *part, const void *bits, int w, int h)
{
    char path[MAX_PATH];
    _snprintf(path, sizeof path, "%s\\%s-%s.bmp", folder, name, part);
    path[sizeof path - 1] = 0;
    BITMAPINFOHEADER info = {sizeof info, w, -h, 1, 32, BI_RGB};
    BITMAPFILEHEADER file = {0x4d42, (DWORD)(sizeof file + sizeof info + (size_t)w * h * 4), 0, 0, sizeof file + sizeof info};
    HANDLE out = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD written;
    if (out == INVALID_HANDLE_VALUE) { say("snapshot not written error=%lu", GetLastError()); return; }
    WriteFile(out, &file, sizeof file, &written, NULL);
    WriteFile(out, &info, sizeof info, &written, NULL);
    WriteFile(out, bits, (DWORD)((size_t)w * h * 4), &written, NULL);
    CloseHandle(out);
}

static RECT client_rect_of(HWND window)
{
    RECT r;
    GetWindowRect(window, &r);
    MapWindowPoints(NULL, app_state.main, (POINT *)&r, 2);
    return r;
}

static void draw_into(HDC dc, HWND window, void (*draw)(const DRAWITEMSTRUCT *), int id, int state)
{
    RECT at = client_rect_of(window);
    DRAWITEMSTRUCT item = {ODT_BUTTON, (UINT)id, 0, ODA_DRAWENTIRE, (UINT)state, window, dc,
                           {at.left, at.top, at.right, at.bottom}, 0};
    draw(&item);
}

/* "snapshot NAME" writes NAME-main.bmp (the main window's client) and, while it is open, NAME-flyout.bmp into
 * MIHAWK_BROWSER_SNAPSHOTS, through the paint code the windows use: the host oracle has no display, and
 * tools/check-browser-reference.py compares these with the prototype's pixels. The page itself (CEF) is not drawn, and
 * the flyout lies on its acrylic's measured tint, as the compositor's backdrop 7 shows it. */
static void snapshot(const char *name)
{
    char folder[MAX_PATH];
    if (!GetEnvironmentVariableA("MIHAWK_BROWSER_SNAPSHOTS", folder, sizeof folder)) { say("snapshot skipped"); return; }
    const PwChromeTheme *t = app_state.theme;
    RECT client;
    GetClientRect(app_state.main, &client);
    for (int part = 0; part < 2; ++part) {
        int w = part ? PW_FLYOUT_WIDTH : client.right, h = client.bottom;
        if (part) {
            if (!flyout_shown()) break;
            RECT box;
            GetClientRect(app_state.flyout, &box);
            h = box.bottom;
        }
        BITMAPINFO info = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
        void *bits = NULL;
        HBITMAP bitmap = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0);
        HDC dc = CreateCompatibleDC(NULL);
        HGDIOBJ old = SelectObject(dc, bitmap);
        RECT all = {0, 0, w, h};
        SetDCBrushColor(dc, part ? RGB(t->flyout.r, t->flyout.g, t->flyout.b) : RGB(t->surface.r, t->surface.g, t->surface.b));
        FillRect(dc, &all, GetStockObject(DC_BRUSH));
        if (part) paint_flyout(dc, w, h);
        else {
            paint_main(dc);
            HWND buttons[4] = {app_state.back, app_state.forward, app_state.reload, app_state.downloads};
            for (int i = 0; i < 4; ++i)
                draw_into(dc, buttons[i], draw_icon_button, GetDlgCtrlID(buttons[i]), IsWindowEnabled(buttons[i]) ? 0 : ODS_DISABLED);
            POINT origin;
            RECT edit = client_rect_of(app_state.address);
            int saved = SaveDC(dc);  /* the EDIT leaves its clip on the DC */
            SetViewportOrgEx(dc, edit.left, edit.top, &origin);
            SendMessageW(app_state.address, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT | PRF_ERASEBKGND);
            paint_placeholder(dc);
            RestoreDC(dc, saved);
            HWND views[2] = {app_state.start, app_state.error};
            for (int i = 0; i < 2; ++i) {
                if (!(GetWindowLongW(views[i], GWL_STYLE) & WS_VISIBLE)) continue;  /* its own flag: the host's main window is never shown */
                RECT at = client_rect_of(views[i]);
                SetViewportOrgEx(dc, at.left, at.top, &origin);
                if (i) paint_error(dc);
                else paint_start(dc);
                SetViewportOrgEx(dc, origin.x, origin.y, NULL);
                for (int l = 0; !i && l < (int)ARRAY_SIZE(launchers); ++l) {
                    HWND launcher = GetDlgItem(app_state.start, ID_LAUNCHER + l);
                    draw_into(dc, launcher, draw_launcher, ID_LAUNCHER + l, 0);
                }
            }
        }
        GdiFlush();
        save_bitmap(folder, name, part ? "flyout" : "main", bits, w, h);
        SelectObject(dc, old);
        DeleteDC(dc);
        DeleteObject(bitmap);
    }
    say("snapshot %s client=%ldx%ld flyout=%d", name, client.right, client.bottom, flyout_shown());
}

/* ---- windows: the main window ---- */

static HWND child(const wchar_t *class_name, const wchar_t *text, DWORD style, HWND parent, int id)
{
    HWND window = CreateWindowExW(0, class_name, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                                  (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(window, WM_SETFONT, (WPARAM)app_state.body, FALSE);
    return window;
}

static LRESULT CALLBACK main_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) show_downloads(0);
        if (app_state.address) layout();
        return 0;
    case WM_MOVE: place_flyout(); return 0;
    case WM_GETMINMAXINFO:
        /* a capture's 1278 x 766 client fits no screen smaller than it, as the host oracle's 1024 x 768 */
        if (GetEnvironmentVariableA("MIHAWK_BROWSER_CLIENT", NULL, 0)) {
            ((MINMAXINFO *)lparam)->ptMaxTrackSize.x = ((MINMAXINFO *)lparam)->ptMaxTrackSize.y = 4096;
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        paint_main(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == ID_DOWNLOADS) {
            /* a click on Downloads closes an open flyout by light dismiss first: it does not open it again */
            if (flyout_shown()) show_downloads(0);
            else if (GetTickCount64() - app_state.flyout_closed > 500) show_downloads(1);
            return 0;
        }
        if (!app_state.browser) return 0;
        switch (LOWORD(wparam)) {
        case ID_BACK: app_state.browser->go_back(app_state.browser); return 0;
        case ID_FORWARD: app_state.browser->go_forward(app_state.browser); return 0;
        case ID_RELOAD:
            if (app_state.loading) app_state.browser->stop_load(app_state.browser);
            else app_state.browser->reload(app_state.browser);
            return 0;
        }
        return 0;
    case WM_TIMER: on_tick(); return 0;
    case WM_DRAWITEM: draw_icon_button((const DRAWITEMSTRUCT *)lparam); return TRUE;
    case WM_CTLCOLOREDIT: {
        const PwChromeTheme *t = app_state.theme;
        int focused = (HWND)lparam == GetFocus();
        PwRgba fill = pw_chrome_blend(focused ? t->input_focused : t->control, t->surface);
        SetTextColor((HDC)wparam, colour(t->text, fill));
        SetBkColor((HDC)wparam, RGB(fill.r, fill.g, fill.b));
        return (LRESULT)(focused ? app_state.edit_focused_fill : app_state.edit_fill);
    }
    case WM_SETTINGCHANGE: case WM_THEMECHANGED:
        load_theme();
        if (app_state.flyout) flyout_surface();
        RedrawWindow(window, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        if (app_state.flyout) InvalidateRect(app_state.flyout, NULL, TRUE);
        return 0;
    case WM_CLOSE:
        if (downloads_active() && !app_state.close_after) {
            /* Teardown order (§3.5): cancel, let Chromium remove the partial files, then close (on_tick). */
            for (int i = 0; i < app_state.download_count; ++i) app_state.downloads_list[i].cancel = 1;
            app_state.close_after = now_ms() + 5000;
            say("close waits for downloads to cancel");
            return 0;
        }
        if (app_state.browser && !app_state.closing) {
            /* Ask CEF first: do_close then sends WM_CLOSE again, and the second one closes (cefclient's order). */
            cef_browser_host_t *host = app_state.browser->get_host(app_state.browser);
            host->close_browser(host, 0);
            RELEASE(host);
            return 0;
        }
        break;
    case WM_DESTROY:
        if (!app_state.browser) cef_quit_message_loop();
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

/* MIHAWK_BROWSER_CLIENT="x,y" puts the client's top left there at the prototype's 1278 x 766, so a capture compares
 * with the reference point for point (its client is at 321,149). */
static void place_for_capture(HWND window)
{
    char text[32];
    int x, y;
    if (!GetEnvironmentVariableA("MIHAWK_BROWSER_CLIENT", text, sizeof text) || sscanf(text, "%d,%d", &x, &y) != 2) return;
    RECT frame, client;
    POINT origin = {0, 0};
    GetWindowRect(window, &frame);
    GetClientRect(window, &client);
    ClientToScreen(window, &origin);
    int left = origin.x - frame.left, top = origin.y - frame.top;
    int width = frame.right - frame.left - client.right + 1278, height = frame.bottom - frame.top - client.bottom + 766;
    SetWindowPos(window, NULL, x - left, y - top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    say("placed client=%d,%d size=1278x766", x, y);
}

static void create_main_window(void)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    INITCOMMONCONTROLSEX controls = {sizeof controls, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    BufferedPaintInit();
    load_theme();
    WNDCLASSEXW classes[] = {
        {sizeof(WNDCLASSEXW), 0, main_proc, 0, 0, instance, app_state.app_icon, LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
         app_state.background, NULL, L"MihawkBrowser", app_state.app_icon_small},
        {sizeof(WNDCLASSEXW), 0, start_proc, 0, 0, instance, NULL, LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), NULL, NULL,
         L"MihawkBrowserStart", NULL},
        {sizeof(WNDCLASSEXW), 0, error_proc, 0, 0, instance, NULL, LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), NULL, NULL,
         L"MihawkBrowserError", NULL},
        {sizeof(WNDCLASSEXW), 0, flyout_proc, 0, 0, instance, NULL, LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), NULL,
         NULL, L"MihawkBrowserDownloads", NULL},
    };
    for (size_t i = 0; i < ARRAY_SIZE(classes); ++i) RegisterClassExW(&classes[i]);
    app_state.main = CreateWindowExW(0, L"MihawkBrowser", L"MihawkBrowser", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                     CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, NULL, NULL, instance, NULL);
    app_state.back = owner_button(app_state.main, L"Back", WS_DISABLED, ID_BACK);
    app_state.forward = owner_button(app_state.main, L"Forward", WS_DISABLED, ID_FORWARD);
    app_state.reload = owner_button(app_state.main, L"Reload", 0, ID_RELOAD);
    app_state.downloads = owner_button(app_state.main, L"Downloads", 0, ID_DOWNLOADS);
    app_state.start = child(L"MihawkBrowserStart", L"", WS_CLIPCHILDREN, app_state.main, 0);
    for (size_t i = 0; i < ARRAY_SIZE(launchers); ++i) owner_button(app_state.start, launchers[i].name, 0, ID_LAUNCHER + (int)i);
    app_state.error = child(L"MihawkBrowserError", L"", 0, app_state.main, 0);
    ShowWindow(app_state.error, SW_HIDE);
    app_state.address = child(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, app_state.main, ID_ADDRESS);
    SendMessageW(app_state.address, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    address_proc = (WNDPROC)SetWindowLongPtrW(app_state.address, GWLP_WNDPROC, (LONG_PTR)address_subclass);
    place_for_capture(app_state.main);
    layout();
    ShowWindow(app_state.main, SW_SHOWDEFAULT);
    UpdateWindow(app_state.main);
    SetFocus(app_state.address);
    SetTimer(app_state.main, TIMER_TICK, 250, NULL);
    say("window ms=%llu", now_ms());
}

/* ---- CEF's application ---- */

static void CEF_CALLBACK on_before_command_line_processing(cef_app_t *self, const cef_string_t *process_type,
                                                           cef_command_line_t *command_line)
{
    (void)self;
    if (!process_type || !process_type->length) {
        for (size_t i = 0; pw_browser_switches[i][0]; ++i) {
            cef_string_t name = {0}, value = {0};
            cef_set_utf8(&name, pw_browser_switches[i][0]);
            if (pw_browser_switches[i][1]) {
                cef_set_utf8(&value, pw_browser_switches[i][1]);
                command_line->append_switch_with_value(command_line, &name, &value);
            } else {
                command_line->append_switch(command_line, &name);
            }
            cef_string_clear(&name);
            cef_string_clear(&value);
        }
    }
    RELEASE(command_line);
}

/* The policies of pw_browser_policies, rewritten at every start so a re-pin's list replaces the last one. */
static void write_policies(void)
{
    HKEY key;
    RegDeleteTreeA(HKEY_CURRENT_USER, PW_BROWSER_POLICY_KEY);
    if (RegCreateKeyExA(HKEY_CURRENT_USER, PW_BROWSER_POLICY_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL)) {
        say("policies not written");
        return;
    }
    for (const PwBrowserPolicy *policy = pw_browser_policies; policy->name; ++policy) {
        if (policy->text) RegSetValueExA(key, policy->name, 0, REG_SZ, (const BYTE *)policy->text, lstrlenA(policy->text) + 1);
        else RegSetValueExA(key, policy->name, 0, REG_DWORD, (const BYTE *)&policy->value, sizeof policy->value);
    }
    RegCloseKey(key);
}

static char start_url[URL_MAX];

static void CEF_CALLBACK on_context_initialized(cef_browser_process_handler_t *self)
{
    (void)self;
    create_main_window();
    if (start_url[0]) navigate(start_url);
    script_next();
}

/* A second MihawkBrowser (the Start entry clicked again, another program opening a link) hands its command line to
 * this one through Chrome's process singleton and exits. Unhandled, CEF would open a Chrome-style window here. */
static int CEF_CALLBACK on_already_running_app_relaunch(cef_browser_process_handler_t *self, cef_command_line_t *command_line,
                                                        const cef_string_t *current_directory)
{
    (void)self; (void)current_directory;
    cef_string_list_t arguments = cef_string_list_alloc();
    command_line->get_arguments(command_line, arguments);
    cef_string_t first = {0};
    char text[URL_MAX * 3], url[URL_MAX];
    if (cef_string_list_size(arguments) && cef_string_list_value(arguments, 0, &first)) {
        utf8_of(&first, text, sizeof text);
        if (pw_browser_address(text, url, sizeof url) == PW_OK) navigate(url);
    }
    cef_string_clear(&first);
    cef_string_list_free(arguments);
    say("relaunch handed over");
    ShowWindow(app_state.main, IsIconic(app_state.main) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(app_state.main);
    RELEASE(command_line);
    return 1;
}

static cef_browser_process_handler_t browser_process_handler = {
    .base = STATIC_BASE(cef_browser_process_handler_t),
    .on_context_initialized = on_context_initialized,
    .on_already_running_app_relaunch = on_already_running_app_relaunch,
};

static cef_browser_process_handler_t *CEF_CALLBACK get_browser_process_handler(cef_app_t *self)
{
    (void)self;
    return &browser_process_handler;
}

static cef_app_t cef_application = {
    .base = STATIC_BASE(cef_app_t),
    .on_before_command_line_processing = on_before_command_line_processing,
    .get_browser_process_handler = get_browser_process_handler,
};

int main(void)
{
    FILETIME created, unused;
    GetProcessTimes(GetCurrentProcess(), &created, &unused, &unused, &unused);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULONGLONG age = ((ULARGE_INTEGER){{now.dwLowDateTime, now.dwHighDateTime}}.QuadPart -
                     (ULARGE_INTEGER){{created.dwLowDateTime, created.dwHighDateTime}}.QuadPart) / 10000;
    app_state.process_start = GetTickCount64() - age;  /* times are from the process's creation, not main()'s */

    const wchar_t *line = GetCommandLineW(), *type = wcsstr(line, L"--type="), *sub = wcsstr(line, L"--utility-sub-type=");
    wchar_t kind[32] = L"browser", service[64] = L"";
    if (type) lstrcpynW(kind, type + 7, 32);
    if (sub) lstrcpynW(service, sub + 19, 64);
    for (wchar_t *p = kind; *p; ++p) if (*p == L' ' || *p == L'"') *p = 0;
    for (wchar_t *p = service; *p; ++p) if (*p == L' ' || *p == L'"') *p = 0;
    say("process type=%ls%s%ls pid=%lu", kind, sub ? " service=" : "", service, GetCurrentProcessId());

    cef_api_hash(CEF_API_VERSION, 0);
    cef_main_args_t args = {GetModuleHandleW(NULL)};
    int code = cef_execute_process(&args, &cef_application, NULL);
    if (code >= 0) return code;  /* a CEF subprocess, finished */

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(line, &argc);
    for (int i = 1; argv && i < argc; ++i) {
        if (argv[i][0] == L'-') continue;
        char text8[URL_MAX * 3];
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, text8, sizeof text8, NULL, NULL);
        if (pw_browser_address(text8, start_url, sizeof start_url) != PW_OK) start_url[0] = 0;
        break;
    }
    if (argv) LocalFree(argv);
    if (GetEnvironmentVariableA("MIHAWK_BROWSER_SCRIPT", app_state.script, sizeof app_state.script)) {
        app_state.step = app_state.script;
        for (const char *p = app_state.script; *p; ++p) app_state.step_count += *p == ';';
        ++app_state.step_count;
    }

    wchar_t data[MAX_PATH], cache[MAX_PATH], log[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", data, MAX_PATH)) lstrcpyW(data, L"C:\\users\\Public");
    _snwprintf(cache, MAX_PATH, L"%s\\MihawkBrowser\\User Data", data);
    _snwprintf(log, MAX_PATH, L"%s\\MihawkBrowser\\debug.log", data);
    write_policies();
    cef_settings_t settings = {0};
    settings.size = sizeof settings;
    settings.no_sandbox = 1;
    settings.persist_session_cookies = 1;
    settings.log_severity = LOGSEVERITY_WARNING;
    cef_set_wide(&settings.root_cache_path, cache);
    cef_set_wide(&settings.cache_path, cache);
    cef_set_wide(&settings.log_file, log);
    cef_set_utf8(&settings.locale, "en-US");
    cef_set_utf8(&settings.javascript_flags, "--jitless");
    cef_set_utf8(&settings.chrome_policy_id, PW_BROWSER_POLICY_KEY);
    if (!cef_initialize(&args, &settings, &cef_application, NULL)) {
        say("initialize failed code=%d", cef_get_exit_code());
        return cef_get_exit_code();
    }
    cef_run_message_loop();
    cef_shutdown();
    say("exit ms=%llu", now_ms());
    return 0;
}
