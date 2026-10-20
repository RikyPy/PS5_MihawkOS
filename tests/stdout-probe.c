/* SPDX-License-Identifier: GPL-3.0-or-later
 * What a Windows program's standard handles are on the console. The title has no usable file
 * descriptors 1 and 2, and Wine's conformance tests print every result with printf, so the
 * first process's standard output has to be something that works (patch 0900: a file).
 * Reports the three handles, writes a line to each of standard output and error, and then
 * opens the file patch 0900 writes by its unix path, to tell whether that name resolves.
 * Ends by itself. */
#include <windows.h>

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static int contains(const char *text, const char *word)
{
    for (; *text; ++text) {
        unsigned i = 0;
        while (word[i] && text[i] == word[i]) ++i;
        if (!word[i]) return 1;
    }
    return 0;
}

static void hex(char *out, ULONG_PTR value)
{
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 16; i++) out[i] = digits[(value >> (60 - 4 * i)) & 15];
    out[16] = 0;
}

static void line(const char *name, ULONG_PTR value)
{
    char text[96], number[20];
    lstrcpyA(text, "PS5_PROTON_STDOUT_PROBE ");
    lstrcatA(text, name);
    lstrcatA(text, " ");
    hex(number, value);
    lstrcatA(text, number);
    lstrcatA(text, "\n");
    say(text);
}

/* Redirect a descendant's actual standard handles. WINE_PS5_STDOUT must not
 * replace these with the initial process's logging file. */
static int redirected_child(void)
{
    SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
    HANDLE read = NULL, write = NULL;
    WCHAR path[MAX_PATH], command[MAX_PATH + 32];
    STARTUPINFOW startup = {0}; PROCESS_INFORMATION process = {0};
    DWORD bytes = 0, used = 0, code = 1;
    char output[128] = {0};
    if (!CreatePipe(&read, &write, &security, 0)) return 0;
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wsprintfW(command, L"\"%s\" --pipe-child", path);
    startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = write;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    BOOL created = CreateProcessW(path, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                                 NULL, NULL, &startup, &process);
    CloseHandle(write);
    if (created) {
        while (used < sizeof(output)-1 && ReadFile(read, output+used, sizeof(output)-1-used, &bytes, NULL) && bytes)
            used += bytes;
        if (WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0)
            GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hProcess); CloseHandle(process.hThread);
    }
    CloseHandle(read);
    return created && !code && !lstrcmpA(output, "child stdout\nchild stderr\n");
}

void mainCRTStartup(void)
{
    static const char out_text[] = "stdout probe: a line on standard output\n";
    static const char err_text[] = "stdout probe: a line on standard error\n";
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE), file;
    DWORD written = 0, error;
    BOOL ok;
    int passed = 1;

    if (contains(GetCommandLineA(), "--pipe-child")) {
        static const char a[] = "child stdout\n", b[] = "child stderr\n";
        ok = WriteFile(out, a, sizeof(a)-1, &written, NULL) && written == sizeof(a)-1;
        ok = ok && WriteFile(err, b, sizeof(b)-1, &written, NULL) && written == sizeof(b)-1;
        ExitProcess(ok ? 0 : 1);
    }

    line("input handle", (ULONG_PTR)in);
    line("output handle", (ULONG_PTR)out);
    line("error handle", (ULONG_PTR)err);
    SetLastError(0);
    ok = WriteFile(out, out_text, sizeof(out_text) - 1, &written, NULL);
    error = GetLastError();
    passed = ok && written == sizeof(out_text) - 1;
    line("WriteFile(output) result", ok);
    line("  written", written);
    line("  error", error);
    SetLastError(0);
    ok = WriteFile(err, err_text, sizeof(err_text) - 1, &written, NULL);
    passed = passed && ok && written == sizeof(err_text) - 1;
    line("WriteFile(error) result", ok);
    line("  error", GetLastError());
    SetLastError(0);
    file = CreateFileW(L"\\??\\unix\\data\\ps5proton\\logs\\stdout-probe.txt", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    line("CreateFile(unix path) handle", (ULONG_PTR)file);
    line("  error", GetLastError());
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, "created by stdout-probe\n", 24, &written, NULL);
        CloseHandle(file);
    }
    if (contains(GetCommandLineA(), "--redirected")) {
        int child_ok = redirected_child();
        line("redirected child writes", child_ok);
        passed = passed && child_ok;
    }
    say(passed ? "PS5_PROTON_STDOUT_PROBE PASS: both standard handles accepted complete writes\n"
               : "PS5_PROTON_STDOUT_PROBE FAIL: a standard handle rejected a write\n");
    ExitProcess(passed ? 0 : 1);
}
