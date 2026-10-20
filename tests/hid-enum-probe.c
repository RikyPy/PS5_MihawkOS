/* SPDX-License-Identifier: GPL-3.0-or-later
 * What a game's controller search finds, step by step, and where it stops (the report of 2026-10-13: a .NET (MonoGame) game, started in the first
 * session of a new prefix, never acquired the DualSense, while the same game in every later session did). Run inside the Mihawk
 * session, as the program it starts or beside a game:
 *
 *   hid-enum-probe [--rounds=N] [--every=MS]
 *
 * Each round does what DirectInput's joystick enumeration does, and prints the result of every step: the HID interfaces SetupAPI
 * lists as present, for each one CreateFile, HidD_GetPreparsedData, HidD_GetAttributes, HidP_GetCaps and HidD_GetProductString with
 * GetLastError; then DirectInput8's own EnumDevices(DI8DEVCLASS_GAMECTRL, DIEDFL_ATTACHEDONLY), and CreateDevice and Acquire on what it
 * returns. A line is printed for each step; the verdict line says PASS when a round acquired a controller. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <setupapi.h>
#include <ddk/hidsdi.h>
#include <ddk/hidpi.h>
#include <dinput.h>
#include <winternl.h>

static void line(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static BOOL starts_with(const char *text, const char *prefix)
{
    while (*prefix) if (*text++ != *prefix++) return FALSE;
    return TRUE;
}

static int number_after(const char *command, const char *name, int fallback)
{
    const char *c;
    for (c = command; *c; ++c) {
        if ((c == command || c[-1] == ' ') && starts_with(c, name)) {
            int value = 0;
            c += lstrlenA(name);
            while (*c >= '0' && *c <= '9') value = value * 10 + (*c++ - '0');
            return value;
        }
    }
    return fallback;
}

static int found_controllers, acquired;

static void say(const char *what, int round, const char *detail, DWORD error)
{
    char text[700];
    wsprintfA(text, "hid-enum round %d: %s %s error=%lu\n", round, what, detail, error);
    line(text);
}


/* Where does the interface's link lead, and is there something at the other end? ("path not found" on a listed interface is a link whose
 * target is gone.) */
