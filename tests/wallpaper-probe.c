/* SPDX-License-Identifier: GPL-3.0-or-later
 * Headless PE32/64 oracle of the actual desktop initializer, Explorer's in the session (wine/ps5/pw_shell_wallpaper.h). Diagnostic owns
 * only the probe prefix. Restore the previous wallpaper and initializer marker.
 * No graphics import and no renderer-side background substitute. */
#include "../wine/ps5/pw_shell_wallpaper.h"
static unsigned checks,failures;
static void check(BOOL okay,const char *name)
{
    char line[256];DWORD written;++checks;if(!okay)++failures;
    lstrcpyA(line,"wallpaper ");lstrcatA(line,okay?"PASS: ":"FAIL: ");lstrcatA(line,name);lstrcatA(line,"\n");
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,lstrlenA(line),&written,NULL);OutputDebugStringA(line);
}
static void append_number(char *text,unsigned value)
{
    char digits[12];unsigned count=0;
    do {digits[count++]=(char)('0'+value%10);value/=10;}while(value);
    unsigned length=lstrlenA(text);
    while(count)text[length++]=digits[--count];text[length]=0;
}
static BOOL read_wallpaper(HKEY key,WCHAR *path,DWORD count)
{
    DWORD type=0,size=count*sizeof(WCHAR);
    return RegQueryValueExW(key,L"Wallpaper",NULL,&type,(BYTE *)path,&size)==ERROR_SUCCESS && type==REG_SZ;
}
static BOOL set_wallpaper(HKEY key,const WCHAR *path)
{
    return RegSetValueExW(key,L"Wallpaper",0,REG_SZ,(BYTE *)path,(lstrlenW(path)+1)*sizeof(WCHAR))==ERROR_SUCCESS;
}
void mainCRTStartup(void)
{
    HKEY settings,desktop;BYTE saved_wallpaper[2048],saved_marker[64];
    DWORD wallpaper_size=sizeof(saved_wallpaper),marker_size=sizeof(saved_marker),wallpaper_type=0,marker_type=0;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\MihawkOS",0,NULL,0,KEY_QUERY_VALUE|KEY_SET_VALUE,NULL,&settings,NULL) ||
       RegCreateKeyExW(HKEY_CURRENT_USER,L"Control Panel\\Desktop",0,NULL,0,KEY_QUERY_VALUE|KEY_SET_VALUE,NULL,&desktop,NULL))ExitProcess(2);
    DWORD old_wallpaper=RegQueryValueExW(desktop,L"Wallpaper",NULL,&wallpaper_type,saved_wallpaper,&wallpaper_size);
    DWORD old_marker=RegQueryValueExW(settings,L"WallpaperInitialized",NULL,&marker_type,saved_marker,&marker_size);
    if((old_wallpaper!=ERROR_SUCCESS && old_wallpaper!=ERROR_FILE_NOT_FOUND) ||
       (old_marker!=ERROR_SUCCESS && old_marker!=ERROR_FILE_NOT_FOUND))ExitProcess(3);
    WCHAR expected[MAX_PATH],actual[MAX_PATH];GetWindowsDirectoryW(expected,MAX_PATH);
    lstrcatW(expected,L"\\web\\wallpaper\\proton.bmp");
    DWORD attributes=GetFileAttributesW(expected),asset_error=GetLastError();
    BOOL has_asset=attributes!=INVALID_FILE_ATTRIBUTES;
    check(has_asset || asset_error==ERROR_FILE_NOT_FOUND || asset_error==ERROR_PATH_NOT_FOUND,
          "managed asset availability is readable");
    RegDeleteValueW(settings,L"WallpaperInitialized");set_wallpaper(desktop,L"");
    check(pw_shell_prepare_wallpaper()==ERROR_SUCCESS,"default initializes through real registry");
    check(read_wallpaper(desktop,actual,MAX_PATH) && !lstrcmpW(actual,has_asset?expected:L""),"available managed default or plain background is persisted");
    set_wallpaper(desktop,L"C:\\chosen.bmp");RegDeleteValueW(settings,L"WallpaperInitialized");
    check(pw_shell_prepare_wallpaper()==ERROR_SUCCESS,"existing choice initializes without replacement");
    check(read_wallpaper(desktop,actual,MAX_PATH) && !lstrcmpW(actual,L"C:\\chosen.bmp"),"existing nonempty choice preserved");
    set_wallpaper(desktop,L"");
    check(pw_shell_prepare_wallpaper()==ERROR_SUCCESS,"later explicit empty choice succeeds");
    check(read_wallpaper(desktop,actual,MAX_PATH) && !actual[0],"later empty choice preserved");
    DWORD initialized=0,type=0,size=sizeof(initialized);
    check(RegQueryValueExW(settings,L"WallpaperInitialized",NULL,&type,(BYTE *)&initialized,&size)==ERROR_SUCCESS &&
          type==REG_DWORD && initialized==1,"one-time initializer marker is retained");
    /* This diagnostic owns the probe prefix; move only its managed local BMP,
     * never a user's selected wallpaper, and restore before returning. */
    WCHAR hidden[MAX_PATH];lstrcpyW(hidden,expected);lstrcatW(hidden,L".pw-probe");
    BOOL moved=has_asset && GetFileAttributesW(hidden)==INVALID_FILE_ATTRIBUTES && MoveFileW(expected,hidden);
    check(!has_asset || moved,"owned managed wallpaper is absent for optional-file test");
    if(!has_asset || moved)
    {
        RegDeleteValueW(settings,L"WallpaperInitialized");set_wallpaper(desktop,L"");
        check(pw_shell_prepare_wallpaper()==ERROR_SUCCESS,"missing optional wallpaper does not block desktop");
        check(read_wallpaper(desktop,actual,MAX_PATH) && !actual[0],"missing wallpaper preserves plain background");
        check(!moved || MoveFileW(hidden,expected),"diagnostic restores original asset availability");
    }
    BOOL restored=old_wallpaper==ERROR_FILE_NOT_FOUND ? RegDeleteValueW(desktop,L"Wallpaper")==ERROR_SUCCESS :
        RegSetValueExW(desktop,L"Wallpaper",0,wallpaper_type,saved_wallpaper,wallpaper_size)==ERROR_SUCCESS;
    restored=(old_marker==ERROR_FILE_NOT_FOUND ? RegDeleteValueW(settings,L"WallpaperInitialized")==ERROR_SUCCESS :
        RegSetValueExW(settings,L"WallpaperInitialized",0,marker_type,saved_marker,marker_size)==ERROR_SUCCESS) && restored;
    check(restored,"diagnostic restores original wallpaper and marker");RegCloseKey(settings);RegCloseKey(desktop);
    char line[160];DWORD written;
    lstrcpyA(line,"PS5_PROTON_WALLPAPER ");lstrcatA(line,failures?"FAIL":"PASS");lstrcatA(line," checks=");
    append_number(line,checks);lstrcatA(line," failures=");append_number(line,failures);lstrcatA(line,"\n");
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),line,lstrlenA(line),&written,NULL);OutputDebugStringA(line);ExitProcess(failures?1:0);
}
