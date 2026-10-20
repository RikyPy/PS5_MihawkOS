/* SPDX-License-Identifier: GPL-3.0-or-later
 * DWM surfaces on the actual composed display: rounded corners, borders,
 * shadows, acrylic/Mica glass and live thumbnails, plus the API, server and
 * uxtheme glass-painting semantics behind them. Runs under mihawk.exe with
 * WINE_PS5_WINDOW_SURFACES=1 and the marked snapshot rendezvous. The
 * opposite-architecture child owns the thumbnail source. Expected effect
 * pixels are computed by tools/run-window-effects.py from an independent
 * reference; this program logs geometry and phases and checks APIs. */
#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <stdint.h>
#include "pw_window_snapshot.h"

/* Windows 11 values absent from the pinned host headers. */
enum { PW_DWMWCP_DEFAULT, PW_DWMWCP_DONOTROUND, PW_DWMWCP_ROUND, PW_DWMWCP_ROUNDSMALL };
enum { PW_DWMSBT_AUTO, PW_DWMSBT_NONE, PW_DWMSBT_MAINWINDOW, PW_DWMSBT_TRANSIENTWINDOW, PW_DWMSBT_TABBEDWINDOW,
       /* the shell's own, past Windows' values (Wine patches 1141 and 1160): taskbar, flyout base, flyout */
       PW_DWMSBT_TASKBAR, PW_DWMSBT_FLYOUT_BASE, PW_DWMSBT_FLYOUT, PW_DWMSBT_PAST_PRIVATE };
#define PW_DWMWA_COLOR_NONE 0xFFFFFFFEu
#define PW_DWMWA_COLOR_DEFAULT 0xFFFFFFFFu
#define PW_BPPF_ERASE 1
typedef HRESULT (WINAPI *query_size_fn)(HTHUMBNAIL,SIZE *);