static void explain_link(int round, const WCHAR *dos_path)
{
    WCHAR nt_path[400], target[400];
    UNICODE_STRING name, target_name;
    OBJECT_ATTRIBUTES attributes;
    HANDLE link = NULL, file = NULL;
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    char text[500];
    int length = 0;

    nt_path[0] = '\\'; nt_path[1] = '?'; nt_path[2] = '?'; nt_path[3] = '\\';
    while (dos_path[length + 4] && length < 380) { nt_path[4 + length] = dos_path[length + 4]; ++length; }
    nt_path[4 + length] = 0;
    RtlInitUnicodeString(&name, nt_path);
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = NtOpenSymbolicLinkObject(&link, 0x0001, &attributes);
    if (status) { wsprintfA(text, "NtOpenSymbolicLinkObject status=%08lx", status); say("link", round, text, 0); return; }
    target_name.Buffer = target; target_name.Length = 0; target_name.MaximumLength = sizeof(target);
    status = NtQuerySymbolicLinkObject(link, &target_name, NULL);
    NtClose(link);
    if (status) { wsprintfA(text, "NtQuerySymbolicLinkObject status=%08lx", status); say("link", round, text, 0); return; }
    target[target_name.Length / sizeof(WCHAR)] = 0;
    InitializeObjectAttributes(&attributes, &target_name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = NtCreateFile(&file, SYNCHRONIZE | FILE_READ_ATTRIBUTES, &attributes, &io, NULL, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, NULL, 0);
    if (!status) NtClose(file);
    {
        char shown[300];
        WideCharToMultiByte(CP_ACP, 0, target, -1, shown, sizeof(shown), NULL, NULL);
        wsprintfA(text, "target %s, opening it status=%08lx", shown, status);
    }
    say("link", round, text, 0);
}

static void search_hid(int round)
{
    static const GUID hid = {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
    HDEVINFO set = SetupDiGetClassDevsW(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA iface = {sizeof(iface)};
    DWORD index;
    char text[300];

    if (set == INVALID_HANDLE_VALUE) { say("SetupDiGetClassDevs failed", round, "", GetLastError()); return; }
    for (index = 0; SetupDiEnumDeviceInterfaces(set, NULL, &hid, index, &iface); ++index) {
        union { SP_DEVICE_INTERFACE_DETAIL_DATA_W detail; BYTE bytes[1024]; } buffer;
        char path[260];
        HANDLE file;
        PHIDP_PREPARSED_DATA preparsed = NULL;
        HIDD_ATTRIBUTES attrs = {sizeof(attrs)};
        HIDP_CAPS caps;
        WCHAR product[128];

        buffer.detail.cbSize = sizeof(buffer.detail);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, &buffer.detail, sizeof(buffer), NULL, NULL)) {
            say("SetupDiGetDeviceInterfaceDetail failed", round, "", GetLastError());
            continue;
        }
        WideCharToMultiByte(CP_ACP, 0, buffer.detail.DevicePath, -1, path, sizeof(path), NULL, NULL);
        say("listed", round, path, 0);
        file = CreateFileW(buffer.detail.DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, 0);
        if (file == INVALID_HANDLE_VALUE) { say("CreateFile failed", round, "", GetLastError()); explain_link(round, buffer.detail.DevicePath); continue; }
        SetLastError(0);
        if (!HidD_GetPreparsedData(file, &preparsed)) { say("HidD_GetPreparsedData failed", round, "", GetLastError()); CloseHandle(file); continue; }
        SetLastError(0);
        if (!HidD_GetAttributes(file, &attrs)) say("HidD_GetAttributes failed", round, "", GetLastError());
        else { wsprintfA(text, "vid=%04x pid=%04x version=%04x", attrs.VendorID, attrs.ProductID, attrs.VersionNumber); say("attributes", round, text, 0); }
        if (HidP_GetCaps(preparsed, &caps) != HIDP_STATUS_SUCCESS) say("HidP_GetCaps failed", round, "", 0);
        else { wsprintfA(text, "usage page %04x usage %04x input %u output %u", caps.UsagePage, caps.Usage, caps.InputReportByteLength, caps.OutputReportByteLength); say("caps", round, text, 0); }
        SetLastError(0);
        if (!HidD_GetProductString(file, product, sizeof(product))) say("HidD_GetProductString failed", round, "", GetLastError());
        else { WideCharToMultiByte(CP_ACP, 0, product, -1, text, sizeof(text), NULL, NULL); say("product", round, text, 0); }
        ++found_controllers;
        HidD_FreePreparsedData(preparsed);
        CloseHandle(file);
    }
    wsprintfA(text, "%lu interfaces listed", index);
    say("SetupAPI end", round, text, GetLastError());
    SetupDiDestroyDeviceInfoList(set);
}

static BOOL CALLBACK enum_device(const DIDEVICEINSTANCEW *instance, void *context)
{
    IDirectInput8W *di = context;
    IDirectInputDevice8W *device = NULL;
    char name[200], text[300];
    HRESULT result;

    WideCharToMultiByte(CP_ACP, 0, instance->tszProductName, -1, name, sizeof(name), NULL, NULL);
    wsprintfA(text, "'%s' type %lx product %08lx", name, instance->dwDevType, instance->guidProduct.Data1);
    say("DirectInput lists", -1, text, 0);
    result = IDirectInput8_CreateDevice(di, &instance->guidInstance, &device, NULL);
    if (FAILED(result)) { wsprintfA(text, "hr=%08lx", result); say("CreateDevice failed", -1, text, 0); return DIENUM_CONTINUE; }
    IDirectInputDevice8_SetDataFormat(device, &c_dfDIJoystick2);
    result = IDirectInputDevice8_Acquire(device);
    wsprintfA(text, "hr=%08lx", result);
    say(SUCCEEDED(result) ? "Acquire ok" : "Acquire failed", -1, text, 0);
    if (SUCCEEDED(result)) { ++acquired; IDirectInputDevice8_Unacquire(device); }
    IDirectInputDevice8_Release(device);
    return DIENUM_CONTINUE;
}

static void search_directinput(int round)
{
    IDirectInput8W *di = NULL;
    HRESULT result = DirectInput8Create(GetModuleHandleW(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8W, (void **)&di, NULL);
    char text[100];

    if (FAILED(result)) { wsprintfA(text, "hr=%08lx", result); say("DirectInput8Create failed", round, text, 0); return; }
    result = IDirectInput8_EnumDevices(di, DI8DEVCLASS_GAMECTRL, enum_device, di, DIEDFL_ATTACHEDONLY);
    wsprintfA(text, "hr=%08lx", result);
    say("EnumDevices end", round, text, 0);
    IDirectInput8_Release(di);
}

void mainCRTStartup(void)
{
    char command[MAX_PATH * 2], text[200];
    int rounds, every, round;

    lstrcpynA(command, GetCommandLineA(), sizeof(command));
    rounds = number_after(command, "--rounds=", 10);
    every = number_after(command, "--every=", 3000);
    for (round = 1; round <= rounds; ++round) {
        search_hid(round);
        search_directinput(round);
        Sleep((DWORD)every);
    }
    wsprintfA(text, "hid-enum note: %d rounds, %d controller opens, %d acquires\n", rounds, found_controllers, acquired);
    line(text);
    line(acquired ? "PS5_PROTON_HID_ENUM PASS\n" : "PS5_PROTON_HID_ENUM FAIL\n");
    ExitProcess(acquired ? 0 : 1);
}
