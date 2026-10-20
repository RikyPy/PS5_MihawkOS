/* SPDX-License-Identifier: GPL-3.0-or-later
 * The display modes a program in a helper is offered (docs/PLAN.md, "The sync device for helpers"): with
 * PROTON_120HZ=1 the broker's display ran at 119.88 Hz, yet a Direct3D 12 game listed only 60 Hz and set
 * 1920x1080@60 (2026-10-17; record removed from the tree). One line says what this process sees: WINE_PS5_REFRESH, the current and
 * registry modes, every mode's rate, whether the full screen is offered at the expected rate, and GDI's VREFRESH; a
 * second line, what DXGI (the dxgi.dll the program gets, DXVK's) lists for its first output, as a game asks.
 *   display-modes-probe.exe [--expect=N]   PASS when the current mode, and a full-screen mode in both lists, run at
 *                                          N Hz (60 default)
 */
#define UNICODE
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <dxgi.h>
__declspec(dllimport) int __cdecl sprintf(char *,const char *,...);
void mainCRTStartup(void)
{
    DWORD expected=60;
    static const WCHAR flag[]=L"--expect=";
    for(const WCHAR *p=GetCommandLineW();*p;++p) {
        DWORD n=0;while(flag[n] && p[n]==flag[n])++n;
        if(!flag[n]) { expected=0;for(p+=n;*p>='0' && *p<='9';++p)expected=expected*10+(*p-'0');break; }
    }
    char refresh[16]="unset";
    if(!GetEnvironmentVariableA("WINE_PS5_REFRESH",refresh,sizeof(refresh)))lstrcpyA(refresh,"unset");
    DEVMODEW current={.dmSize=sizeof(current)},saved={.dmSize=sizeof(saved)},mode={.dmSize=sizeof(mode)};
    const BOOL have_current=EnumDisplaySettingsW(NULL,ENUM_CURRENT_SETTINGS,&current);
    const BOOL have_saved=EnumDisplaySettingsW(NULL,ENUM_REGISTRY_SETTINGS,&saved);
    DWORD rates[8]={0},rate_count=0,modes=0,full_at_expected=0;
    for(DWORD i=0;EnumDisplaySettingsW(NULL,i,&mode);++i) {
        ++modes;
        DWORD k=0;while(k<rate_count && rates[k]!=mode.dmDisplayFrequency)++k;
        if(k==rate_count && rate_count<8)rates[rate_count++]=mode.dmDisplayFrequency;
        if(have_current && mode.dmPelsWidth==current.dmPelsWidth && mode.dmPelsHeight==current.dmPelsHeight &&
           mode.dmDisplayFrequency==expected)full_at_expected=1;
    }
    HDC dc=GetDC(NULL);const int vrefresh=dc ? GetDeviceCaps(dc,VREFRESH) : -1;if(dc)ReleaseDC(NULL,dc);
    char line[400];int used=sprintf(line,"display-modes: refresh_env=%s current=%lux%lu@%lu registry=%lux%lu@%lu modes=%lu rates=",
        refresh,have_current ? current.dmPelsWidth : 0,have_current ? current.dmPelsHeight : 0,
        have_current ? current.dmDisplayFrequency : 0,have_saved ? saved.dmPelsWidth : 0,have_saved ? saved.dmPelsHeight : 0,
        have_saved ? saved.dmDisplayFrequency : 0,modes);
    for(DWORD k=0;k<rate_count;++k)used+=sprintf(line+used,"%s%lu",k ? "," : "",rates[k]);
    sprintf(line+used," full_at_%lu=%lu vrefresh=%d\n",expected,full_at_expected,vrefresh);
    OutputDebugStringA(line);
    /* DXGI's view, loaded at run time so the probe needs no import library. */
    DWORD dxgi_rates[8]={0},dxgi_rate_count=0,dxgi_modes=0,dxgi_full_at_expected=0;HRESULT hr=E_FAIL;
    HMODULE dxgi=LoadLibraryW(L"dxgi.dll");
    HRESULT (WINAPI *create)(REFIID,void **)=dxgi ? (void *)GetProcAddress(dxgi,"CreateDXGIFactory1") : NULL;
    IDXGIFactory1 *factory=NULL;IDXGIAdapter1 *adapter=NULL;IDXGIOutput *output=NULL;
    if(create && SUCCEEDED(hr=create(&IID_IDXGIFactory1,(void **)&factory)) &&
       SUCCEEDED(hr=IDXGIFactory1_EnumAdapters1(factory,0,&adapter)) && SUCCEEDED(hr=IDXGIAdapter1_EnumOutputs(adapter,0,&output))) {
        static DXGI_MODE_DESC list[512];UINT count=512;
        if(SUCCEEDED(hr=IDXGIOutput_GetDisplayModeList(output,DXGI_FORMAT_R8G8B8A8_UNORM,0,&count,list)))
            for(UINT i=0;i<count;++i) {
                const DWORD hz=list[i].RefreshRate.Denominator ?
                    (list[i].RefreshRate.Numerator+list[i].RefreshRate.Denominator/2)/list[i].RefreshRate.Denominator : 0;
                ++dxgi_modes;
                DWORD k=0;while(k<dxgi_rate_count && dxgi_rates[k]!=hz)++k;
                if(k==dxgi_rate_count && dxgi_rate_count<8)dxgi_rates[dxgi_rate_count++]=hz;
                if(have_current && list[i].Width==current.dmPelsWidth && list[i].Height==current.dmPelsHeight && hz==expected)
                    dxgi_full_at_expected=1;
            }
    }
    if(output)IDXGIOutput_Release(output);
    if(adapter)IDXGIAdapter1_Release(adapter);
    if(factory)IDXGIFactory1_Release(factory);
    used=sprintf(line,"display-modes: dxgi hr=%#lx modes=%lu rates=",(unsigned long)hr,dxgi_modes);
    for(DWORD k=0;k<dxgi_rate_count;++k)used+=sprintf(line+used,"%s%lu",k ? "," : "",dxgi_rates[k]);
    sprintf(line+used," full_at_%lu=%lu\n",expected,dxgi_full_at_expected);
    OutputDebugStringA(line);
    /* Then this process takes the desktop window, which publishes the driver's mode from its own environment
     * (patch 1187's line says so), and lists again: what a program sees once it has made a window. */
    GetDesktopWindow();
    DEVMODEW after={.dmSize=sizeof(after)};DWORD after_rates[8]={0},after_count=0,after_modes=0;
    const BOOL have_after=EnumDisplaySettingsW(NULL,ENUM_CURRENT_SETTINGS,&after);
    for(DWORD i=0;EnumDisplaySettingsW(NULL,i,&mode);++i) {
        ++after_modes;
        DWORD k=0;while(k<after_count && after_rates[k]!=mode.dmDisplayFrequency)++k;
        if(k==after_count && after_count<8)after_rates[after_count++]=mode.dmDisplayFrequency;
    }
    used=sprintf(line,"display-modes: after its own desktop window current=%lux%lu@%lu modes=%lu rates=",
        have_after ? after.dmPelsWidth : 0,have_after ? after.dmPelsHeight : 0,have_after ? after.dmDisplayFrequency : 0,
        after_modes);
    for(DWORD k=0;k<after_count;++k)used+=sprintf(line+used,"%s%lu",k ? "," : "",after_rates[k]);
    sprintf(line+used,"\n");
    OutputDebugStringA(line);
    const BOOL pass=have_current && current.dmDisplayFrequency==expected && full_at_expected && dxgi_full_at_expected;
    char verdict[120];sprintf(verdict,"PS5_PROTON_DISPLAY_MODES %s expected=%lu\n",pass ? "PASS" : "FAIL",expected);
    OutputDebugStringA(verdict);ExitProcess(pass ? 0 : 1);
}
