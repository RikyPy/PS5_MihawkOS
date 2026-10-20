/* SPDX-License-Identifier: GPL-3.0-or-later
 * A prefix Wine made on the console, the service manager, and an installer (docs/WINDOWS_PROCESSES.md, "Prefixes,
 * services and installers"). Run as a library entry whose prefix did not exist, it checks what wineboot made: the
 * system folder and its modules, the registry's Windows version, the user's folders; then that the service control
 * manager answers and lists services; then it installs an MSI package beside it (the first *.msi in its own folder)
 * with msiexec, silently, and runs a program the package installed. Each step prints a line; it passes when all do.
 * MSI: msiexec /i <package> /qn /l*v C:\prefix-probe-msi.log; the program: the first "<Program Files>\<name>\<exe>"
 * named on the command line after --run (e.g. --run 7-Zip\7z.exe), run with "i" and expected to exit 0. With
 * --no-services (the title layout, which starts no services) the service manager is reported but not required. With
 * --trace CHANNELS, msiexec runs with those Wine debug channels added (Wine hands a child its own WINEDEBUG). A
 * program that has not ended in time is ended, so a stalled step fails the probe instead of holding the title. */
#include <windows.h>

static int failures;

static void say(const char *text)
{
    OutputDebugStringA(text);
}

static void check(int ok, const char *what)
{
    char line[400];
    wsprintfA(line, "prefix-probe: %s: %s\n", what, ok ? "yes" : "NO");
    say(line);
    if (!ok) ++failures;
}

static int exists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* Runs command and waits; its exit code, or ~0u when it did not start or did not end in time. */
static DWORD run(char *command, DWORD milliseconds)
{
    STARTUPINFOA startup = {sizeof(startup)};
    PROCESS_INFORMATION process;
    DWORD code = ~0u;
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        char line[160];
        wsprintfA(line, "prefix-probe: could not start a program (error %lu)\n", GetLastError());
        say(line);
        return ~0u;
    }
    if (WaitForSingleObject(process.hProcess, milliseconds) == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &code);
    else {
        say("prefix-probe: the program did not end in time; ending it\n");
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 10000);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}

