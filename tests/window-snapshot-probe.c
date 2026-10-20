/* SPDX-License-Identifier: GPL-3.0-or-later
 * Runs under mihawk.exe on the persistent Default desktop. A marked native
 * observer copies server state; the PE compares its own Win32 state with that
 * exact snapshot. This proves metadata, not retained pixels or composition. */
#include <windows.h>
#include <dwmapi.h>
#include "pw_window_snapshot.h"

static unsigned checks, failures;
static DWORD nonce;
static void check(BOOL okay, const char *what)
{
    char line[256];DWORD written;
    ++checks;if (!okay) ++failures;
    wsprintfA(line,"window-snapshot %s: %s error=%lu\n",okay?"PASS":"FAIL",what,GetLastError());
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,lstrlenA(line),&written,NULL);OutputDebugStringA(line);
}
static void pump(DWORD ms)
{
    DWORD start=GetTickCount();
    do { MSG message;while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)) {
        TranslateMessage(&message);DispatchMessageW(&message);
    } Sleep(5); } while(GetTickCount()-start<ms);
}
static BOOL write_file(const char *path,const void *data,DWORD bytes)
{
    char temp[MAX_PATH];DWORD written=0;
    wsprintfA(temp,"%s.tmp",path);
    HANDLE file=CreateFileA(temp,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
    if(file==INVALID_HANDLE_VALUE)return FALSE;
    BOOL okay=WriteFile(file,data,bytes,&written,NULL)&&written==bytes;
    okay=CloseHandle(file)&&okay;
    return okay && MoveFileExA(temp,path,MOVEFILE_REPLACE_EXISTING);
}
static BOOL read_file(const char *path,void *data,DWORD bytes)
{
    DWORD read=0;HANDLE file=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(file==INVALID_HANDLE_VALUE)return FALSE;
    BOOL okay=ReadFile(file,data,bytes,&read,NULL)&&read==bytes;CloseHandle(file);return okay;
}
static BOOL snapshot(PwWindowSnapshot *out)
{
    static PwWindowProbeReply reply;DWORD start=GetTickCount();++nonce;
    if(!write_file("C:\\.pw-window-request",&nonce,sizeof(nonce)))return FALSE;
    do {
        if(read_file("C:\\.pw-window-reply",&reply,sizeof(reply))&&reply.nonce==nonce) {
            if(reply.status||reply.snapshot.version!=PW_WINDOW_SNAPSHOT_VERSION||reply.snapshot.size!=sizeof(*out)||reply.snapshot.count>PW_WINDOW_LIMIT)return FALSE;
            *out=reply.snapshot;return TRUE;
        }
        pump(10);
    }while(GetTickCount()-start<5000);
    return FALSE;
}
static const PwWindowRow *row(const PwWindowSnapshot *snap,HWND window)
{
    for(unsigned i=0;i<snap->count;++i)if(snap->windows[i].hwnd==(uint32_t)(ULONG_PTR)window)return &snap->windows[i];
    return NULL;
}
static unsigned index_of(const PwWindowSnapshot *snap,HWND window)
{
    for(unsigned i=0;i<snap->count;++i)if(snap->windows[i].hwnd==(uint32_t)(ULONG_PTR)window)return i;
    return PW_WINDOW_LIMIT;
}
/* A row is what the window shows: DWM's frame bounds, inside a framed window's invisible resize border (Wine
 * patch 1139), as on Windows 11. */
static void geometry(const PwWindowSnapshot *snap,HWND window,const char *what)
{
    RECT rect={0};DWORD pid=0;const PwWindowRow *item=row(snap,window);
    GetWindowThreadProcessId(window,&pid);
    if(FAILED(DwmGetWindowAttribute(window,DWMWA_EXTENDED_FRAME_BOUNDS,&rect,sizeof(rect))))SetRectEmpty(&rect);
    check(item&&item->windows_pid==pid&&item->pid>0&&item->generation&&
        item->left==rect.left&&item->top==rect.top&&item->right==rect.right&&item->bottom==rect.bottom,what);
}
static LRESULT CALLBACK window_proc(HWND w,UINT m,WPARAM wp,LPARAM lp)
{
    if(m==WM_PAINT) {
        PAINTSTRUCT paint;HDC dc=BeginPaint(w,&paint);HBRUSH brush=CreateSolidBrush(RGB(30,110,210));
        FillRect(dc,&paint.rcPaint,brush);DeleteObject(brush);EndPaint(w,&paint);return 0;
    }
    return DefWindowProcW(w,m,wp,lp);
}
static HWND make_window(DWORD extra,DWORD style,HWND owner,int x,int y)
{
    return CreateWindowExW(extra,L"MihawkWindowSnapshot",L"Window snapshot probe",style,x,y,320,240,owner,NULL,GetModuleHandleW(NULL),NULL);
}
static void cleanup_files(void)
{
    const char *paths[]={"C:\\.pw-window-request","C:\\.pw-window-request.tmp","C:\\.pw-window-reply","C:\\.pw-window-reply.tmp","C:\\.pw-window-child-ready","C:\\.pw-window-child-stop"};
    for(unsigned i=0;i<sizeof(paths)/sizeof(paths[0]);++i)DeleteFileA(paths[i]);
}
void mainCRTStartup(void)
{
    WNDCLASSW type={0};type.lpfnWndProc=window_proc;type.hInstance=GetModuleHandleW(NULL);type.lpszClassName=L"MihawkWindowSnapshot";
    if(!RegisterClassW(&type))ExitProcess(2);
    if(wcsstr(GetCommandLineW(),L"--child")) {
        HWND child=make_window(0,WS_OVERLAPPEDWINDOW|WS_VISIBLE,NULL,550,380);
        uint32_t identity=(uint32_t)(ULONG_PTR)child;DWORD start=GetTickCount();
        if(!child||!write_file("C:\\.pw-window-child-ready",&identity,sizeof(identity)))ExitProcess(3);
        while(GetFileAttributesA("C:\\.pw-window-child-stop")==INVALID_FILE_ATTRIBUTES&&GetTickCount()-start<45000)pump(10);
        DestroyWindow(child);ExitProcess(0);
    }
    cleanup_files();
    DWORD shell_start=GetTickCount();
    while(!IsWindow(GetShellWindow())&&GetTickCount()-shell_start<5000)pump(10);
    check(IsWindow(GetShellWindow()),"live persistent shell");
    HWND back=make_window(0,WS_OVERLAPPEDWINDOW|WS_VISIBLE,NULL,200,180);
    HWND front=make_window(0,WS_OVERLAPPEDWINDOW|WS_VISIBLE,NULL,340,270);
    HWND owned=make_window(WS_EX_TOOLWINDOW,WS_POPUP|WS_CAPTION|WS_VISIBLE,front,440,330);
    check(back&&front&&owned,"three windows in one process");
    STARTUPINFOW startup={0};PROCESS_INFORMATION child={0};startup.cb=sizeof(startup);
    WCHAR command[128];lstrcpyW(command,sizeof(void*)==8?L"C:\\window-snapshot32-probe.exe --child":L"C:\\window-snapshot64-probe.exe --child");
    BOOL started=CreateProcessW(NULL,command,NULL,NULL,FALSE,0,NULL,NULL,&startup,&child);
    check(started,"opposite architecture child");
    uint32_t child_hwnd=0;DWORD wait_start=GetTickCount();
    while(!read_file("C:\\.pw-window-child-ready",&child_hwnd,sizeof(child_hwnd))&&GetTickCount()-wait_start<10000)pump(10);
    HWND other=(HWND)(ULONG_PTR)child_hwnd;
    check(child_hwnd&&IsWindow(other),"child window ready");
    static PwWindowSnapshot snap;check(snapshot(&snap),"coherent versioned server snapshot");
    geometry(&snap,back,"parent geometry and authoritative identity");geometry(&snap,other,"child geometry and authoritative identity");
    const PwWindowRow *a=row(&snap,back),*b=row(&snap,other),*o=row(&snap,owned);
    check(a&&b&&a->pid!=b->pid,"native process ownership differs");
    check(o&&o->owner==(uint32_t)(ULONG_PTR)front,"owned tool window retains owner HWND");
    check(snap.count&&snap.windows[snap.count-1].hwnd==(uint32_t)(ULONG_PTR)GetDesktopWindow()&&
        (snap.windows[snap.count-1].flags&PW_WINDOW_DESKTOP),"desktop root last in authoritative order");
    uint64_t old_generation=a?a->generation:0,old_sequence=snap.sequence;
    SetWindowPos(back,HWND_TOP,180,170,360,260,SWP_NOACTIVATE);pump(20);
    check(snapshot(&snap)&&snap.sequence>old_sequence,"position mutation publishes a new sequence");
    geometry(&snap,back,"move and resize geometry");
    check(index_of(&snap,back)<index_of(&snap,other),"cross-process topmost-first order");
    ShowWindow(back,SW_HIDE);pump(20);check(snapshot(&snap)&&row(&snap,back)&&!(row(&snap,back)->flags&PW_WINDOW_VISIBLE),"hide retains identity but clears visibility");
    ShowWindow(back,SW_SHOW);ShowWindow(back,SW_MINIMIZE);pump(20);
    check(snapshot(&snap)&&row(&snap,back)&&!(row(&snap,back)->flags&PW_WINDOW_VISIBLE),"minimized window is not composed");
    ShowWindow(back,SW_RESTORE);pump(20);check(snapshot(&snap)&&row(&snap,back)&&(row(&snap,back)->flags&PW_WINDOW_VISIBLE),"restore makes window visible");
    SetWindowRgn(back,CreateRectRgn(0,0,160,120),TRUE);pump(20);
    check(snapshot(&snap)&&row(&snap,back)&&(row(&snap,back)->flags&PW_WINDOW_SHAPED)&&row(&snap,back)->shape_count&&!(row(&snap,back)->flags&PW_WINDOW_UNSUPPORTED),"shaped content has authoritative clip rectangles");
    SetWindowRgn(back,NULL,TRUE);pump(20);check(snapshot(&snap)&&row(&snap,back)&&!(row(&snap,back)->flags&PW_WINDOW_SHAPED),"region removal clears shaped flag");
    DestroyWindow(back);pump(20);check(snapshot(&snap)&&!row(&snap,back),"destroy removes stale HWND row");
    HWND replacement=make_window(0,WS_OVERLAPPEDWINDOW|WS_VISIBLE,NULL,230,200);pump(20);
    check(snapshot(&snap)&&row(&snap,replacement)&&row(&snap,replacement)->generation>old_generation,"new lifetime cannot reuse stale generation");
    if(started) {
        write_file("C:\\.pw-window-child-stop",&nonce,sizeof(nonce));
        DWORD waited=WaitForSingleObject(child.hProcess,10000),code=~0u;GetExitCodeProcess(child.hProcess,&code);
        check(waited==WAIT_OBJECT_0&&code==0,"child exits normally");
        if(waited!=WAIT_OBJECT_0)TerminateProcess(child.hProcess,4);
        CloseHandle(child.hThread);CloseHandle(child.hProcess);
    }
    pump(20);check(snapshot(&snap)&&!row(&snap,other)&&row(&snap,replacement)&&IsWindow(GetShellWindow()),"child exit removes its row and preserves parent and shell");
    HWND many[130]={0};for(unsigned i=0;i<130;++i)many[i]=make_window(0,WS_OVERLAPPEDWINDOW,NULL,0,0);
    check(snapshot(&snap)&&snap.overflow&&snap.count==PW_WINDOW_LIMIT,"bounded snapshot reports overflow");
    for(unsigned i=0;i<130;++i)if(many[i])DestroyWindow(many[i]);
    pump(20);check(snapshot(&snap)&&!snap.overflow,"overflow clears after destruction");
    DestroyWindow(owned);DestroyWindow(front);DestroyWindow(replacement);
    HWND desktop=GetShellWindow();cleanup_files();
    char line[160];DWORD written;wsprintfA(line,"PS5_PROTON_WINDOW_SNAPSHOT %s checks=%u failures=%u\n",failures?"FAIL":"PASS",checks,failures);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,lstrlenA(line),&written,NULL);OutputDebugStringA(line);
    if(IsWindow(desktop))PostMessageW(desktop,WM_CLOSE,0,0);
    ExitProcess(failures?1:0);
}
