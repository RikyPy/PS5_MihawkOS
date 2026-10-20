/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_SHELL_WALLPAPER_H
#define PW_SHELL_WALLPAPER_H
#include <windows.h>
/* The Mihawk desktop session's background, initialized once per prefix (formerly the session's Windows mihawk.exe;
 * now Explorer's, as the title's session root with MIHAWK_SESSION=1, Wine patch 0996). Existing nonempty wallpaper
 * choices survive, and later explicit empty choices too. The local image is optional and never vendored. Explorer
 * paints through Wine's real desktop APIs; the broker has no wallpaper authority or process-image fallback.
 * tests/wallpaper-probe.c runs this very function against the real registry. */
static DWORD pw_shell_prepare_wallpaper(void)
{
    HKEY settings = NULL, desktop = NULL;
    DWORD initialized = 0, size = sizeof(initialized), type = 0, status;
    WCHAR existing[MAX_PATH], path[MAX_PATH];
    existing[0] = 0;
    status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\MihawkOS", 0, NULL, 0,
                            KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &settings, NULL);
    if (status != ERROR_SUCCESS) return status;
    if (RegQueryValueExW(settings, L"WallpaperInitialized", NULL, &type,
                        (BYTE *)&initialized, &size) == ERROR_SUCCESS &&
        type == REG_DWORD && size == sizeof(initialized) && initialized == 1)
    { RegCloseKey(settings); return ERROR_SUCCESS; }
    status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, NULL, 0,
                            KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &desktop, NULL);
    if (status == ERROR_SUCCESS)
    {
        size = sizeof(existing);
        DWORD queried = RegQueryValueExW(desktop, L"Wallpaper", NULL, &type, (BYTE *)existing, &size);
        if (queried == ERROR_FILE_NOT_FOUND ||
            (queried == ERROR_SUCCESS && type == REG_SZ && !existing[0]))
        {
            UINT length = GetWindowsDirectoryW(path, MAX_PATH);
            const WCHAR *suffix = L"\\web\\wallpaper\\proton.bmp";
            if (!length || length + lstrlenW(suffix) >= MAX_PATH) status = ERROR_INSUFFICIENT_BUFFER;
            else
            {
                lstrcatW(path, suffix);
                if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
                {
                    status = GetLastError();
                    /* The local wallpaper is optional and is never vendored.
                     * A checkout without it still starts the real desktop. */
                    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
                        status = ERROR_SUCCESS;
                }
                else status = RegSetValueExW(desktop, L"Wallpaper", 0, REG_SZ, (BYTE *)path,
                                             (lstrlenW(path) + 1) * sizeof(WCHAR));
            }
        }
        else if (queried != ERROR_SUCCESS && queried != ERROR_MORE_DATA) status = queried;
        RegCloseKey(desktop);
    }
    if (status == ERROR_SUCCESS)
    {
        initialized = 1;
        status = RegSetValueExW(settings, L"WallpaperInitialized", 0, REG_DWORD,
                                (BYTE *)&initialized, sizeof(initialized));
    }
    RegCloseKey(settings);
    return status;
}
#endif
