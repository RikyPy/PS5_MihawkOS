/* SPDX-License-Identifier: GPL-3.0-or-later
 * tools/run-games-autodetect.py plants this as C:\Games\GateAutodetect\gate.exe, a game folder with no game.json
 * (src/pw_game_folders.h), and the desktop session runs it: it says where it runs from, then closes the session's
 * shell, which ends the session as End session does, rather than leaving it to the run's budget and a stop. */
#include <windows.h>

static int contains(const char *text, const char *part)
{
    for (; *text; ++text) {
        const char *a = text, *b = part;
        while (*a && *b && (*a | 0x20) == (*b | 0x20)) ++a, ++b;
        if (!*b) return 1;
    }
    return 0;
}

void mainCRTStartup(void)
{
    char path[MAX_PATH], line[MAX_PATH + 96];
    DWORD written;
    path[0] = 0;   /* no initializer: it would call memset, and nothing here links a C library */
    const int ok = GetModuleFileNameA(NULL, path, sizeof(path)) && contains(path, "\\Games\\GateAutodetect\\gate.exe");
    wsprintfA(line, "PS5_PROTON_GAMES_AUTODETECT %s: started from %s\r\n", ok ? "PASS" : "FAIL", path);
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, lstrlenA(line), &written, NULL);
    OutputDebugStringA(line);
    for (int i = 0; i < 200 && !GetShellWindow(); ++i) Sleep(50);
    if (GetShellWindow()) PostMessageW(GetShellWindow(), WM_CLOSE, 0, 0);
    ExitProcess(ok ? 0 : 1);
}
