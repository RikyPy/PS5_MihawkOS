/* SPDX-License-Identifier: GPL-3.0-or-later
 * Same PE32/PE64 request, without graphics: the driver's internal SendHardwareInput path must deliver
 * WM_INPUT, its HID bytes and device-change parameters across WoW64. INPUT's union starts at byte 4
 * in PE32 and byte 8 in PE64. Passing the inner pointer unchanged reads WM_INPUT as zero, flooding
 * the message pump when a HID driver sends reports. Each request has a matching observable reply. */
#define __WINESRC__
#include <windows.h>
#include <winternl.h>
#include <ntuser.h>

static void verdict(const char *reason, int ok, unsigned checks)
{
    char line[256]; DWORD written;
    wsprintfA(line, "PS5_PROTON_INPUT_MESSAGE_PROBE %s: bits=%u checks=%u %s error=%lu\n",
              ok ? "PASS" : "FAIL", (unsigned)(sizeof(void *) * 8), checks, reason, GetLastError());
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, lstrlenA(line), &written, NULL);
    OutputDebugStringA(line);
    ExitProcess(ok ? 0 : 1);
}

static int receive(HWND window, UINT wanted, WPARAM wparam, LPARAM lparam, DWORD payload)
{
    DWORD start = GetTickCount(); MSG msg;
    do {
        if (!PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { Sleep(1); continue; }
        if (msg.hwnd == window && msg.message == wanted) {
            if (wanted == WM_INPUT) {
                BYTE storage[256]; UINT size = sizeof(storage);
                RAWINPUT *raw = (void *)storage;
                if (GetRawInputData((HRAWINPUT)msg.lParam, RID_INPUT, raw, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1)
                    return 0;
                DWORD value = 0;
                if (raw->header.dwType != RIM_TYPEHID || raw->data.hid.dwCount != 1 || raw->data.hid.dwSizeHid != 4)
                    return 0;
                CopyMemory(&value, raw->data.hid.bRawData, sizeof(value));
                if (value != payload) return 0;
            } else if (msg.wParam != wparam || msg.lParam != lparam) return 0;
            DispatchMessageA(&msg);
            return 1;
        }
        TranslateMessage(&msg); DispatchMessageA(&msg);
    } while (GetTickCount() - start < 1000);
    return 0;
}

void mainCRTStartup(void)
{
    WNDCLASSA cls = {0}; cls.lpfnWndProc = DefWindowProcA;
    cls.hInstance = GetModuleHandleA(NULL); cls.lpszClassName = "ps5_input_message_probe";
    if (!RegisterClassA(&cls)) verdict("RegisterClass", 0, 0);
    HWND window = CreateWindowA(cls.lpszClassName, "Input message probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                0, 0, 320, 180, NULL, NULL, cls.hInstance, NULL);
    if (!window) verdict("CreateWindow", 0, 0);
    SetForegroundWindow(window); SetFocus(window);
    RAWINPUTDEVICE device = {0xff00, 1, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window};
    if (!RegisterRawInputDevices(&device, 1, sizeof(device))) verdict("RegisterRawInputDevices", 0, 0);
    struct { struct hid_input head; DWORD data; } packet = {{0x7788, MAKELONG(1, 0xff00), 1, 4}, 0};
    unsigned checks = 0;
    for (unsigned i = 0; i < 20; ++i) {
        INPUT input = {0}; input.type = INPUT_HARDWARE; input.hi.uMsg = WM_INPUT;
        packet.head.count = 1; packet.head.length = 4; packet.data = 0xa5810000u | i;
        if (NtUserSendHardwareInput(window, 0, &input, (LPARAM)&packet)) verdict("raw input request", 0, checks);
        if (!receive(window, WM_INPUT, 0, 0, packet.data)) verdict("raw input reply", 0, checks);
        ++checks;
        for (unsigned change = GIDC_ARRIVAL; change <= GIDC_REMOVAL; ++change) {
            input.hi.uMsg = WM_INPUT_DEVICE_CHANGE; input.hi.wParamL = change;
            packet.head.count = packet.head.length = 0;
            if (NtUserSendHardwareInput(window, 0, &input, (LPARAM)&packet)) verdict("device change request", 0, checks);
            if (!receive(window, WM_INPUT_DEVICE_CHANGE, change, packet.head.device, 0)) verdict("device change reply", 0, checks);
            ++checks;
        }
    }
    device.dwFlags = RIDEV_REMOVE; device.hwndTarget = NULL;
    RegisterRawInputDevices(&device, 1, sizeof(device));
    DestroyWindow(window);
    verdict("hardware input preserved", 1, checks);
}
