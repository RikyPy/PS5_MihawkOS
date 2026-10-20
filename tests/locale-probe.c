/* SPDX-License-Identifier: GPL-3.0-or-later
 * The guided setup's language reaches Windows programs (wine/patches/1230, docs/GUIDED_SETUP.md): the title gives the
 * session WINE_PS5_LOCALE, and every program but the shell's Explorer takes it as its system locale, user locale, UI
 * language and keyboard layout, its children too. Run with WINE_PS5_LOCALE=fr_FR.UTF-8 it expects fr-FR; with
 * --expect <name> another; a child started with CreateProcess must agree.
 */
#define UNICODE
#include <windows.h>
__declspec(dllimport) int __cdecl sprintf(char *,const char *,...);
static unsigned checks,failures;
static void check(BOOL okay,const char *name)
{
    ++checks;failures+=!okay;
    if(!okay) { char line[180];sprintf(line,"locale: FAIL %s\n",name);OutputDebugStringA(line); }
}
void mainCRTStartup(void)
{
    const WCHAR *command=GetCommandLineW(),*expected=L"fr-FR";BOOL child=FALSE;
    for(const WCHAR *p=command;*p;++p) {
        if(CompareStringOrdinal(p,min(lstrlenW(p),7),L"--child",7,FALSE)==CSTR_EQUAL)child=TRUE;
        if(CompareStringOrdinal(p,min(lstrlenW(p),9),L"--expect ",9,FALSE)==CSTR_EQUAL)expected=p+9;
    }
    WCHAR user[LOCALE_NAME_MAX_LENGTH]={0},system[LOCALE_NAME_MAX_LENGTH]={0};
    GetUserDefaultLocaleName(user,LOCALE_NAME_MAX_LENGTH);GetSystemDefaultLocaleName(system,LOCALE_NAME_MAX_LENGTH);
    WCHAR name[LOCALE_NAME_MAX_LENGTH]={0};int i=0;
    for(;expected[i] && expected[i]!=' ' && i<LOCALE_NAME_MAX_LENGTH-1;++i)name[i]=expected[i];
    const LANGID language=LANGIDFROMLCID(LocaleNameToLCID(name,0)),ui=GetUserDefaultUILanguage();
    const HKL keyboard=GetKeyboardLayout(0);
    check(!lstrcmpiW(user,name),"user locale");
    check(!lstrcmpiW(system,name),"system locale");
    check(ui==language,"UI language");
    check(LOWORD((ULONG_PTR)keyboard)==language,"keyboard layout");
    if(!child) {
        WCHAR executable[MAX_PATH],line[1024];GetModuleFileNameW(NULL,executable,MAX_PATH);
        lstrcpyW(line,L"\"");lstrcatW(line,executable);lstrcatW(line,L"\" --child --expect ");lstrcatW(line,name);
        STARTUPINFOW startup={0};PROCESS_INFORMATION process={0};startup.cb=sizeof(startup);
        const BOOL started=CreateProcessW(NULL,line,NULL,NULL,FALSE,0,NULL,NULL,&startup,&process);
        DWORD code=~0u;
        check(started,"child created");
        if(started) {
            check(WaitForSingleObject(process.hProcess,20000)==WAIT_OBJECT_0,"bounded child completion");
            check(GetExitCodeProcess(process.hProcess,&code) && !code,"child agrees");
            CloseHandle(process.hThread);CloseHandle(process.hProcess);
        }
    }
    char verdict[256];
    sprintf(verdict,"PS5_PROTON_LOCALE %s%s checks=%u failures=%u user=%ls system=%ls ui=%04x keyboard=%08lx\n",
            child?"child ":"",failures?"FAIL":"PASS",checks,failures,user,system,ui,(unsigned long)(ULONG_PTR)keyboard);
    OutputDebugStringA(verdict);ExitProcess(failures?1:0);
}
