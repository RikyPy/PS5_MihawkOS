/* SPDX-License-Identifier: GPL-3.0-or-later
 * What a program is told about the machine's memory. A launcher's install of a Unity game said "Your RAM: 665 MB,
 * Minimum: 8 GB" on the 16 GB console (2026-10-11): the figure a program reads is GlobalMemoryStatusEx's
 * ullTotalPhys (SystemBasicInformation's page count times the page size), and GetPhysicallyInstalledSystemMemory
 * derives from it. The probe prints every figure and checks them against each other and against what the console
 * has, not a number of its own: the total is at least 8 GiB, the free memory is within it, the commit limit is
 * not below it (Titan Quest refuses otherwise) and the load percentage agrees with the two. */
#include <windows.h>

WINBASEAPI BOOL WINAPI GetPhysicallyInstalledSystemMemory(PULONGLONG);

static char line[700];
static int failures;

static void need(const char *what, int ok)
{
    if (ok) return;
    ++failures;
    lstrcatA(line, " "); lstrcatA(line, what); lstrcatA(line, ";");
}

void mainCRTStartup(void)
{
    MEMORYSTATUSEX status = {sizeof(status)};
    SYSTEM_INFO info;
    ULONGLONG installed_kib = 0;
    BOOL ok = GlobalMemoryStatusEx(&status);
    BOOL installed = GetPhysicallyInstalledSystemMemory(&installed_kib);
    char out[1200];
    DWORD load_expected;

    GetNativeSystemInfo(&info);
    need("GlobalMemoryStatusEx", ok);
    need("total at least 8 GiB", status.ullTotalPhys >= 8ull << 30);
    need("free within total", status.ullAvailPhys <= status.ullTotalPhys);
    need("free not zero", status.ullAvailPhys != 0);
    need("commit limit above total", status.ullTotalPageFile > status.ullTotalPhys);
    need("free commit within limit", status.ullAvailPageFile <= status.ullTotalPageFile);
    need("installed memory agrees", installed && installed_kib == status.ullTotalPhys >> 10);
    {   /* in MiB, so no 64-bit division is needed (the PE32 build has no runtime) */
        DWORD total_mib = (DWORD)(status.ullTotalPhys >> 20), free_mib = (DWORD)(status.ullAvailPhys >> 20);
        load_expected = total_mib ? (total_mib - free_mib) * 100 / total_mib : 0;
        need("load percentage agrees", status.dwMemoryLoad + 1 >= load_expected && status.dwMemoryLoad <= load_expected + 1);
    }

    wsprintfA(out, "PS5_PROTON_SYSTEM_MEMORY %s: failures=%d total_mib=%lu free_mib=%lu commit_limit_mib=%lu commit_free_mib=%lu "
              "virtual_mib=%lu load=%lu installed_kib=%lu page_size=%lu%s\n",
              failures ? "FAIL" : "PASS", failures,
              (unsigned long)(status.ullTotalPhys >> 20), (unsigned long)(status.ullAvailPhys >> 20),
              (unsigned long)(status.ullTotalPageFile >> 20), (unsigned long)(status.ullAvailPageFile >> 20),
              (unsigned long)(status.ullTotalVirtual >> 20), status.dwMemoryLoad, (unsigned long)installed_kib,
              info.dwPageSize, line);
    OutputDebugStringA(out);
    ExitProcess(failures ? 1 : 0);
}
