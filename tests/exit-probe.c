/* SPDX-License-Identifier: GPL-3.0-or-later
 * The shortest PE64 process: its verdict, then ExitProcess at once, while
 * the in-process HID/PnP stack is still starting. It must return to the
 * launcher like any other process. */
#include <windows.h>

void mainCRTStartup(void)
{
    static const char message[] = "PS5_PROTON_EXIT_PROBE PASS: exiting immediately\r\n";
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, sizeof(message) - 1, &written, NULL);
    OutputDebugStringA(message);
    ExitProcess(0);
}
