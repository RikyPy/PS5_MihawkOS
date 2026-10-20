/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What a 32-bit program needs of the runtime beyond its own process (docs/IA32_DESIGN.md, M4 step 2), checked
 * from 32-bit code under WoW64. Each check prints one line; the last is "PS5_PROTON_WOW64_PROBE PASS" or
 * "... FAIL <checks>", which tools/run-console.py reads.
 *
 *   child     a 32-bit child process (this program, "child") starts in its own helper and ends with code 32
 *   registry  the prefix's 32-bit registry view: HKLM\Software\Microsoft\Windows NT\CurrentVersion read from
 *             32-bit code (WoW64 redirects it to Wow6432Node, which the prefix's WoW64 install creates)
 *   audio     a 200 ms buffer played through winmm, mmdevapi and the PS5 audio driver's 32-bit entry points
 *   xinput    controller 0 connected and controller 1 absent through the 32-bit xinput1_4 (the title's pad)
 * Built by tools/build-wine-probe.sh with clang --target=i686-windows, no C runtime. */
#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>

static HANDLE out;
static char line[256];
static unsigned used;

static void add(const char *text)
{
    while (*text && used < sizeof(line) - 1) line[used++] = *text++;
    line[used] = 0;
}

static void add_hex(unsigned value)
{
    char text[11] = "0x";
    for (int i = 0; i < 8; i++) text[2 + i] = "0123456789abcdef"[(value >> (28 - 4 * i)) & 15];
    text[10] = 0;
    add(text);
}

/* One line to standard output and to the debugger channel: a helper's verdict reaches the run's log through
 * +debugstr (pe32-probe.c). */
static void flush_line(void)
{
    DWORD written;
    add("\r\n");
    WriteFile(out, line, used, &written, NULL);
    OutputDebugStringA(line);
    used = 0;
    line[0] = 0;
}

static int check_child(void)
{
    WCHAR path[MAX_PATH], command[MAX_PATH + 16];
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION info;
    DWORD code = 0, wait = WAIT_FAILED;
    BOOL started;

    GetModuleFileNameW(NULL, path, MAX_PATH);
    lstrcpyW(command, L"\"");
    lstrcatW(command, path);
    lstrcatW(command, L"\" child");
    started = CreateProcessW(path, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &info);
    if (started) {
        wait = WaitForSingleObject(info.hProcess, 60000);
        GetExitCodeProcess(info.hProcess, &code);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
    }
    add("child started="); add(started ? "1" : "0");
    if (!started) { add(" error="); add_hex(GetLastError()); }
    add(" exit="); add_hex(code); add(wait == WAIT_OBJECT_0 ? "" : " (did not end)");
    flush_line();
    return started && wait == WAIT_OBJECT_0 && code == 32;
}

static int check_registry(void)
{
    HKEY key;
    WCHAR value[64];
    DWORD size = sizeof(value), type = 0;
    LONG opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion", 0,
                                KEY_READ | KEY_WOW64_32KEY, &key);
    LONG read = ERROR_FILE_NOT_FOUND;

    if (!opened) {
        read = RegQueryValueExW(key, L"CurrentVersion", NULL, &type, (BYTE *)value, &size);
        RegCloseKey(key);
    }
    add("registry open="); add_hex(opened); add(" CurrentVersion="); add_hex(read);
    flush_line();
    return !opened && !read && type == REG_SZ;
}

static int check_audio(void)
{
    static short samples[44100 / 5 * 2];   /* 200 ms of 16-bit stereo silence */
    WAVEFORMATEX format = { WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0 };
    WAVEHDR header = { (char *)samples, sizeof(samples) };
    HWAVEOUT wave = NULL;
    const UINT devices = waveOutGetNumDevs();
    MMRESULT opened = waveOutOpen(&wave, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL), written = MMSYSERR_ERROR;
    int done = 0;

    if (!opened) {
        if (!waveOutPrepareHeader(wave, &header, sizeof(header)))
            written = waveOutWrite(wave, &header, sizeof(header));
        for (int i = 0; !written && i < 300 && !(header.dwFlags & WHDR_DONE); i++) Sleep(10);
        done = !!(header.dwFlags & WHDR_DONE);
        waveOutReset(wave);
        waveOutUnprepareHeader(wave, &header, sizeof(header));
        waveOutClose(wave);
    }
    add("audio devices="); add_hex(devices); add(" open="); add_hex(opened); add(" write="); add_hex(written);
    add(" played="); add(done ? "1" : "0");
    flush_line();
    return devices && !opened && !written && done;
}

static int check_xinput(void)
{
    HMODULE module = LoadLibraryW(L"xinput1_4.dll");
    DWORD (WINAPI *get_state)(DWORD, XINPUT_STATE *) = module ? (void *)GetProcAddress(module, "XInputGetState") : NULL;
    XINPUT_STATE state;
    const DWORD first = get_state ? get_state(0, &state) : ~0u, second = get_state ? get_state(1, &state) : ~0u;

    add("xinput loaded="); add(get_state ? "1" : "0"); add(" pad0="); add_hex(first); add(" pad1="); add_hex(second);
    flush_line();
    if (module) FreeLibrary(module);
    return first == ERROR_SUCCESS && second == ERROR_DEVICE_NOT_CONNECTED;
}

int mainCRTStartup(void)
{
    const WCHAR *command = GetCommandLineW();
    out = GetStdHandle(STD_OUTPUT_HANDLE);
    for (const WCHAR *p = command; *p; p++)
        if (p[0] == ' ' && p[1] == 'c' && p[2] == 'h' && p[3] == 'i' && p[4] == 'l' && p[5] == 'd' && !p[6])
            ExitProcess(sizeof(void *) == 4 ? 32 : 64);   /* the child: which side it ran on */

    const int child = check_child(), registry = check_registry(), audio = check_audio(), xinput = check_xinput();
    if (child && registry && audio && xinput)
        add("PS5_PROTON_WOW64_PROBE PASS: 32-bit child process, registry view, audio, XInput");
    else {
        add("PS5_PROTON_WOW64_PROBE FAIL");
        if (!child) add(" child");
        if (!registry) add(" registry");
        if (!audio) add(" audio");
        if (!xinput) add(" xinput");
    }
    flush_line();
    ExitProcess(child && registry && audio && xinput ? 0 : 1);
    return 0;
}
