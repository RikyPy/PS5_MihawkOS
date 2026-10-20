/* SPDX-License-Identifier: GPL-3.0-or-later
 * A separate process changes the shared theme while this window stays alive.
 * WM_THEMECHANGED, system colours and newly opened theme data must agree.
 * Restore the prior diagnostic-prefix selection; no game prefix is targeted.
 */
#define UNICODE
#include <windows.h>
#include <uxtheme.h>
#include <vssym32.h>
#include "appearance.h"

typedef HRESULT (WINAPI *open_fn)(const WCHAR *,const WCHAR *,const WCHAR *,HANDLE *,DWORD);
typedef HRESULT (WINAPI *apply_fn)(HANDLE,char *,HWND);
typedef HRESULT (WINAPI *close_fn)(HANDLE);
static unsigned checks, failures, notifications;
static HMODULE themes;
static HWND window;

static void check(BOOL okay,const char *name)
{
    ++checks; failures += !okay;
    if (!okay) { char text[180];wsprintfA(text,"theme-notify: FAIL %s error=%lu\n",name,GetLastError());OutputDebugStringA(text); }
}

static HRESULT apply(const WCHAR *path,const WCHAR *color,const WCHAR *size)
{
    open_fn open=(open_fn)GetProcAddress(themes,MAKEINTRESOURCEA(2));
    apply_fn set=(apply_fn)GetProcAddress(themes,MAKEINTRESOURCEA(4));
    close_fn close=(close_fn)GetProcAddress(themes,MAKEINTRESOURCEA(3));
    HANDLE file=NULL;
    if (!open || !set || !close) return E_NOTIMPL;
    if (!path) return set(NULL,NULL,window);
    HRESULT result=open(path,color,size,&file,0);
    if (SUCCEEDED(result)) { result=set(file,NULL,window);close(file); }
    return result;
}

static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM w,LPARAM l)
{
    if (message==WM_THEMECHANGED) { ++notifications;InvalidateRect(hwnd,NULL,TRUE); }
    if (message==WM_PAINT) {
        PAINTSTRUCT paint;HDC dc=BeginPaint(hwnd,&paint);
        HBRUSH brush=CreateSolidBrush(GetSysColor(COLOR_WINDOW));
        FillRect(dc,&paint.rcPaint,brush);DeleteObject(brush);
        SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));SetBkMode(dc,TRANSPARENT);
        const WCHAR *text=L"Appearance changed by another process";
        TextOutW(dc,18,32,text,lstrlenW(text));EndPaint(hwnd,&paint);return 0;
    }
    return DefWindowProcW(hwnd,message,w,l);
}

