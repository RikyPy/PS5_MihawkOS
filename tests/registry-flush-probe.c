/* SPDX-License-Identifier: GPL-3.0-or-later
* A registry value set and kept (no RegFlushKey: that writes the hive from the client, and programs that set a value do not call it): it writes HKCU\Software\MihawkOS\FlushProbe, "stamp", then idles for 60 s and ends.
 * tools/run-registry-flush.py reads the prefix's user.reg over FTP while this still runs: the value must be on the
 * disk by about 35 s, not only when the last process ends. The wineserver saves its registry only at its exit, so a
 * session that was cut short lost what it wrote (a launcher ran its install scripts again, 2026-10-10); the title asks for
 * a save every 30 s since 2026-10-11. */
#include <windows.h>

void mainCRTStartup(void)
{
    HKEY key;
    char out[200];
    DWORD stamp = GetTickCount(), disposition;
    LONG status = RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\MihawkOS\\FlushProbe", 0, NULL, 0, KEY_SET_VALUE, NULL,
                                  &key, &disposition);
    if (!status) status = RegSetValueExA(key, "stamp", 0, REG_DWORD, (const BYTE *)&stamp, sizeof(stamp));
    wsprintfA(out, "PS5_PROTON_REGISTRY_FLUSH %s: status=%ld stamp=%lu\n", status ? "FAIL" : "PASS", status, stamp);
    OutputDebugStringA(out);
    Sleep(60000);
    ExitProcess(status ? 1 : 0);
}
