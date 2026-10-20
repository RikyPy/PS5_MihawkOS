/* SPDX-License-Identifier: GPL-3.0-or-later
 * Headless PE32/64 oracle of the pointer scheme: the session root's initializer (wine/ps5/pw_shell_cursors.h, the
 * very function) against the real registry, then user32's system cursors (Wine patch 1137) loaded from the scheme's
 * files. Owns only the probe prefix: the scheme values and the marker are restored before it returns. */
#include "../wine/ps5/pw_shell_cursors.h"
static unsigned checks,failures;
static void say(const char *text)
{
    DWORD written;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,lstrlenA(text),&written,NULL);OutputDebugStringA(text);
}
static void check(BOOL okay,const char *name)
{
    char line[256];++checks;if(!okay)++failures;
    lstrcpyA(line,"cursor-scheme ");lstrcatA(line,okay?"PASS: ":"FAIL: ");lstrcatA(line,name);lstrcatA(line,"\n");say(line);
}
static void append_number(char *text,unsigned value)
{
    char digits[12];unsigned count=0;
    do {digits[count++]=(char)('0'+value%10);value/=10;}while(value);
    unsigned length=lstrlenA(text);
    while(count)text[length++]=digits[--count];text[length]=0;
}
static const struct { const WCHAR *role, *file; } scheme[] = PW_CURSOR_SCHEME;
enum { ROLES = sizeof(scheme) / sizeof(scheme[0]) };
static BYTE saved[ROLES + 2][2 * MAX_PATH];
static DWORD saved_size[ROLES + 2], saved_type[ROLES + 2], saved_status[ROLES + 2];
static void save(HKEY key,const WCHAR *name,unsigned slot)
{
    saved_size[slot]=sizeof(saved[slot]);
    saved_status[slot]=RegQueryValueExW(key,name,NULL,&saved_type[slot],saved[slot],&saved_size[slot]);
}
static BOOL restore(HKEY key,const WCHAR *name,unsigned slot)
{
    if(saved_status[slot]==ERROR_FILE_NOT_FOUND){LONG s=RegDeleteValueW(key,name);return s==ERROR_SUCCESS||s==ERROR_FILE_NOT_FOUND;}
    return RegSetValueExW(key,name,0,saved_type[slot],saved[slot],saved_size[slot])==ERROR_SUCCESS;
}
static void clear(HKEY settings,HKEY cursors)
{
    unsigned i;RegDeleteValueW(settings,L"CursorsInitialized");RegDeleteValueW(cursors,NULL);
    for(i=0;i<ROLES;i++)RegDeleteValueW(cursors,scheme[i].role);
}
/* A cursor user32 loaded from a file has no module; one from its own resources names user32. */
static BOOL from_file(HCURSOR cursor,POINT *hotspot)
{
    ICONINFOEXW info;info.cbSize=sizeof(info);
    if(!cursor||!GetIconInfoExW(cursor,&info))return FALSE;
    if(info.hbmMask)DeleteObject(info.hbmMask);
    if(info.hbmColor)DeleteObject(info.hbmColor);
    hotspot->x=info.xHotspot;hotspot->y=info.yHotspot;
    return !info.szModName[0] && !info.wResID;
}
void mainCRTStartup(void)
{
    HKEY settings,cursors;unsigned i;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\MihawkOS",0,NULL,0,KEY_QUERY_VALUE|KEY_SET_VALUE,NULL,&settings,NULL) ||
       RegCreateKeyExW(HKEY_CURRENT_USER,L"Control Panel\\Cursors",0,NULL,0,KEY_QUERY_VALUE|KEY_SET_VALUE,NULL,&cursors,NULL))ExitProcess(2);
    for(i=0;i<ROLES;i++)save(cursors,scheme[i].role,i);
    save(cursors,NULL,ROLES);save(settings,L"CursorsInitialized",ROLES+1);

    WCHAR path[MAX_PATH],value[MAX_PATH];BOOL files=TRUE;
    for(i=0;i<ROLES;i++)
    {
        GetWindowsDirectoryW(path,MAX_PATH);lstrcatW(path,L"\\Cursors\\");lstrcatW(path,scheme[i].file);
        files=files&&GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES;
    }
    check(files,"every scheme file is staged in C:\\windows\\Cursors");
    clear(settings,cursors);
    check(pw_shell_prepare_cursors()==ERROR_SUCCESS,"the scheme initializes through the real registry");
    BOOL named=TRUE;DWORD type,size;
    for(i=0;i<ROLES;i++)
    {
        WCHAR expected[MAX_PATH];lstrcpyW(expected,L"%SystemRoot%\\Cursors\\");lstrcatW(expected,scheme[i].file);
        size=sizeof(value);
        named=named&&RegQueryValueExW(cursors,scheme[i].role,NULL,&type,(BYTE *)value,&size)==ERROR_SUCCESS&&
              type==REG_EXPAND_SZ&&!lstrcmpW(value,expected);
    }
    check(named,"every role names its file as REG_EXPAND_SZ %SystemRoot%\\Cursors");
    size=sizeof(value);
    check(RegQueryValueExW(cursors,NULL,NULL,&type,(BYTE *)value,&size)==ERROR_SUCCESS&&!lstrcmpW(value,PW_CURSOR_SCHEME_NAME),
          "the scheme is named");

    POINT hot;HCURSOR arrow=LoadCursorW(NULL,(LPCWSTR)IDC_ARROW);
    check(from_file(arrow,&hot)&&hot.x==1&&hot.y==1,"the system arrow comes from the scheme file, hotspot 1,1");
    check(LoadCursorW(NULL,(LPCWSTR)IDC_ARROW)==arrow,"the system arrow is one shared cursor");
    check(from_file(LoadCursorW(NULL,(LPCWSTR)IDC_IBEAM),&hot)&&hot.x==16&&hot.y==16,"the text cursor comes from the scheme");
    check(from_file(LoadCursorW(NULL,(LPCWSTR)IDC_HAND),&hot)&&hot.x==8&&hot.y==1,"the link cursor comes from the scheme");
    HCURSOR wait=LoadCursorW(NULL,(LPCWSTR)IDC_WAIT);DWORD rate=0,steps=0;
    check(wait&&GetCursorFrameInfo(wait,0,0,&rate,&steps)&&steps==12&&rate==5,"the busy cursor animates 12 frames at 5 jiffies");
    DestroyCursor(wait);
    check(LoadCursorW(NULL,(LPCWSTR)IDC_WAIT)==wait&&GetCursorFrameInfo(wait,0,0,&rate,&steps),"a shared animated cursor outlives DestroyCursor");

    clear(settings,cursors);
    RegSetValueExW(cursors,L"Arrow",0,REG_SZ,(BYTE *)L"C:\\chosen.cur",sizeof(L"C:\\chosen.cur"));
    check(pw_shell_prepare_cursors()==ERROR_SUCCESS,"a chosen scheme initializes without replacement");
    size=sizeof(value);
    check(RegQueryValueExW(cursors,L"Arrow",NULL,&type,(BYTE *)value,&size)==ERROR_SUCCESS&&!lstrcmpW(value,L"C:\\chosen.cur")&&
          RegQueryValueExW(cursors,L"IBeam",NULL,NULL,NULL,NULL)==ERROR_FILE_NOT_FOUND,"a chosen scheme is kept as it is");
    DWORD initialized=0;size=sizeof(initialized);
    check(RegQueryValueExW(settings,L"CursorsInitialized",NULL,&type,(BYTE *)&initialized,&size)==ERROR_SUCCESS&&
          type==REG_DWORD&&initialized==1,"the one-time marker is kept");

    BOOL restored=TRUE;clear(settings,cursors);
    for(i=0;i<ROLES;i++)restored=restore(cursors,scheme[i].role,i)&&restored;
    restored=restore(cursors,NULL,ROLES)&&restore(settings,L"CursorsInitialized",ROLES+1)&&restored;
    check(restored,"the probe restores the scheme and the marker");RegCloseKey(settings);RegCloseKey(cursors);
    char line[160];
    lstrcpyA(line,"PS5_PROTON_CURSOR_SCHEME ");lstrcatA(line,failures?"FAIL":"PASS");lstrcatA(line," checks=");
    append_number(line,checks);lstrcatA(line," failures=");append_number(line,failures);lstrcatA(line,"\n");
    say(line);ExitProcess(failures?1:0);
}