static unsigned checks,failures,captures;
static DWORD nonce;
static const COLORREF back_colour=RGB(40,90,160),red=RGB(220,50,60),green=RGB(30,180,70),yellow=RGB(240,180,30);
static const COLORREF grey=RGB(128,128,128),orange=RGB(250,130,20),cyan=RGB(20,200,220),purple=RGB(150,40,190);
static const COLORREF border_colour=RGB(0,120,215);
static void say(const char *line)
{
    DWORD written;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,lstrlenA(line),&written,NULL);OutputDebugStringA(line);
}
static void check(BOOL okay,const char *what)
{
    char line[256];++checks;if(!okay)++failures;
    wsprintfA(line,"window-effects %s: %s error=%lu\n",okay?"PASS":"FAIL",what,GetLastError());say(line);
}
static void check_hr(HRESULT hr,HRESULT expected,const char *what)
{
    char line[256];wsprintfA(line,"%s (hr=%#lx expected=%#lx)",what,(unsigned long)hr,(unsigned long)expected);
    check(hr==expected,line);
}
static void pump(DWORD ms)
{
    DWORD start=GetTickCount();
    do {MSG m;while(PeekMessageW(&m,NULL,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(5);}while(GetTickCount()-start<ms);
}
static BOOL write_file(const char *path,const void *data,DWORD bytes)
{
    char tmp[MAX_PATH];DWORD written;wsprintfA(tmp,"%s.tmp",path);
    HANDLE f=CreateFileA(tmp,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);if(f==INVALID_HANDLE_VALUE)return FALSE;
    BOOL okay=WriteFile(f,data,bytes,&written,NULL)&&written==bytes;okay=CloseHandle(f)&&okay;
    return okay&&MoveFileExA(tmp,path,MOVEFILE_REPLACE_EXISTING);
}
static BOOL read_file(const char *path,void *data,DWORD bytes)
{
    DWORD read;HANDLE f=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(f==INVALID_HANDLE_VALUE)return FALSE;BOOL okay=ReadFile(f,data,bytes,&read,NULL)&&read==bytes;CloseHandle(f);return okay;
}
static BOOL snapshot(PwWindowSnapshot *out)
{
    static PwWindowProbeReply reply;DWORD start=GetTickCount();++nonce;
    pump(30);
    if(!write_file("C:\\.pw-window-request",&nonce,sizeof(nonce)))return FALSE;
    do {
        if(read_file("C:\\.pw-window-reply",&reply,sizeof(reply))&&reply.nonce==nonce) {
            if(reply.status||reply.snapshot.version!=PW_WINDOW_SNAPSHOT_VERSION||reply.snapshot.size!=sizeof(*out))return FALSE;
            *out=reply.snapshot;return TRUE;
        }
        pump(10);
    }while(GetTickCount()-start<5000);
    return FALSE;
}
static const PwWindowRow *row(const PwWindowSnapshot *s,HWND window)
{
    for(unsigned i=0;i<s->count;++i)if(s->windows[i].hwnd==(uint32_t)(ULONG_PTR)window)return &s->windows[i];
    return NULL;
}
static const PwWindowThumbnail *thumbnail_row(const PwWindowSnapshot *s,HWND destination,HWND source)
{
    for(unsigned i=0;i<s->thumbnail_count;++i)
        if(s->thumbnails[i].destination==(uint32_t)(ULONG_PTR)destination && s->thumbnails[i].source==(uint32_t)(ULONG_PTR)source)
            return &s->thumbnails[i];
    return NULL;
}
static void log_rect(const char *name,HWND window)
{
    RECT r={0},c={0};POINT origin={0,0};char line[200];
    /* what the window shows (DWM's frame bounds: inside a framed window's invisible resize border) */
    if(FAILED(DwmGetWindowAttribute(window,DWMWA_EXTENDED_FRAME_BOUNDS,&r,sizeof(r))))GetWindowRect(window,&r);
    GetClientRect(window,&c);ClientToScreen(window,&origin);
    wsprintfA(line,"window-effects rect name=%s window=%d,%d,%d,%d client=%d,%d,%d,%d\n",name,(int)r.left,(int)r.top,(int)r.right,(int)r.bottom,
              (int)origin.x,(int)origin.y,(int)(origin.x+c.right),(int)(origin.y+c.bottom));
    say(line);
}

/* Glass windows paint with the documented buffered-paint recipe: clear to
 * transparent, then make the opaque part's alpha 255. */
static LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM w,LPARAM l)
{
    if(msg==WM_PAINT) {
        PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);RECT client;GetClientRect(hwnd,&client);
        if(GetPropW(hwnd,L"Glass")) {
            BP_PAINTPARAMS params={sizeof(params),PW_BPPF_ERASE,NULL,NULL};HDC buffer=NULL;
            HPAINTBUFFER paint=BeginBufferedPaint(dc,&client,BPBF_TOPDOWNDIB,&params,&buffer);
            if(paint) {
                if(GetPropW(hwnd,L"Solid")) {
                    RECT solid={50,50,100,100};HBRUSH white=CreateSolidBrush(RGB(255,255,255));
                    FillRect(buffer,&solid,white);DeleteObject(white);BufferedPaintSetAlpha(paint,&solid,255);
                }
                EndBufferedPaint(paint,TRUE);
            }
        } else {
            HBRUSH brush=CreateSolidBrush((COLORREF)GetWindowLongPtrW(hwnd,GWLP_USERDATA));FillRect(dc,&ps.rcPaint,brush);DeleteObject(brush);
            if(GetPropW(hwnd,L"Split")) {
                RECT right=client;right.left=right.right/2;
                brush=CreateSolidBrush((COLORREF)(ULONG_PTR)GetPropW(hwnd,L"Split"));FillRect(dc,&right,brush);DeleteObject(brush);
            }
        }
        EndPaint(hwnd,&ps);return 0;
    }
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_LBUTTONDOWN) {SetPropW(hwnd,L"Clicks",(HANDLE)((ULONG_PTR)GetPropW(hwnd,L"Clicks")+1));return 0;}
    if(msg==WM_APP+1)return (LRESULT)GetPropW(hwnd,L"Clicks");
    return DefWindowProcW(hwnd,msg,w,l);
}
static HWND window(const WCHAR *type,DWORD style,COLORREF colour,int x,int y,int right,int bottom)
{
    HWND hwnd=CreateWindowExW(0,type,L"Window effects probe",style,x,y,right-x,bottom-y,NULL,NULL,GetModuleHandleW(NULL),NULL);
    if(hwnd) {SetWindowLongPtrW(hwnd,GWLP_USERDATA,colour);ShowWindow(hwnd,SW_SHOWNOACTIVATE);InvalidateRect(hwnd,NULL,TRUE);UpdateWindow(hwnd);}
    return hwnd;
}
static HRESULT set_dword(HWND hwnd,DWORD attribute,DWORD value)
{
    return DwmSetWindowAttribute(hwnd,attribute,&value,sizeof(value));
}
static BOOL click(HWND hwnd,int x,int y)
{
    DWORD before=(DWORD)SendMessageW(hwnd,WM_APP+1,0,0);INPUT input[3]={{0}};
    input[0].type=input[1].type=input[2].type=INPUT_MOUSE;
    input[0].mi.dx=x*65535/1919;input[0].mi.dy=y*65535/1079;input[0].mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE;
    input[1].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;input[2].mi.dwFlags=MOUSEEVENTF_LEFTUP;
    if(SendInput(3,input,sizeof(INPUT))!=3)return FALSE;
    DWORD start=GetTickCount();while(GetTickCount()-start<3000){pump(10);if((DWORD)SendMessageW(hwnd,WM_APP+1,0,0)==before+1)return TRUE;}
    return FALSE;
}
static char last_capture[MAX_PATH];
/* Ask the broker for the next displayed frame; the host fetches and judges it. */
static BOOL capture(const char *phase)
{
    const char *base="Z:\\data\\ps5proton\\logs\\game-frame";char request[MAX_PATH],*path=last_capture,line[160];
    DWORD start=GetTickCount();
    wsprintfA(request,"%s.ppm.broker-request",base);wsprintfA(path,"%s-%02u.ppm",base,++captures);
    SetCursorPos(1880+(captures&1),1060); /* a pointer that moves gives the broker a frame to show and save */
    DeleteFileA(path);pump(150);
    if(!write_file(request,"",0))return FALSE;
    while(GetTickCount()-start<10000) {
        HANDLE file=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
        if(file!=INVALID_HANDLE_VALUE) {
            const DWORD size=GetFileSize(file,NULL);CloseHandle(file);
            if(size==17+1920*1080*3) {
                wsprintfA(line,"window-effects capture=%u phase=%s\n",captures,phase);say(line);return TRUE;
            }
        }
        pump(20);
    }
    return FALSE;
}
/* A frame still one flat colour on a 40x30 grid: Explorer paints the session wallpaper some time after the shell
 * window exists, and a root taken before that (white but the icons, gate-hang-a-2026-10-15-01 window-effects64)
 * gave Mica's reference the wrong desktop. */
