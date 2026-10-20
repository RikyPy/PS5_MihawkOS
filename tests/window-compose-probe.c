/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual broker-displayed pixels and hardware-queue input, not API success
 * alone. Run under mihawk.exe on Default. Independent GDI HWNDs from PE32 and
 * PE64 deliberately interleave. Captures use the existing numbered broker
 * request; no process image masquerades as desktop background. */
#include <windows.h>
#include <stdint.h>
#include <string.h>
static unsigned checks,failures,captures;
static const COLORREF blue=RGB(30,110,210),green=RGB(30,180,70),red=RGB(220,50,60),yellow=RGB(240,180,30);
static void check(BOOL okay,const char *what)
{
    char line[256];DWORD written;++checks;if(!okay)++failures;
    wsprintfA(line,"window-compose %s: %s error=%lu\n",okay?"PASS":"FAIL",what,GetLastError());
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,lstrlenA(line),&written,NULL);OutputDebugStringA(line);
}
static void pump(DWORD ms)
{
    DWORD start=GetTickCount();do {MSG m;while(PeekMessageW(&m,NULL,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(5);}while(GetTickCount()-start<ms);
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
static LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM w,LPARAM l)
{
    if(msg==WM_PAINT) {PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);HBRUSH brush=CreateSolidBrush((COLORREF)GetWindowLongPtrW(hwnd,GWLP_USERDATA));FillRect(dc,&ps.rcPaint,brush);DeleteObject(brush);
        if(GetPropW(hwnd,L"Split")) {RECT right;GetClientRect(hwnd,&right);right.left=right.right/2;
            brush=CreateSolidBrush(yellow);FillRect(dc,&right,brush);DeleteObject(brush);}
        EndPaint(hwnd,&ps);return 0;}
    if(msg==WM_LBUTTONDOWN) {SetPropW(hwnd,L"Clicks",(HANDLE)((ULONG_PTR)GetPropW(hwnd,L"Clicks")+1));return 0;}
    if(msg==WM_APP+1)return (LRESULT)GetPropW(hwnd,L"Clicks");
    return DefWindowProcW(hwnd,msg,w,l);
}
static HWND window(COLORREF color,HWND owner,int x,int y,int width,int height)
{
    HWND hwnd=CreateWindowExW(0,L"MihawkWindowCompose",L"Window composition probe",WS_POPUP, x,y,width,height,owner,NULL,GetModuleHandleW(NULL),NULL);
    if(hwnd){SetWindowLongPtrW(hwnd,GWLP_USERDATA,color);ShowWindow(hwnd,SW_SHOW);InvalidateRect(hwnd,NULL,TRUE);UpdateWindow(hwnd);}return hwnd;
}
static BOOL click(HWND hwnd,int x,int y)
{
    DWORD before=(DWORD)SendMessageW(hwnd,WM_APP+1,0,0);INPUT input[3]={{0}};
    input[0].type=input[1].type=input[2].type=INPUT_MOUSE;
    input[0].mi.dx=x*65535/1919;input[0].mi.dy=y*65535/1079;input[0].mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE;
    input[1].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;input[2].mi.dwFlags=MOUSEEVENTF_LEFTUP;
    if(SendInput(3,input,sizeof(INPUT))!=3)return FALSE;
    DWORD start=GetTickCount();while(GetTickCount()-start<3000){pump(10);if((DWORD)SendMessageW(hwnd,WM_APP+1,0,0)==before+1)return TRUE;}return FALSE;
}
struct Sample {int x,y;COLORREF color;};
static BOOL capture(const char *phase,const struct Sample *samples,unsigned count)
{
    const char *base="Z:\\data\\ps5proton\\logs\\game-frame";char request[MAX_PATH],path[MAX_PATH];
    DWORD start=GetTickCount();HANDLE file=INVALID_HANDLE_VALUE;char header[32]={0};DWORD read=0;
    wsprintfA(request,"%s.ppm.broker-request",base);wsprintfA(path,"%s-%02u.ppm",base,++captures);
    /* Keep the real cursor outside sampled rectangles after input checks. */
    POINT pointer={0};
    check(SetCursorPos(1800,100) && GetCursorPos(&pointer) && pointer.x==1800 && pointer.y==100,
          "application cursor position before actual capture");
    {
        COLORREF color=GetSysColor(COLOR_DESKTOP);char line[160];DWORD written;
        wsprintfA(line,"window-compose desktop_colour=%u,%u,%u\n",GetRValue(color),GetGValue(color),GetBValue(color));
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,lstrlenA(line),&written,NULL);OutputDebugStringA(line);
    }
    DeleteFileA(path);pump(100);
    if(!write_file(request,"",0))return FALSE;
    while(GetTickCount()-start<10000) {
        file=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
        if(file!=INVALID_HANDLE_VALUE) {
            if(ReadFile(file,header,18,&read,NULL)&&read>=17) {
                const char *expected="P6\n1920 1080\n255\n";DWORD bytes=lstrlenA(expected);
                if(!memcmp(header,expected,bytes)&&GetFileSize(file,NULL)==bytes+1920*1080*3) {
                    BOOL okay=TRUE;
                    const struct Sample cursor_sample={1801,102,RGB(255,255,255)};
                    for(unsigned i=0;i<count+1;++i) {
                        const struct Sample *sample=i<count ? &samples[i] : &cursor_sample;
                        unsigned char pixel[3];DWORD offset=bytes+(sample->y*1920+sample->x)*3;
                        SetFilePointer(file,offset,NULL,FILE_BEGIN);
                        BOOL match=ReadFile(file,pixel,3,&read,NULL)&&read==3&&
                            pixel[0]==GetRValue(sample->color)&&pixel[1]==GetGValue(sample->color)&&pixel[2]==GetBValue(sample->color);
                        char line[256];DWORD written;wsprintfA(line,"window-compose capture=%u phase=%s sample=%d,%d expected=%u,%u,%u result=%s\n",captures,phase,sample->x,sample->y,GetRValue(sample->color),GetGValue(sample->color),GetBValue(sample->color),match?"PASS":"FAIL");
                        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,lstrlenA(line),&written,NULL);OutputDebugStringA(line);okay=match&&okay;
                    }
                    CloseHandle(file);return okay;
                }
            }
            CloseHandle(file);
        }
        pump(20);
    }
    return FALSE;
}
void mainCRTStartup(void)
{
    WNDCLASSW type={0};type.hInstance=GetModuleHandleW(NULL);type.lpfnWndProc=proc;type.lpszClassName=L"MihawkWindowCompose";
    if(!RegisterClassW(&type))ExitProcess(2);
    if(wcsstr(GetCommandLineW(),L"--child")) {
        HWND h=window(green,NULL,400,250,700,500);uint32_t identity=(uint32_t)(ULONG_PTR)h;
        if(!h||!write_file("C:\\.pw-compose-ready",&identity,sizeof(identity)))ExitProcess(3);
        DWORD start=GetTickCount();while(GetFileAttributesA("C:\\.pw-compose-stop")==INVALID_FILE_ATTRIBUTES&&GetTickCount()-start<60000)pump(10);
        DestroyWindow(h);ExitProcess(0);
    }
    DeleteFileA("C:\\.pw-compose-ready");DeleteFileA("C:\\.pw-compose-stop");
    DWORD start=GetTickCount();while(!IsWindow(GetShellWindow())&&GetTickCount()-start<5000)pump(10);
    HWND shell=GetShellWindow();check(IsWindow(shell),"persistent shell ready");
    HWND back=window(blue,NULL,200,150,700,500),front=window(red,NULL,600,350,400,350);
    STARTUPINFOW startup={0};PROCESS_INFORMATION child={0};startup.cb=sizeof(startup);WCHAR command[128];
    lstrcpyW(command,sizeof(void*)==8?L"C:\\window-compose32-probe.exe --child":L"C:\\window-compose64-probe.exe --child");
    BOOL started=CreateProcessW(NULL,command,NULL,NULL,FALSE,0,NULL,NULL,&startup,&child);check(started&&back&&front,"mixed-architecture independent windows");
    uint32_t identity=0;start=GetTickCount();while(!read_file("C:\\.pw-compose-ready",&identity,sizeof(identity))&&GetTickCount()-start<10000)pump(10);
    HWND other=(HWND)(ULONG_PTR)identity;check(identity&&IsWindow(other),"opposite-architecture HWND ready");
    if(identity&&back&&front) {
        SetWindowPos(back,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        SetWindowPos(other,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        SetWindowPos(front,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);SetForegroundWindow(front);pump(100);
        struct Sample first[]={{300,200,blue},{500,300,green},{650,400,red}};
        check(capture("interleaved",first,3),"actual interleaved displayed pixels");
        check(click(other,500,300),"hardware mouse goes to other process");
        SetWindowPos(back,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);SetForegroundWindow(back);pump(100);
        struct Sample raised[]={{650,400,blue},{950,400,red},{500,700,green}};
        check(capture("raised",raised,3),"static retained windows reorder without repaint");
        check(click(back,650,400),"hardware mouse goes to raised parent window");
        SetWindowPos(back,HWND_TOP,100,100,400,250,0);ShowWindow(other,SW_MINIMIZE);pump(100);
        struct Sample moved[]={{200,200,blue},{650,400,red}};
        check(capture("moved-minimized",moved,2),"move resize and minimize displayed pixels");
        HWND modal=window(yellow,front,700,450,250,150);EnableWindow(front,FALSE);SetForegroundWindow(modal);pump(100);
        struct Sample dialog[]={{800,500,yellow},{650,400,red},{200,200,blue}};
        check(capture("owned-modal",dialog,3),"owned modal content above its owner");
        check(modal&&GetWindow(modal,GW_OWNER)==front&&!IsWindowEnabled(front)&&click(modal,800,500),"modal ownership and hardware input");
        if(modal)DestroyWindow(modal);EnableWindow(front,TRUE);ShowWindow(other,SW_RESTORE);SetForegroundWindow(other);pump(100);
        struct Sample restored[]={{200,200,blue},{650,400,green}};
        check(capture("restored",restored,2),"restored child retained pixels and focus");
        check(click(other,650,400),"restored child accepts hardware input");
        SetWindowPos(back,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
        SetWindowPos(other,HWND_TOP,200,150,700,500,0);
        SetWindowPos(front,HWND_TOP,600,350,400,350,0);
        HRGN shape=CreateRectRgn(0,0,400,80),leg=CreateRectRgn(0,80,80,350);
        BOOL shaped=shape&&leg&&CombineRgn(shape,shape,leg,RGN_OR)!=ERROR;
        if(leg)DeleteObject(leg);
        BOOL adopted=shaped&&SetWindowRgn(front,shape,TRUE);
        if(!adopted&&shape)DeleteObject(shape);
        check(adopted,"apply authoritative L-shaped window region");pump(100);
        struct Sample cutout[]={{650,500,red},{800,500,green},{850,380,red}};
        check(capture("shaped",cutout,3),"shape cutout shows the retained lower process window");
        check(click(other,800,500),"hardware mouse passes through region cutout");
        SetWindowPos(front,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
        HRGN empty=CreateRectRgn(0,0,0,0);adopted=empty&&SetWindowRgn(front,empty,TRUE);
        if(!adopted&&empty)DeleteObject(empty);
        check(adopted,"apply empty window region");pump(100);
        struct Sample hidden_shape[]={{650,500,green},{800,500,green}};
        check(capture("shape-empty",hidden_shape,2),"empty region draws no pixels");
        check(SetWindowRgn(front,NULL,TRUE),"clear window region");pump(100);
        struct Sample cleared_shape[]={{650,500,red},{800,500,red}};
        check(capture("shape-cleared",cleared_shape,2),"cleared region restores rectangular retained pixels");
        SetWindowLongPtrW(front,GWL_EXSTYLE,GetWindowLongPtrW(front,GWL_EXSTYLE)|WS_EX_LAYERED);
        check(SetLayeredWindowAttributes(front,0,128,LWA_ALPHA),"constant alpha enabled");pump(100);
        struct Sample alpha_half[]={{650,500,RGB(125,115,65)},{800,500,RGB(125,115,65)}};
        check(capture("alpha-constant",alpha_half,2),"constant opacity blends retained cross-process pixels once");
        check(click(front,650,500),"visible constant-alpha pixels retain hardware input");
        check(SetLayeredWindowAttributes(front,0,0,LWA_ALPHA),"zero constant alpha enabled");pump(100);
        check(capture("alpha-zero",hidden_shape,2),"zero alpha preserves underlying pixels");
        check(click(other,650,500),"hardware mouse passes through zero constant alpha");
        SetWindowPos(front,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
        SetPropW(front,L"Split",(HANDLE)1);
        check(SetLayeredWindowAttributes(front,red,128,LWA_COLORKEY|LWA_ALPHA),"colour key and constant alpha enabled");
        InvalidateRect(front,NULL,TRUE);UpdateWindow(front);pump(100);
        struct Sample keyed[]={{650,500,green},{850,500,RGB(135,180,50)}};
        check(capture("alpha-key",keyed,2),"colour key coverage and non-key alpha are independent");
        check(click(front,850,500),"non-key pixels retain hardware input");
        check(click(other,650,500),"hardware mouse passes through colour-key coverage");
        SetWindowPos(front,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE);
        RemovePropW(front,L"Split");
        SetWindowLongPtrW(front,GWL_EXSTYLE,GetWindowLongPtrW(front,GWL_EXSTYLE)&~WS_EX_LAYERED);
        SetWindowLongPtrW(front,GWL_EXSTYLE,GetWindowLongPtrW(front,GWL_EXSTYLE)|WS_EX_LAYERED);
        BITMAPINFO info={0};info.bmiHeader.biSize=sizeof(info.bmiHeader);info.bmiHeader.biWidth=400;
        info.bmiHeader.biHeight=-350;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
        void *bits=NULL;HDC source=CreateCompatibleDC(NULL);
        HBITMAP bitmap=CreateDIBSection(source,&info,DIB_RGB_COLORS,&bits,NULL,0),previous=NULL;
        BOOL prepared=source&&bitmap&&bits;
        if(prepared) {
            previous=SelectObject(source,bitmap);
            for(unsigned y=0;y<350;++y)for(unsigned x=0;x<400;++x)
                ((uint32_t *)bits)[y*400+x]=x<200 ? 0x806e191e : 0;
            POINT origin={0,0},position={600,350};SIZE size={400,350};
            BLENDFUNCTION blend={AC_SRC_OVER,0,255,AC_SRC_ALPHA};
            prepared=UpdateLayeredWindow(front,NULL,&position,&size,source,&origin,0,&blend,ULW_ALPHA);
            check(prepared,"per-pixel premultiplied alpha surface published");pump(100);
            struct Sample pixel_alpha[]={{650,500,RGB(125,115,65)},{850,500,green}};
            check(capture("alpha-pixel",pixel_alpha,2),"per-pixel alpha blends without double multiplication");
            blend.SourceConstantAlpha=128;
            check(UpdateLayeredWindow(front,NULL,NULL,NULL,NULL,NULL,0,&blend,ULW_ALPHA),"opacity-only layered update");pump(100);
            struct Sample pixel_half[]={{650,500,RGB(77,148,67)},{850,500,green}};
            check(capture("alpha-pixel-opacity",pixel_half,2),"opacity-only update refreshes retained pixels");
            check(click(front,650,500),"nonzero per-pixel alpha retains hardware input");
            check(click(other,850,500),"hardware mouse passes through zero per-pixel alpha");
            SetWindowPos(front,HWND_TOP,500,300,0,0,SWP_NOSIZE);pump(100);
            check(click(front,550,450),"moved layered coverage follows visible pixels");
            check(click(other,750,450),"moved layered hole keeps cross-process click-through");
        } else check(FALSE,"allocate owned per-pixel alpha fixture");
        if(previous)SelectObject(source,previous);if(bitmap)DeleteObject(bitmap);if(source)DeleteDC(source);
        SetWindowLongPtrW(front,GWL_EXSTYLE,GetWindowLongPtrW(front,GWL_EXSTYLE)&~WS_EX_LAYERED);
        SetWindowPos(front,HWND_TOP,600,350,400,350,SWP_FRAMECHANGED);
        InvalidateRect(front,NULL,TRUE);UpdateWindow(front);pump(100);
        check(capture("alpha-cleared",cleared_shape,2),"clearing layered style restores opaque pixels");
        check(click(front,850,500),"cleared layered style restores input through former hole");


    }
    write_file("C:\\.pw-compose-stop","",0);DWORD wait=started?WaitForSingleObject(child.hProcess,10000):WAIT_FAILED,code=1;
    BOOL clean=wait==WAIT_OBJECT_0&&GetExitCodeProcess(child.hProcess,&code)&&!code;
    check(clean,"child exits normally");check(IsWindow(shell)&&IsWindow(back)&&IsWindow(front),"parent and shell survive child exit");
    if(back)DestroyWindow(back);if(front)DestroyWindow(front);
    if(started){CloseHandle(child.hThread);CloseHandle(child.hProcess);}
    DeleteFileA("C:\\.pw-compose-ready");DeleteFileA("C:\\.pw-compose-stop");
    char line[256];DWORD written;wsprintfA(line,"PS5_PROTON_WINDOW_COMPOSE %s checks=%u failures=%u captures=%u\n",failures?"FAIL":"PASS",checks,failures,captures);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,lstrlenA(line),&written,NULL);OutputDebugStringA(line);
    if(clean&&IsWindow(shell))PostMessageW(shell,WM_CLOSE,0,0);
    ExitProcess(failures?1:0);
}
