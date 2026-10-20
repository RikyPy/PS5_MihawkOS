/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <windows.h>
#include <xinput.h>

void mainCRTStartup(void)
{
    HMODULE module = LoadLibraryW(L"xinput1_4.dll");
    DWORD (WINAPI *get_state)(DWORD, XINPUT_STATE *) = module ?
        (void *)GetProcAddress(module, "XInputGetState") : NULL;
    XINPUT_STATE state;
    BOOL ok = get_state && get_state(0, &state) == ERROR_SUCCESS &&
        get_state(1, &state) == ERROR_DEVICE_NOT_CONNECTED;
    const char *message = ok ? "PS5_PROTON_XINPUT_PROBE PASS: controller 0 connected, controller 1 absent\r\n"
                             : "PS5_PROTON_XINPUT_PROBE FAIL: controller bridge unavailable\r\n";
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, lstrlenA(message), &written, NULL);
    OutputDebugStringA(message);
    if (module) FreeLibrary(module);
    ExitProcess(ok ? 0 : 1);
}