static BOOL flat_frame(const char *path)
{
    static BYTE row[1920*3];
    HANDLE file=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    unsigned samples=0,other=0;int first[3]={0,0,0};DWORD got;
    if(file==INVALID_HANDLE_VALUE)return FALSE;
    for(unsigned y=0;y<1080;y+=36) {
        LARGE_INTEGER at;at.QuadPart=17+(LONGLONG)y*sizeof(row);
        if(!SetFilePointerEx(file,at,NULL,FILE_BEGIN)||!ReadFile(file,row,sizeof(row),&got,NULL)||got!=sizeof(row))break;
        for(unsigned x=0;x<1920;x+=48) {
            const BYTE *p=row+x*3;int d=0;
            if(!samples++) {first[0]=p[0];first[1]=p[1];first[2]=p[2];continue;}
            for(int c=0;c<3;++c)d+=p[c]>first[c]?p[c]-first[c]:first[c]-p[c];
            if(d>24)++other;
        }
    }
    CloseHandle(file);return samples>1&&other*20<samples;
}

/* uxtheme glass painting, checked through the buffer's own bits. */
static void buffered_paint_checks(HWND owner)
{
    BITMAPINFO info={{sizeof(info.bmiHeader),64,-32,1,32,BI_RGB}};
    DWORD *target_bits=NULL;HDC target=CreateCompatibleDC(NULL);
    HBITMAP bitmap=CreateDIBSection(target,&info,DIB_RGB_COLORS,(void **)&target_bits,NULL,0),old=NULL;
    check(target&&bitmap&&target_bits,"32-bit target DIB");
    if(!target||!bitmap||!target_bits)return;
    old=SelectObject(target,bitmap);
    for(unsigned i=0;i<64*32;++i)target_bits[i]=0xff0000ff;   /* opaque red, BGRA */
    RECT all={0,0,64,32},sub={8,4,24,12},excluded={40,0,64,32};
    BP_PAINTPARAMS params={sizeof(params),PW_BPPF_ERASE,NULL,NULL};HDC dc=NULL;
    HPAINTBUFFER paint=BeginBufferedPaint(target,&all,BPBF_TOPDOWNDIB,&params,&dc);
    RGBQUAD *bits=NULL;int width=0;
    check(paint&&dc&&GetBufferedPaintBits(paint,&bits,&width)==S_OK&&bits&&width==64,"top-down buffered paint");
    if(paint&&bits) {
        DWORD *pixels=(DWORD *)bits;BOOL erased=TRUE,gdi_zero=TRUE,alpha=TRUE,cleared=TRUE;
        for(unsigned i=0;i<64*32;++i)erased=erased&&!pixels[i];
        check(erased,"BPPF_ERASE leaves ARGB 0");
        HBRUSH brush=CreateSolidBrush(RGB(10,20,30));FillRect(dc,&all,brush);DeleteObject(brush);GdiFlush();
        for(unsigned i=0;i<64*32;++i)gdi_zero=gdi_zero&&pixels[i]==0x000a141e;
        check(gdi_zero,"GDI writes colour with zero alpha");
        check(BufferedPaintSetAlpha(paint,&sub,255)==S_OK,"BufferedPaintSetAlpha succeeds");
        for(int y=0;y<32;++y)for(int x=0;x<64;++x) {
            BOOL inside=x>=sub.left&&x<sub.right&&y>=sub.top&&y<sub.bottom;
            alpha=alpha&&(pixels[y*64+x]>>24)==(inside?255u:0u)&&(pixels[y*64+x]&0xffffff)==0x0a141e;
        }
        check(alpha,"alpha set only inside the rectangle, colour untouched");
        RECT clear={8,4,16,8};check(BufferedPaintClear(paint,&clear)==S_OK,"BufferedPaintClear succeeds");
        for(int y=0;y<32;++y)for(int x=0;x<64;++x) {
            BOOL inside=x>=clear.left&&x<clear.right&&y>=clear.top&&y<clear.bottom;
            if(inside)cleared=cleared&&!pixels[y*64+x];
        }
        check(cleared&&pixels[10*64+20]==0xff0a141e,"clear zeroes only its rectangle");
        check(EndBufferedPaint(paint,TRUE)==S_OK,"EndBufferedPaint copies");
        check(target_bits[10*64+20]==0xff0a141e&&target_bits[0]==0x000a141e,"copy keeps buffer alpha");
    }
    /* Bottom-up DIB: rows are stored in reverse. */
    paint=BeginBufferedPaint(target,&all,BPBF_DIB,&params,&dc);bits=NULL;
    if(paint&&GetBufferedPaintBits(paint,&bits,&width)==S_OK&&bits) {
        RECT top={0,0,64,1};BufferedPaintSetAlpha(paint,&top,255);
        check(((DWORD *)bits)[31*64]>>24==255&&!(((DWORD *)bits)[0]>>24),"bottom-up buffer orientation");
        EndBufferedPaint(paint,FALSE);
    } else check(FALSE,"bottom-up buffered paint");
    /* The excluded rectangle is never updated on the target. */
    for(unsigned i=0;i<64*32;++i)target_bits[i]=0xff0000ff;
    BP_PAINTPARAMS excluding={sizeof(excluding),PW_BPPF_ERASE,&excluded,NULL};
    paint=BeginBufferedPaint(target,&all,BPBF_TOPDOWNDIB,&excluding,&dc);
    if(paint) {
        HBRUSH white=CreateSolidBrush(RGB(255,255,255));FillRect(dc,&all,white);DeleteObject(white);
        EndBufferedPaint(paint,TRUE);
        check((target_bits[10*64+20]&0xffffff)==0xffffff&&(target_bits[10*64+50]&0xffffff)==0x0000ff,"excluded rectangle");
    } else check(FALSE,"excluding buffered paint");
    /* A blend function composes only the opaque part over the target. */
    for(unsigned i=0;i<64*32;++i)target_bits[i]=0xff0000ff;
    BLENDFUNCTION blend={AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    BP_PAINTPARAMS blended={sizeof(blended),PW_BPPF_ERASE,NULL,&blend};
    paint=BeginBufferedPaint(target,&all,BPBF_TOPDOWNDIB,&blended,&dc);
    if(paint) {
        HBRUSH white=CreateSolidBrush(RGB(255,255,255));FillRect(dc,&sub,white);DeleteObject(white);
        BufferedPaintSetAlpha(paint,&sub,255);EndBufferedPaint(paint,TRUE);
        check((target_bits[10*64+20]&0xffffff)==0xffffff&&(target_bits[20*64+20]&0xffffff)==0x0000ff,"blend function");
    } else check(FALSE,"blended buffered paint");
    /* DTT_COMPOSITED: premultiplied coverage only where glyphs are. */
    HTHEME theme=OpenThemeData(owner,L"WINDOW");
    check(theme!=NULL,"open window theme");
    if(theme) {
        unsigned composited=0,plain=0;BOOL premultiplied=TRUE;
        for(unsigned pass=0;pass<2;++pass) {
            paint=BeginBufferedPaint(target,&all,BPBF_TOPDOWNDIB,&params,&dc);bits=NULL;
            if(!paint||GetBufferedPaintBits(paint,&bits,&width)!=S_OK||!bits) {check(FALSE,"text buffer");break;}
            DTTOPTS options={sizeof(options),DTT_TEXTCOLOR|(pass?0:DTT_COMPOSITED)};options.crText=RGB(255,255,255);
            RECT text=all;DrawThemeTextEx(theme,dc,WP_CAPTION,CS_ACTIVE,L"Mihawk",-1,DT_LEFT|DT_TOP|DT_SINGLELINE,&text,&options);GdiFlush();
            for(unsigned i=0;i<64*32;++i) {
                DWORD p=((DWORD *)bits)[i],a=p>>24;
                if(!pass && a){++composited;premultiplied=premultiplied&&(p&0xff)<=a&&((p>>8)&0xff)<=a&&((p>>16)&0xff)<=a;}
                if(pass && (p&0xffffff))++plain;
                if(pass && a)premultiplied=FALSE;
            }
            EndBufferedPaint(paint,FALSE);
        }
        char line[160];wsprintfA(line,"DTT_COMPOSITED premultiplied coverage (pixels=%u plain=%u)",composited,plain);
        check(composited>=20&&plain>=20&&premultiplied,line);
        CloseThemeData(theme);
    }
    SelectObject(target,old);DeleteObject(bitmap);DeleteDC(target);
}

void mainCRTStartup(void)
{
    WNDCLASSW type={0},shadowed={0};
    type.hInstance=shadowed.hInstance=GetModuleHandleW(NULL);type.lpfnWndProc=shadowed.lpfnWndProc=proc;
    type.lpszClassName=L"MihawkWindowEffects";shadowed.lpszClassName=L"MihawkDropShadow";shadowed.style=CS_DROPSHADOW;
    type.hCursor=shadowed.hCursor=LoadCursorW(NULL,(LPCWSTR)IDC_ARROW);
    if(!RegisterClassW(&type)||!RegisterClassW(&shadowed))ExitProcess(2);
    if(wcsstr(GetCommandLineW(),L"--child")) {
        HWND source=window(L"MihawkWindowEffects",WS_POPUP,orange,600,800,1000,1000);
        if(source){SetPropW(source,L"Split",(HANDLE)(ULONG_PTR)cyan);SetWindowPos(source,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            InvalidateRect(source,NULL,TRUE);UpdateWindow(source);}
        uint32_t identity=(uint32_t)(ULONG_PTR)source;
        if(!source||!write_file("C:\\.pw-effects-ready",&identity,sizeof(identity)))ExitProcess(3);
        DWORD start=GetTickCount();BOOL repainted=FALSE;
        while(GetFileAttributesA("C:\\.pw-effects-stop")==INVALID_FILE_ATTRIBUTES&&GetTickCount()-start<120000) {
            if(!repainted&&GetFileAttributesA("C:\\.pw-effects-repaint")!=INVALID_FILE_ATTRIBUTES) {
                SetWindowLongPtrW(source,GWLP_USERDATA,purple);InvalidateRect(source,NULL,TRUE);UpdateWindow(source);
                repainted=write_file("C:\\.pw-effects-repainted","",0);
            }
            pump(10);
        }
        DestroyWindow(source);ExitProcess(0);
    }
    const char *files[]={"C:\\.pw-effects-ready","C:\\.pw-effects-stop","C:\\.pw-effects-repaint","C:\\.pw-effects-repainted",
                         "C:\\.pw-window-request","C:\\.pw-window-reply"};
    for(unsigned i=0;i<sizeof(files)/sizeof(files[0]);++i)DeleteFileA(files[i]);
    DWORD start=GetTickCount();while(!IsWindow(GetShellWindow())&&GetTickCount()-start<5000)pump(10);
    const DWORD shell_wait=GetTickCount()-start;
    HWND shell=GetShellWindow();check(IsWindow(shell),"persistent shell ready");
    BOOL composition=FALSE;check(DwmIsCompositionEnabled(&composition)==S_OK&&composition,"composition reported enabled");

    /* Root capture first: Mica's independent reference is the actual desktop root, taken again while it is still flat,
     * for up to 10 s. Each retake is the next number, as the title numbers every request (reusing the number put every
     * later capture one behind the title's in gate-d3d12-census-2026-10-17-01 window-effects64); the host judges the
     * last capture logged for each phase. */
    DWORD drawn=GetTickCount();BOOL root;unsigned retakes=0;char drawn_line[96];
    while((root=capture("root"))&&flat_frame(last_capture)&&GetTickCount()-drawn<10000){++retakes;pump(250);}
    /* the desktop's time to its wallpaper, as a user waits for it: the shell window, then the first root not flat (each
     * capture is about a second, so this is to within one) */
    wsprintfA(drawn_line,"window-effects startup shell_wait_ms=%lu root_flat_ms=%lu retakes=%u\n",shell_wait,GetTickCount()-drawn,
              retakes);say(drawn_line);
    check(root,"root capture");

    HWND back=window(L"MihawkWindowEffects",WS_POPUP,back_colour,60,60,1860,1020);
    HWND round=window(L"MihawkWindowEffects",WS_POPUP,red,150,150,450,400);
    HWND compact=window(L"MihawkWindowEffects",WS_POPUP,green,550,150,800,300);
    HWND framed=window(L"MihawkWindowEffects",WS_OVERLAPPEDWINDOW,yellow,150,500,550,750);
    HWND dropped=window(L"MihawkDropShadow",WS_POPUP,grey,900,600,1000,700);
    HWND acrylic=window(L"MihawkWindowEffects",WS_POPUP,0,1100,150,1400,400);
    HWND mica=window(L"MihawkWindowEffects",WS_POPUP,0,1100,550,1400,800);
    HWND destination=window(L"MihawkWindowEffects",WS_POPUP,grey,1500,150,1800,400);
    check(back&&round&&compact&&framed&&dropped&&acrylic&&mica&&destination,"probe windows created");

    /* Attribute semantics, then server resolution. */
    check_hr(set_dword(round,DWMWA_WINDOW_CORNER_PREFERENCE,PW_DWMWCP_ROUND),S_OK,"corner preference ROUND");
    check_hr(set_dword(compact,DWMWA_WINDOW_CORNER_PREFERENCE,PW_DWMWCP_ROUNDSMALL),S_OK,"corner preference ROUNDSMALL");
    check_hr(set_dword(round,DWMWA_WINDOW_CORNER_PREFERENCE,4),E_INVALIDARG,"invalid corner preference");
    {DWORD value=PW_DWMWCP_ROUND;check_hr(DwmSetWindowAttribute(round,DWMWA_WINDOW_CORNER_PREFERENCE,&value,2),E_INVALIDARG,"short corner attribute");}
    check_hr(set_dword(NULL,DWMWA_WINDOW_CORNER_PREFERENCE,PW_DWMWCP_ROUND),E_HANDLE,"corner preference on no window");
    check_hr(set_dword(compact,DWMWA_BORDER_COLOR,border_colour),S_OK,"border colour");
    check_hr(set_dword(compact,DWMWA_BORDER_COLOR,0x01000000),E_INVALIDARG,"invalid border colour");
    check_hr(set_dword(round,DWMWA_BORDER_COLOR,PW_DWMWA_COLOR_NONE),S_OK,"no border");
    check_hr(set_dword(acrylic,DWMWA_SYSTEMBACKDROP_TYPE,PW_DWMSBT_TRANSIENTWINDOW),S_OK,"acrylic backdrop");
    check_hr(set_dword(mica,DWMWA_SYSTEMBACKDROP_TYPE,PW_DWMSBT_MAINWINDOW),S_OK,"Mica backdrop");
    check_hr(set_dword(mica,DWMWA_USE_IMMERSIVE_DARK_MODE,TRUE),S_OK,"dark material");
    /* The shell's private backdrops are accepted and read back; the first value past them is refused and leaves the
     * window's as it was. */
    for(DWORD kind=PW_DWMSBT_TASKBAR;kind<PW_DWMSBT_PAST_PRIVATE;++kind) {
        DWORD value=0;char what[64];
        wsprintfA(what,"private backdrop %lu accepted",(unsigned long)kind);check_hr(set_dword(mica,DWMWA_SYSTEMBACKDROP_TYPE,kind),S_OK,what);
        wsprintfA(what,"private backdrop %lu reads back",(unsigned long)kind);
        check(DwmGetWindowAttribute(mica,DWMWA_SYSTEMBACKDROP_TYPE,&value,sizeof(value))==S_OK&&value==kind,what);
    }
    check_hr(set_dword(mica,DWMWA_SYSTEMBACKDROP_TYPE,PW_DWMSBT_PAST_PRIVATE),E_INVALIDARG,"invalid backdrop");
    {DWORD value=0;check(DwmGetWindowAttribute(mica,DWMWA_SYSTEMBACKDROP_TYPE,&value,sizeof(value))==S_OK&&value==PW_DWMSBT_FLYOUT,
                         "an invalid backdrop leaves the window's");}
    check_hr(set_dword(mica,DWMWA_SYSTEMBACKDROP_TYPE,PW_DWMSBT_MAINWINDOW),S_OK,"Mica backdrop again");
    {DWORD value=0;check(DwmGetWindowAttribute(mica,DWMWA_SYSTEMBACKDROP_TYPE,&value,sizeof(value))==S_OK&&value==PW_DWMSBT_MAINWINDOW,"backdrop reads back");
     UINT thickness=0;check(DwmGetWindowAttribute(mica,DWMWA_VISIBLE_FRAME_BORDER_THICKNESS,&thickness,sizeof(thickness))==S_OK&&thickness==1,"visible frame border thickness");}
    MARGINS sheet={-1,-1,-1,-1},inset={10,20,30,40};
    check_hr(DwmExtendFrameIntoClientArea(acrylic,NULL),E_INVALIDARG,"null margins");
    check_hr(DwmExtendFrameIntoClientArea(NULL,&sheet),E_HANDLE,"margins on no window");
    check_hr(DwmExtendFrameIntoClientArea(acrylic,&inset),S_OK,"inset frame margins");
    SetPropW(acrylic,L"Glass",(HANDLE)1);SetPropW(acrylic,L"Solid",(HANDLE)1);SetPropW(mica,L"Glass",(HANDLE)1);
    check_hr(DwmExtendFrameIntoClientArea(mica,&sheet),S_OK,"Mica sheet of glass");
    InvalidateRect(acrylic,NULL,TRUE);InvalidateRect(mica,NULL,TRUE);UpdateWindow(acrylic);UpdateWindow(mica);

    /* Child process: source of thumbnails and cross-process ownership checks. */
    STARTUPINFOW startup={0};PROCESS_INFORMATION child={0};startup.cb=sizeof(startup);WCHAR command[128];
    lstrcpyW(command,sizeof(void*)==8?L"C:\\window-effects32-probe.exe --child":L"C:\\window-effects64-probe.exe --child");
    BOOL started=CreateProcessW(NULL,command,NULL,NULL,FALSE,0,NULL,NULL,&startup,&child);
    uint32_t identity=0;start=GetTickCount();while(!read_file("C:\\.pw-effects-ready",&identity,sizeof(identity))&&GetTickCount()-start<10000)pump(10);
    HWND source=(HWND)(ULONG_PTR)identity;check(started&&identity&&IsWindow(source),"opposite-architecture source window");
    check_hr(set_dword(source,DWMWA_WINDOW_CORNER_PREFERENCE,PW_DWMWCP_ROUND),E_ACCESSDENIED,"another process's window attribute");

    HTHUMBNAIL thumb=NULL,spare=NULL;
    query_size_fn query_size=(query_size_fn)GetProcAddress(GetModuleHandleA("dwmapi.dll"),"DwmQueryThumbnailSourceSize");
    check(query_size!=NULL,"DwmQueryThumbnailSourceSize export");
    check_hr(DwmRegisterThumbnail(destination,source,NULL),E_INVALIDARG,"null thumbnail handle");
    check_hr(DwmRegisterThumbnail(destination,destination,&spare),E_INVALIDARG,"thumbnail of itself");
    check_hr(DwmRegisterThumbnail(source,destination,&spare),E_INVALIDARG,"destination owned by another process");
    check_hr(DwmRegisterThumbnail(destination,(HWND)(ULONG_PTR)0x7ffe,&spare),E_INVALIDARG,"invalid source window");
    check_hr(DwmRegisterThumbnail(destination,source,&thumb),S_OK,"register live thumbnail");
    check(thumb!=NULL,"nonzero thumbnail handle");
    {SIZE size={0};check(query_size&&query_size(thumb,&size)==S_OK&&size.cx==400&&size.cy==200,"thumbnail source size");}
    check_hr(DwmUpdateThumbnailProperties(thumb,NULL),E_INVALIDARG,"null thumbnail properties");
    DWM_THUMBNAIL_PROPERTIES props={0};
    props.dwFlags=DWM_TNP_RECTDESTINATION|DWM_TNP_VISIBLE|DWM_TNP_OPACITY;SetRect(&props.rcDestination,20,20,220,170);
    props.fVisible=TRUE;props.opacity=255;
    check_hr(DwmUpdateThumbnailProperties(thumb,&props),S_OK,"thumbnail visible");
    check_hr(DwmUpdateThumbnailProperties((HTHUMBNAIL)(ULONG_PTR)0x7fffffff,&props),E_INVALIDARG,"unknown thumbnail");

    /* Snapshot: the server's resolution of every attribute and default. */
    static PwWindowSnapshot snap;
    check(snapshot(&snap),"versioned v4 snapshot");
    const PwWindowRow *r=row(&snap,round),*s=row(&snap,compact),*f=row(&snap,framed),*b=row(&snap,back),*d=row(&snap,dropped);
    const PwWindowRow *g=row(&snap,acrylic),*m=row(&snap,mica);
    check(r&&r->radius==8&&r->shadow==PW_SHADOW_WINDOW&&!(r->flags&(PW_WINDOW_BORDER|PW_WINDOW_GLASS)),"ROUND popup: 8 px, window shadow");
    check(s&&s->radius==4&&s->shadow==PW_SHADOW_SMALL&&(s->flags&PW_WINDOW_BORDER)&&s->border==0x0078d7,"ROUNDSMALL popup: 4 px, small shadow, RGB border");
    check(f&&f->radius==8&&f->shadow==PW_SHADOW_WINDOW,"framed window rounds by default");
    check(b&&!b->radius&&!b->shadow,"plain pop-up stays square without shadow");
    check(d&&!d->radius&&d->shadow==PW_SHADOW_SMALL,"CS_DROPSHADOW pop-up: small shadow, square");
    check(g&&(g->flags&PW_WINDOW_GLASS)&&g->backdrop==PW_BACKDROP_ACRYLIC&&g->material==PW_MATERIAL_LIGHT&&
          g->solid_left==g->client_left+10&&g->solid_top==g->client_top+30&&g->solid_right==g->client_right-20&&g->solid_bottom==g->client_bottom-40,
          "acrylic glass with inset margins");
    check(m&&(m->flags&PW_WINDOW_GLASS)&&m->backdrop==PW_BACKDROP_MICA&&m->material==PW_MATERIAL_DARK&&m->solid_left==m->solid_right,"dark Mica sheet of glass");
    {
        /* The server resolves the private backdrops to the compositor's materials, the flyouts with their shadow. */
        static const struct {DWORD kind;unsigned backdrop;const char *what;} privates[]={
            {PW_DWMSBT_TASKBAR,PW_BACKDROP_TASKBAR,"taskbar backdrop resolves"},
            {PW_DWMSBT_FLYOUT_BASE,PW_BACKDROP_FLYOUT_BASE,"flyout base backdrop resolves, with the flyout shadow"},
            {PW_DWMSBT_FLYOUT,PW_BACKDROP_FLYOUT,"flyout backdrop resolves, with the flyout shadow"}};
        for(unsigned i=0;i<sizeof(privates)/sizeof(privates[0]);++i) {
            const PwWindowRow *p;set_dword(mica,DWMWA_SYSTEMBACKDROP_TYPE,privates[i].kind);
            /* a flyout: --shadow-28 and SurfaceStrokeColorFlyout, black at 20% on this dark window */
            check(snapshot(&snap)&&(p=row(&snap,mica))&&(p->flags&PW_WINDOW_GLASS)&&p->backdrop==privates[i].backdrop&&
                  (privates[i].backdrop==PW_BACKDROP_TASKBAR ? p->shadow==PW_SHADOW_NONE && !(p->flags&PW_WINDOW_BORDER) :
                   p->shadow==PW_SHADOW_FLYOUT && (p->flags&PW_WINDOW_BORDER) && p->border==51u<<24),privates[i].what);
        }
        set_dword(mica,DWMWA_SYSTEMBACKDROP_TYPE,PW_DWMSBT_MAINWINDOW);
        check(snapshot(&snap)&&(m=row(&snap,mica))&&m->backdrop==PW_BACKDROP_MICA,"Mica again before the captures");
    }
    const PwWindowThumbnail *t=thumbnail_row(&snap,destination,source);
    {RECT client;POINT origin={0,0};GetClientRect(destination,&client);ClientToScreen(destination,&origin);
     check(t&&t->left==origin.x+20&&t->top==origin.y+20&&t->right==origin.x+220&&t->bottom==origin.y+170&&
           t->source_left==0&&t->source_top==0&&t->source_right==400&&t->source_bottom==200&&t->opacity==255,"thumbnail placement and whole-window source");}
    {
        /* Standard menus (#32768) round small by default, as on Windows 11. */
        HMENU items=CreatePopupMenu();
        HWND menu=CreateWindowExW(WS_EX_TOOLWINDOW,(LPCWSTR)MAKEINTATOM(32768),L"",WS_POPUP,1700,600,80,60,NULL,NULL,GetModuleHandleW(NULL),items);
        const PwWindowRow *u=menu&&snapshot(&snap)?row(&snap,menu):NULL;
        check(u&&u->radius==4&&u->shadow==PW_SHADOW_SMALL,"standard menu window: 4 px, small shadow");
        if(menu)DestroyWindow(menu);
        if(items)DestroyMenu(items);
    }
    check_hr(DwmExtendFrameIntoClientArea(acrylic,&sheet),S_OK,"acrylic sheet of glass");
    check_hr(set_dword(framed,DWMWA_WINDOW_CORNER_PREFERENCE,PW_DWMWCP_DONOTROUND),S_OK,"framed DONOTROUND");
    check(snapshot(&snap)&&(f=row(&snap,framed))&&!f->radius&&f->shadow==PW_SHADOW_WINDOW,"DONOTROUND keeps the frame shadow");
    set_dword(framed,DWMWA_WINDOW_CORNER_PREFERENCE,PW_DWMWCP_DEFAULT);
    ShowWindow(framed,SW_MAXIMIZE);pump(100);
    check(snapshot(&snap)&&(f=row(&snap,framed))&&!f->radius&&!f->shadow,"maximized window is square without shadow");
    ShowWindow(framed,SW_RESTORE);SetWindowPos(framed,HWND_TOP,150,500,400,250,SWP_NOACTIVATE);pump(100);
    check(snapshot(&snap)&&(f=row(&snap,framed))&&f->radius==8,"restored window rounds again");
    HRGN shape=CreateRectRgn(0,0,300,250);BOOL shaped=shape&&SetWindowRgn(round,shape,TRUE);if(!shaped&&shape)DeleteObject(shape);
    check(shaped&&snapshot(&snap)&&(r=row(&snap,round))&&!r->radius&&!r->shadow,"window region is never rounded");
    SetWindowRgn(round,NULL,TRUE);
    SetWindowLongPtrW(round,GWL_EXSTYLE,GetWindowLongPtrW(round,GWL_EXSTYLE)|WS_EX_LAYERED);SetLayeredWindowAttributes(round,0,255,LWA_ALPHA);
    check(snapshot(&snap)&&(r=row(&snap,round))&&!r->radius,"layered window is never rounded");
    SetWindowLongPtrW(round,GWL_EXSTYLE,GetWindowLongPtrW(round,GWL_EXSTYLE)&~WS_EX_LAYERED);
    SetWindowPos(round,HWND_TOP,150,150,300,250,SWP_FRAMECHANGED|SWP_NOACTIVATE);InvalidateRect(round,NULL,TRUE);UpdateWindow(round);
    check(snapshot(&snap)&&(r=row(&snap,round))&&r->radius==8,"ROUND restored after layering");

    /* Hit-testing is unchanged: corners, glass and thumbnails keep their owner. */
    SetWindowPos(back,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    check(click(round,150,150),"rounded-off corner still hits its window");
    check(click(acrylic,1300,300),"transparent glass still hits its window");
    check(click(destination,1560,240),"thumbnail area hits its destination window");

    buffered_paint_checks(round);

    const HWND logged[]={back,round,compact,framed,dropped,acrylic,mica,destination,source};
    const char *names[]={"back","round","small","framed","dropped","acrylic","mica","destination","source"};
    for(unsigned i=0;i<9;++i)log_rect(names[i],logged[i]);
    {
        /* The foreground's active window casts the active shadow: the snapshot marks it, and no other. */
        const HWND active=GetForegroundWindow();
        const char *name="none";
        BOOL marked=snapshot(&snap);
        for(unsigned i=0;i<9;++i) {
            const PwWindowRow *w=row(&snap,logged[i]);
            if(logged[i]==active)name=names[i];
            marked=marked&&w&&!!(w->flags&PW_WINDOW_ACTIVE)==(logged[i]==active);
        }
        check(marked,"the snapshot marks the active window, and only it");
        char line[64];wsprintfA(line,"window-effects active name=%s\n",name);say(line);
    }
    {
        /* the colour win32u paints the framed window's frame in, under the compositor's 1 px default border */
        const DWORD frame=GetSysColor(GetForegroundWindow()==framed?COLOR_ACTIVECAPTION:COLOR_INACTIVECAPTION);
        char line[96];wsprintfA(line,"window-effects frame name=framed colour=%u,%u,%u\n",GetRValue(frame),GetGValue(frame),GetBValue(frame));
        say(line);
    }
    pump(200);
    check(capture("effects"),"corners, borders, shadows, glass and thumbnail capture");
    props.dwFlags=DWM_TNP_OPACITY;props.opacity=128;
    check_hr(DwmUpdateThumbnailProperties(thumb,&props),S_OK,"thumbnail half opacity");
    check(capture("thumbnail-half"),"half-opacity thumbnail capture");
    props.opacity=255;DwmUpdateThumbnailProperties(thumb,&props);
    write_file("C:\\.pw-effects-repaint","",0);start=GetTickCount();
    while(GetFileAttributesA("C:\\.pw-effects-repainted")==INVALID_FILE_ATTRIBUTES&&GetTickCount()-start<5000)pump(10);
    check(capture("thumbnail-live"),"live source repaint capture");
    props.dwFlags=DWM_TNP_VISIBLE;props.fVisible=FALSE;
    check_hr(DwmUpdateThumbnailProperties(thumb,&props),S_OK,"thumbnail hidden");
    check(snapshot(&snap)&&!thumbnail_row(&snap,destination,source),"hidden thumbnail is not exported");
    check(capture("thumbnail-hidden"),"hidden thumbnail capture");
    set_dword(acrylic,DWMWA_USE_IMMERSIVE_DARK_MODE,TRUE);InvalidateRect(acrylic,NULL,TRUE);UpdateWindow(acrylic);
    check(capture("acrylic-dark"),"dark acrylic capture");

    /* Lifetimes: unregister, the bounded table, destination destruction. */
    check_hr(DwmUnregisterThumbnail(thumb),S_OK,"unregister thumbnail");
    check_hr(DwmUnregisterThumbnail(thumb),E_INVALIDARG,"unregister twice");
    check_hr(DwmUpdateThumbnailProperties(thumb,&props),E_INVALIDARG,"update after unregister");
    HTHUMBNAIL many[40]={0};unsigned registered=0;HRESULT last=S_OK;
    for(unsigned i=0;i<40&&last==S_OK;++i)if((last=DwmRegisterThumbnail(destination,source,&many[i]))==S_OK)++registered;
    check(last==E_OUTOFMEMORY&&registered>0&&registered<=PW_WINDOW_THUMBNAIL_LIMIT,"thumbnail table is bounded");
    BOOL released=TRUE;for(unsigned i=0;i<registered;++i)released=released&&DwmUnregisterThumbnail(many[i])==S_OK;
    check(released,"bounded thumbnails unregister");
    check_hr(DwmRegisterThumbnail(destination,source,&spare),S_OK,"register before destination destruction");
    props.dwFlags=DWM_TNP_RECTDESTINATION|DWM_TNP_VISIBLE;props.fVisible=TRUE;DwmUpdateThumbnailProperties(spare,&props);
    DestroyWindow(destination);pump(50);
    check_hr(DwmUnregisterThumbnail(spare),E_INVALIDARG,"destination destruction releases its thumbnail");
    check(snapshot(&snap)&&!snap.thumbnail_count,"no thumbnail outlives its destination");

    write_file("C:\\.pw-effects-stop","",0);
    DWORD wait=started?WaitForSingleObject(child.hProcess,10000):WAIT_FAILED,code=1;
    BOOL clean=wait==WAIT_OBJECT_0&&GetExitCodeProcess(child.hProcess,&code)&&!code;
    check(clean,"child exits normally");
    HWND windows[]={back,round,compact,framed,dropped,acrylic,mica};
    for(unsigned i=0;i<7;++i)if(windows[i])DestroyWindow(windows[i]);
    if(started){CloseHandle(child.hThread);CloseHandle(child.hProcess);}
    for(unsigned i=0;i<sizeof(files)/sizeof(files[0]);++i)DeleteFileA(files[i]);
    check(IsWindow(shell),"shell survives");
    char line[200];wsprintfA(line,"PS5_PROTON_WINDOW_EFFECTS %s checks=%u failures=%u captures=%u\n",failures?"FAIL":"PASS",checks,failures,captures);
    say(line);
    if(clean&&IsWindow(shell))PostMessageW(shell,WM_CLOSE,0,0);
    ExitProcess(failures?1:0);
}
