/* SPDX-License-Identifier: GPL-3.0-or-later
 * One launch, many probes (tools/run-battery.py --grouped): C:\gate-sequence.txt names one probe a line,
 * "<case><TAB><budget seconds><TAB><command line>", and each runs in turn as its own Windows process (in the launcher
 * layout, its own helper), started as a launcher starts a game, with this program's standard handles, and waited
 * for. Around each, OutputDebugString says "begin case=<case>" and "end case=<case> status=<exit code>"; the Wine
 * timestamps of those lines are the window the battery reads that probe's own verdict in. A probe still running
 * after its budget ends the sequence through the title's own stop request (Z: is the console's root, so this is the
 * runner's /data/ps5proton/stop-game): the title asks the game to close and, 10 s later, ends its helpers, as the
 * runner's watchdog would for that probe alone. The last line is PASS when every probe was started and waited for,
 * whatever each one's verdict, which is the battery's to judge; FAIL when the list could not be read, a probe could
 * not be started, or one ran out of its budget. */
#include <windows.h>

static char list[65536];

static void say(const char *line)
{
    OutputDebugStringA(line);
}

void mainCRTStartup(void)
{
    char line[1024];
    DWORD size = 0, ran = 0, failed = 0;
    HANDLE file = CreateFileA("C:\\gate-sequence.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE || !ReadFile(file, list, sizeof(list) - 1, &size, NULL) || !size ||
        size == sizeof(list) - 1) {
        wsprintfA(line, "PS5_PROTON_GATE_SEQUENCE FAIL: list unreadable error=%lu size=%lu\n", GetLastError(), size);
        say(line);
        ExitProcess(1);
    }
    CloseHandle(file);
    list[size] = 0;
    for (char *next = list; *next;) {
        char *name = next, *command, *end = name;
        while (*end && *end != '\n') ++end;
        next = *end ? end + 1 : end;
        *end = 0;
        if (end > name && end[-1] == '\r') end[-1] = 0;
        DWORD budget = 0;
        for (command = name; *command && *command != '\t'; ++command) {}
        if (*command) for (*command++ = 0; *command >= '0' && *command <= '9'; ++command) budget = budget * 10 + *command - '0';
        if (*command != '\t' || !budget) {
            if (*name) { ++failed; wsprintfA(line, "PS5_PROTON_GATE_SEQUENCE malformed line: %.200s\n", name); say(line); }
            continue;
        }
        ++command;
        STARTUPINFOA startup = {0};
        PROCESS_INFORMATION process;
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        wsprintfA(line, "PS5_PROTON_GATE_SEQUENCE begin case=%.100s\n", name);
        say(line);
        if (!CreateProcessA(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) {
            ++failed;
            wsprintfA(line, "PS5_PROTON_GATE_SEQUENCE end case=%.100s status=start-failed error=%lu\n", name,
                      GetLastError());
            say(line);
            continue;
        }
        DWORD status = ~0u;
        if (WaitForSingleObject(process.hProcess, budget * 1000) == WAIT_TIMEOUT) {
            HANDLE stop = CreateFileA("Z:\\data\\ps5proton\\stop-game", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
            wsprintfA(line, "PS5_PROTON_GATE_SEQUENCE end case=%.100s status=timeout budget=%lu stop_requested=%d\n", name,
                      budget, stop != INVALID_HANDLE_VALUE);
            say(line);
            if (stop != INVALID_HANDLE_VALUE) CloseHandle(stop);
            wsprintfA(line, "PS5_PROTON_GATE_SEQUENCE FAIL ran=%lu failed=%lu\n", ran, failed + 1);
            say(line);
            ExitProcess(1);
        }
        GetExitCodeProcess(process.hProcess, &status);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        ++ran;
        wsprintfA(line, "PS5_PROTON_GATE_SEQUENCE end case=%.100s status=%lu\n", name, status);
        say(line);
    }
    wsprintfA(line, "PS5_PROTON_GATE_SEQUENCE %s ran=%lu failed=%lu\n", failed || !ran ? "FAIL" : "PASS", ran, failed);
    say(line);
    ExitProcess(failed || !ran ? 1 : 0);
}