void mainCRTStartup(void)
{
    WCHAR path[MAX_PATH],prior[MAX_PATH],prior_color[64],prior_size[64];
    themes=LoadLibraryW(L"uxtheme.dll");
    if (!themes) ExitProcess(1);
    GetWindowsDirectoryW(path,MAX_PATH);lstrcatW(path,L"\\resources\\themes\\light\\light.msstyles");
    for (const WCHAR *p=GetCommandLineW();*p;++p) {
        if (!lstrcmpW(p,L"--change=Dark")) ExitProcess(FAILED(apply(path,L"Dark",L"NormalSize")));
    }
    HRESULT saved=GetCurrentThemeName(prior,MAX_PATH,prior_color,64,prior_size,64);
    check(SUCCEEDED(apply(path,L"Blue",L"NormalSize")),"initial light theme");
    WNDCLASSW type={0};type.hInstance=GetModuleHandleW(NULL);type.lpfnWndProc=procedure;
    type.lpszClassName=L"MihawkThemeNotification";type.hCursor=LoadCursorW(NULL,IDC_ARROW);
    check(RegisterClassW(&type)!=0,"window class");
    window=CreateWindowW(type.lpszClassName,L"Theme notification probe",WS_OVERLAPPEDWINDOW,
                         760,430,400,220,NULL,NULL,type.hInstance,NULL);
    check(window!=NULL,"persistent window");ShowWindow(window,SW_SHOW);UpdateWindow(window);
    HTHEME retained = OpenThemeData(window,L"Window");
    LOGBRUSH brush;
    check(GetObjectW(GetSysColorBrush(COLOR_WINDOW),sizeof(brush),&brush) == sizeof(brush) &&
          brush.lbColor == PW_APPEARANCE_CLASSIC_LIGHT_WINDOW,"prime initial system brush cache");
    notifications=0;
    WCHAR executable[MAX_PATH];static WCHAR command[4096];
    GetModuleFileNameW(NULL,executable,MAX_PATH);wsprintfW(command,L"\"%s\" --change=Dark",executable);
    STARTUPINFOW startup={0};PROCESS_INFORMATION process={0};startup.cb=sizeof(startup);
    BOOL started=CreateProcessW(NULL,command,NULL,NULL,FALSE,0,NULL,NULL,&startup,&process);
    check(started,"separate theme-changing process");
    if (started) {
        DWORD begin=GetTickCount(),wait=WAIT_TIMEOUT;
        while (GetTickCount()-begin<20000) {
            wait=MsgWaitForMultipleObjects(1,&process.hProcess,FALSE,200,QS_ALLINPUT);
            MSG message;while (PeekMessageW(&message,NULL,0,0,PM_REMOVE)) { TranslateMessage(&message);DispatchMessageW(&message); }
            if (wait==WAIT_OBJECT_0 || wait==WAIT_FAILED) break;
        }
        check(wait==WAIT_OBJECT_0,"child completes within twenty seconds");
        DWORD code=~0u;check(GetExitCodeProcess(process.hProcess,&code) && !code,"child applied theme successfully");
        if (wait != WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess,1);
            WaitForSingleObject(process.hProcess,2000);
        }
        CloseHandle(process.hThread);CloseHandle(process.hProcess);
    }
    WCHAR file[MAX_PATH],color[64],size[64];
    check(notifications>0,"existing window receives theme notification");
    check(SUCCEEDED(GetCurrentThemeName(file,MAX_PATH,color,64,size,64)) && !lstrcmpW(color,L"Dark"),
          "existing process reports the changed theme");
    check(GetSysColor(COLOR_WINDOW)==PW_APPEARANCE_CLASSIC_DARK_WINDOW,"existing process uses changed pane colour");
    check(GetObjectW(GetSysColorBrush(COLOR_WINDOW),sizeof(brush),&brush) == sizeof(brush) &&
          brush.lbColor == PW_APPEARANCE_CLASSIC_DARK_WINDOW,"changed system brush cache");
    COLORREF old_text=0;
    check(retained && SUCCEEDED(GetThemeColor(retained,WP_CAPTION,CS_ACTIVE,TMT_TEXTCOLOR,&old_text)) &&
          old_text==PW_APPEARANCE_LIGHT_TEXT,"prior theme handle retains its owned data");
    if (retained) CloseThemeData(retained);
    HTHEME theme=OpenThemeData(window,L"Window");COLORREF text=0;
    check(theme && SUCCEEDED(GetThemeColor(theme,WP_CAPTION,CS_ACTIVE,TMT_TEXTCOLOR,&text)) &&
          text==PW_APPEARANCE_DARK_TEXT,"new theme handle uses changed caption text");
    if (theme) CloseThemeData(theme);
    char diagnostic[180];wsprintfA(diagnostic,"theme-notify: notifications=%u pane=%06lx caption=%06lx\n",
                                  notifications,(unsigned long)GetSysColor(COLOR_WINDOW),(unsigned long)text);
    OutputDebugStringA(diagnostic);
    check(SUCCEEDED(SUCCEEDED(saved)?apply(prior,prior_color,prior_size):apply(NULL,NULL,NULL)),"restore prior theme");
    if (window) DestroyWindow(window);
    FreeLibrary(themes);
    char verdict[180];wsprintfA(verdict,"PS5_PROTON_THEME_NOTIFY_PROBE %s checks=%u failures=%u\n",
                               failures?"FAIL":"PASS",checks,failures);OutputDebugStringA(verdict);
    ExitProcess(failures?1:0);
}