void mainCRTStartup(void)
{
    char line[600], folder[MAX_PATH], package[MAX_PATH] = "", value[256], *command = GetCommandLineA(), *program = NULL;
    DWORD size = sizeof(value), type = 0;
    HKEY key;

    /* The prefix wineboot made. */
    check(exists("C:\\windows\\system32\\kernel32.dll") && exists("C:\\windows\\system32\\msiexec.exe"),
          "system32 has its modules");
    check(!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &key) &&
          !RegQueryValueExA(key, "ProductName", NULL, &type, (BYTE *)value, &size) && type == REG_SZ && value[0],
          "the registry names a Windows version");
    if (type == REG_SZ) { wsprintfA(line, "prefix-probe: ProductName \"%s\"\n", value); say(line); }
    RegCloseKey(key);
    size = GetEnvironmentVariableA("APPDATA", folder, sizeof(folder));
    check(size && size < sizeof(folder) && exists(folder), "the user's application data folder exists");

    /* The service control manager. */
    {
        typedef SC_HANDLE (WINAPI *Open)(const char *, const char *, DWORD);
        typedef BOOL (WINAPI *Enum)(SC_HANDLE, SC_ENUM_TYPE, DWORD, DWORD, BYTE *, DWORD, DWORD *, DWORD *, DWORD *,
                                    const char *);
        typedef BOOL (WINAPI *Close)(SC_HANDLE);
        HMODULE advapi = LoadLibraryA("advapi32.dll");
        Open open = (Open)GetProcAddress(advapi, "OpenSCManagerA");
        Enum enumerate = (Enum)GetProcAddress(advapi, "EnumServicesStatusExA");
        Close close = (Close)GetProcAddress(advapi, "CloseServiceHandle");
        SC_HANDLE manager = open ? open(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE) : NULL;
        DWORD needed = 0, count = 0, resume = 0;
        static BYTE services[65536];
        wsprintfA(line, "prefix-probe: OpenSCManager %s (error %lu)\n", manager ? "answered" : "failed",
                  manager ? 0 : GetLastError());
        say(line);
        const BOOL listed = manager && enumerate(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32 | SERVICE_DRIVER,
                                                 SERVICE_STATE_ALL, services, sizeof(services), &needed, &count,
                                                 &resume, NULL);
        wsprintfA(line, "prefix-probe: services listed %lu\n", listed ? count : 0);
        say(line);
        int required = 1;
        for (const char *p = command; *p; ++p)
            if (!memcmp(p, "--no-services", 13)) required = 0;
        if (required) check(manager && listed && count > 0, "the service control manager lists services");
        else say("prefix-probe: services are not started in this layout; not required\n");
        if (manager) close(manager);
    }

    /* An installer beside this program, and a program it installed. */
    GetModuleFileNameA(NULL, folder, sizeof(folder));
    { char *slash = folder; for (char *p = folder; *p; ++p) if (*p == '\\') slash = p; *slash = 0; }
    {
        WIN32_FIND_DATAA found;
        char pattern[MAX_PATH];
        wsprintfA(pattern, "%s\\*.msi", folder);
        HANDLE search = FindFirstFileA(pattern, &found);
        if (search != INVALID_HANDLE_VALUE) {
            wsprintfA(package, "%s\\%s", folder, found.cFileName);
            FindClose(search);
        }
    }
    for (char *p = command; *p; ++p)
        if (p[0] == '-' && p[1] == '-' && p[2] == 'r' && p[3] == 'u' && p[4] == 'n' && p[5] == ' ') { program = p + 6; break; }
    if (package[0] && program) {
        char install[700], installed[MAX_PATH], files[MAX_PATH];
        GetEnvironmentVariableA("ProgramFiles", files, sizeof(files));
        wsprintfA(installed, "%s\\%s", files, program);
        for (char *p = installed; *p; ++p) if (*p == ' ' && p > installed + lstrlenA(files)) { *p = 0; break; }
        wsprintfA(install, "msiexec /i \"%s\" /qn /l*v C:\\prefix-probe-msi.log", package);
        for (const char *p = command; *p; ++p)
            if (!memcmp(p, "--trace ", 8)) {
                char channels[200], debug[260];
                int length = 0;
                for (p += 8; *p && *p != ' ' && length < (int)sizeof(channels) - 1; ++p) channels[length++] = *p;
                channels[length] = 0;
                wsprintfA(debug, "+timestamp,+pid,+tid,+debugstr,+loaddll,%s", channels);
                SetEnvironmentVariableA("WINEDEBUG", debug);
                wsprintfA(line, "prefix-probe: msiexec traces %s\n", channels);
                say(line);
                break;
            }
        const DWORD installer = run(install, 300000);
        SetEnvironmentVariableA("WINEDEBUG", NULL);
        wsprintfA(line, "prefix-probe: msiexec exit code %lu\n", installer);
        say(line);
        check(installer == 0 || installer == ERROR_SUCCESS_REBOOT_REQUIRED, "msiexec installed the package");
        check(exists(installed), "the package's program is in Program Files");
        char start[MAX_PATH + 8];
        wsprintfA(start, "\"%s\" i", installed);
        const DWORD ran = exists(installed) ? run(start, 60000) : ~0u;
        wsprintfA(line, "prefix-probe: the installed program exit code %lu\n", ran);
        say(line);
        check(ran == 0, "the installed program ran");
    } else {
        say("prefix-probe: no package beside the probe or no --run program; installer steps left out\n");
        ++failures;
    }
    say(failures ? "PS5_PROTON_PREFIX_PROBE FAIL\n"
                 : "PS5_PROTON_PREFIX_PROBE PASS: a prefix wineboot made, the service manager, an MSI installed and run\n");
    ExitProcess(failures ? 1 : 0);
}
