/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <windows.h>
#include <setupapi.h>
#include "ddk/hidsdi.h"


void mainCRTStartup(void)
{
    GUID guid;
    unsigned found = 0;
    HidD_GetHidGuid(&guid);
    for (unsigned retry = 0; retry < 50 && !found; ++retry) {
        HDEVINFO set = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        SP_DEVICE_INTERFACE_DATA iface = {.cbSize = sizeof(iface)};
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, NULL, &guid, i, &iface); ++i) {
            DWORD size = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &iface, NULL, 0, &size, NULL);
            PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = HeapAlloc(GetProcessHeap(), 0, size);
            if (!detail) continue;
            detail->cbSize = sizeof(*detail);
            if (SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, NULL, NULL)) {
                HANDLE dev = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
                HIDD_ATTRIBUTES attr = {.Size = sizeof(attr)};
                if (dev != INVALID_HANDLE_VALUE && HidD_GetAttributes(dev, &attr) &&
                    attr.VendorID == 0x054c && attr.ProductID == 0x0ce6) {
                    WCHAR product[128] = {0};
                    PHIDP_PREPARSED_DATA data;
                    HIDP_CAPS caps = {0};
                    BOOL named = HidD_GetProductString(dev, product, sizeof(product));
                    DWORD name_error = GetLastError();
                    BOOL parsed = HidD_GetPreparsedData(dev, &data);
                    DWORD parse_error = GetLastError();
                    BOOL ok = parsed;
                    if (parsed) { ok = HidP_GetCaps(data, &caps) == HIDP_STATUS_SUCCESS; HidD_FreePreparsedData(data); }
                    char diagnostic[512];
                    wsprintfA(diagnostic, "HID diagnostic: name=%S named=%u error=%lu parsed=%u error=%lu bytes=%u\n",
                        product, named, name_error, parsed, parse_error, caps.InputReportByteLength);
                    OutputDebugStringA(diagnostic);
                    /* Wine normalizes Sony product strings to the physical USB name. */
                    ok = ok && named && (!lstrcmpW(product, L"Wireless Controller") ||
                                         !lstrcmpW(product, L"DualSense Wireless Controller"));
                    ok = ok && caps.UsagePage == 1 && caps.Usage == 5 && caps.InputReportByteLength == 64;
                    BYTE report[64] = {0};
                    DWORD read = 0;
                    OVERLAPPED ov = {.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL)};
                    if (ok) {
                        BOOL ready = ReadFile(dev, report, sizeof(report), &read, &ov);
                        if (!ready && GetLastError() == ERROR_IO_PENDING && WaitForSingleObject(ov.hEvent, 3000) == WAIT_OBJECT_0)
                            ready = GetOverlappedResult(dev, &ov, &read, FALSE);
                        ok = ready && read == 64 && report[0] == 1;
                        CancelIo(dev);
                    }
                    CloseHandle(ov.hEvent);
                    char message[256];
                    wsprintfA(message, "PS5_PROTON_HID_PROBE %s: DualSense 054c:0ce6 usage=%u:%u report=%lu id=%u axes=%u,%u buttons=%02x,%02x\r\n",
                        ok ? "PASS" : "FAIL", caps.UsagePage, caps.Usage, read, report[0], report[1], report[2], report[8], report[9]);
                    OutputDebugStringA(message);
                    DWORD written; WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, lstrlenA(message), &written, NULL);
                    found = ok ? 1 : 2;
                }
                if (dev != INVALID_HANDLE_VALUE) CloseHandle(dev);
            }
            HeapFree(GetProcessHeap(), 0, detail);
        }
        SetupDiDestroyDeviceInfoList(set);
        if (!found) Sleep(100);
    }
    if (!found) { OutputDebugStringA("PS5_PROTON_HID_PROBE FAIL: DualSense not enumerated\n"); ExitProcess(1); }
    ExitProcess(found == 1 ? 0 : 1);
}
