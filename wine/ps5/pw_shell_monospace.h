/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_SHELL_MONOSPACE_H
#define PW_SHELL_MONOSPACE_H
#include <windows.h>
/* Cascadia Mono as the monospace face (OFL-1.1, themes/windows11/fonts/cascadia, staged in C:\windows\Fonts by
 * tools/stage-wine.py), named once per prefix by the session root (Wine patch 1138), as Windows 11 has it: the
 * console's face (HKCU\Console FaceName), and Consolas, which programs ask for and no prefix has, substituted by it
 * (FontSubstitutes). A face someone chose, or a Consolas that is installed, is kept; a runtime without the font
 * leaves both alone and is tried again next session. tests/monospace-probe.c runs this very function. */
static DWORD pw_shell_prepare_monospace(void)
{
    HKEY settings = NULL, key = NULL;
    DWORD initialized = 0, size = sizeof(initialized), type = 0, status;
    WCHAR path[MAX_PATH];
    UINT length;
    static const WCHAR face[] = L"Cascadia Mono";

    status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\MihawkOS", 0, NULL, 0,
                             KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &settings, NULL);
    if (status != ERROR_SUCCESS) return status;
    if (RegQueryValueExW(settings, L"MonospaceInitialized", NULL, &type, (BYTE *)&initialized, &size) == ERROR_SUCCESS &&
        type == REG_DWORD && size == sizeof(initialized) && initialized == 1)
    { RegCloseKey(settings); return ERROR_SUCCESS; }
    length = GetWindowsDirectoryW(path, MAX_PATH);
    if (!length || length + 32 >= MAX_PATH) { RegCloseKey(settings); return ERROR_INSUFFICIENT_BUFFER; }
    lstrcatW(path, L"\\Fonts\\CascadiaMono-Regular.ttf");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) { RegCloseKey(settings); return ERROR_SUCCESS; }
    path[length] = 0;
    lstrcatW(path, L"\\Fonts\\consola.ttf");
    status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes", 0,
                             NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key, NULL);
    if (status == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(key, L"Consolas", NULL, NULL, NULL, NULL) == ERROR_FILE_NOT_FOUND &&
            GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
            status = RegSetValueExW(key, L"Consolas", 0, REG_SZ, (BYTE *)face, sizeof(face));
        RegCloseKey(key);
    }
    if (status == ERROR_SUCCESS)
        status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Console", 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key, NULL);
    if (status == ERROR_SUCCESS)
    {
        if (RegQueryValueExW(key, L"FaceName", NULL, NULL, NULL, NULL) == ERROR_FILE_NOT_FOUND)
            status = RegSetValueExW(key, L"FaceName", 0, REG_SZ, (BYTE *)face, sizeof(face));
        RegCloseKey(key);
    }
    if (status == ERROR_SUCCESS)
    {
        initialized = 1;
        status = RegSetValueExW(settings, L"MonospaceInitialized", 0, REG_DWORD, (BYTE *)&initialized, sizeof(initialized));
    }
    RegCloseKey(settings);
    return status;
}
#endif
