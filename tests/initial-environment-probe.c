/* SPDX-License-Identifier: GPL-3.0-or-later
 * Explicit launch settings survive root provisioning, then ordinary Windows
 * inheritance and explicit CreateProcess environments remain authoritative.
 * Same source is run under the pinned Linux Wine oracle and both console PEs.
 */
#define UNICODE
#include <windows.h>
__declspec(dllimport) int __cdecl sprintf(char *,const char *,...);
static unsigned checks,failures;
static void check(BOOL okay,const char *name)
{
    ++checks;failures+=!okay;
    if(!okay) { char line[180];sprintf(line,"initial-env: FAIL %s error=%lu\n",name,GetLastError());OutputDebugStringA(line); }
}
static void value(const WCHAR *name,const WCHAR *expected,const char *label)
{
    WCHAR buffer[256]={0};DWORD length=GetEnvironmentVariableW(name,buffer,256);
    check(length==(DWORD)lstrlenW(expected) && !lstrcmpW(buffer,expected),label);
}
static void selected(void)
{
    value(L"MIHAWK_ENV_ROOT",L"launch selected","root selected value");
    value(L"MIHAWK_ENV_UTF8",L"caf\x00e9 \x03a9","UTF-8 Unicode value");
    value(L"MIHAWK_ENV_CASE",L"last wins","case-insensitive last override");
    value(L"PATH",L"C:\\Selected Tools;C:\\windows\\system32","PATH after registry provisioning");
    WCHAR *environment=GetEnvironmentStringsW();unsigned empty=0,variants=0;
    if(environment) {
        for(const WCHAR *item=environment;*item;item+=lstrlenW(item)+1) {
            if(!lstrcmpiW(item,L"MIHAWK_ENV_EMPTY="))++empty;
            const WCHAR *equals=item;while(*equals && *equals!='=')++equals;
            if(equals-item==15 && CompareStringOrdinal(item,15,L"MIHAWK_ENV_CASE",15,TRUE)==CSTR_EQUAL)++variants;
        }
        FreeEnvironmentStringsW(environment);
    }
    check(empty==1,"empty value remains present");
    check(variants==1,"selected case-insensitive name is unique");
}
static void run(const WCHAR *mode,WCHAR *environment)
{
    WCHAR executable[MAX_PATH],command[1024];GetModuleFileNameW(NULL,executable,MAX_PATH);
    lstrcpyW(command,L"\"");lstrcatW(command,executable);lstrcatW(command,L"\" ");lstrcatW(command,mode);
    STARTUPINFOW startup={0};PROCESS_INFORMATION child={0};startup.cb=sizeof(startup);
    BOOL started=CreateProcessW(NULL,command,NULL,NULL,FALSE,CREATE_UNICODE_ENVIRONMENT,environment,NULL,&startup,&child);
    check(started,"child created");
    if(started) {
        DWORD wait=WaitForSingleObject(child.hProcess,20000),code=~0u;
        check(wait==WAIT_OBJECT_0,"bounded child completion");
        check(GetExitCodeProcess(child.hProcess,&code) && !code,"child environment verdict and exit");
        if(wait!=WAIT_OBJECT_0) { TerminateProcess(child.hProcess,1);WaitForSingleObject(child.hProcess,2000); }
        CloseHandle(child.hThread);CloseHandle(child.hProcess);
    }
}
void mainCRTStartup(void)
{
    const WCHAR *command=GetCommandLineW();BOOL inherited=FALSE,explicit=FALSE,leaf=FALSE;
    for(const WCHAR *p=command;*p;++p) {
        if(!lstrcmpW(p,L"--inherit"))inherited=TRUE;
        if(!lstrcmpW(p,L"--explicit"))explicit=TRUE;
        if(!lstrcmpW(p,L"--leaf"))leaf=TRUE;
    }
    if(explicit) {
        value(L"MIHAWK_ENV_ROOT",L"child override","explicit child value");
        SetLastError(0);WCHAR buffer[32];
        check(!GetEnvironmentVariableW(L"MIHAWK_ENV_UTF8",buffer,32) && GetLastError()==ERROR_ENVVAR_NOT_FOUND,
              "explicit child block does not reapply root settings");
        value(L"PATH",L"C:\\Child Tools","explicit child PATH");
    } else {
        selected();
        if(inherited || leaf)value(L"MIHAWK_ENV_DESCENDANT",L"parent selected","ordinary inheritance");
        if(inherited)run(L"--leaf",NULL);
        if(!inherited && !leaf) {
            check(SetEnvironmentVariableW(L"MIHAWK_ENV_DESCENDANT",L"parent selected"),"set inherited value");
            run(L"--inherit",NULL);
            static WCHAR child_environment[]=L"MIHAWK_ENV_ROOT=child override\0PATH=C:\\Child Tools\0\0";
            run(L"--explicit",child_environment);
            selected();
        }
    }
    char verdict[180];sprintf(verdict,"PS5_PROTON_INITIAL_ENV %s checks=%u failures=%u\n",failures?"FAIL":"PASS",checks,failures);
    OutputDebugStringA(verdict);ExitProcess(failures?1:0);
}
