/* SPDX-License-Identifier: GPL-3.0-or-later
 * File name resolution, from the places Wine's conformance tests found it wanting on the console:
 * creating a file by a relative name (ntdll virtual test: "Failed to create test file"), opening a
 * name relative to a directory handle (ntdll path test: STATUS_INVALID_HANDLE for every name given
 * with a RootDirectory), and a file name with a trailing backslash (opened where Windows refuses).
 * Prints the status or error of each, with what the same call does by an absolute name. Ends by itself. */
#include <windows.h>

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } UNICODE_STRING_;
typedef struct { ULONG Length; HANDLE RootDirectory; UNICODE_STRING_ *ObjectName; ULONG Attributes; PVOID SecurityDescriptor, SecurityQualityOfService; } OBJECT_ATTRIBUTES_;
typedef struct { LONG_PTR Status; ULONG_PTR Information; } IO_STATUS_BLOCK_;

static LONG (WINAPI *NtOpenFile_)(HANDLE *, ACCESS_MASK, OBJECT_ATTRIBUTES_ *, IO_STATUS_BLOCK_ *, ULONG, ULONG);
static LONG (WINAPI *NtClose_)(HANDLE);

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void hex(char *out, ULONG_PTR value)
{
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 8; i++) out[i] = digits[(value >> (28 - 4 * i)) & 15];
    out[8] = 0;
}

static void result(const char *what, ULONG_PTR value)
{
    char text[200], number[12];
    lstrcpyA(text, "PS5_PROTON_NT_FILE_PROBE ");
    lstrcatA(text, what);
    lstrcatA(text, " -> ");
    hex(number, value);
    lstrcatA(text, number);
    lstrcatA(text, "\n");
    say(text);
}

static LONG open_nt(const WCHAR *root_name, const WCHAR *name, ULONG options, HANDLE *out)
{
    UNICODE_STRING_ root_string, string;
    OBJECT_ATTRIBUTES_ attributes = { sizeof(attributes), NULL, &string, 0x40 /* OBJ_CASE_INSENSITIVE */, NULL, NULL };
    IO_STATUS_BLOCK_ io;
    HANDLE root = NULL;
    LONG status = 0;

    *out = NULL;
    if (root_name) {
        OBJECT_ATTRIBUTES_ root_attributes = { sizeof(root_attributes), NULL, &root_string, 0x40, NULL, NULL };
        root_string.Buffer = (PWSTR)root_name;
        root_string.Length = (USHORT)(lstrlenW(root_name) * 2);
        root_string.MaximumLength = root_string.Length + 2;
        status = NtOpenFile_(&root, SYNCHRONIZE | FILE_LIST_DIRECTORY, &root_attributes, &io, FILE_SHARE_READ,
                             0x20 /* SYNCHRONOUS_IO_NONALERT */ | 0x4000 /* OPEN_FOR_BACKUP_INTENT */ | 1 /* DIRECTORY_FILE */);
        if (status) return status;
        attributes.RootDirectory = root;
    }
    if (name) {
        string.Buffer = (PWSTR)name;
        string.Length = (USHORT)(lstrlenW(name) * 2);
        string.MaximumLength = string.Length + 2;
        status = NtOpenFile_(out, FILE_GENERIC_READ, &attributes, &io, FILE_SHARE_READ, 0x20 | options);
    }
    if (root) NtClose_(root);
    return status;
}

void mainCRTStartup(void)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    HANDLE file, handle;
    WCHAR directory[260];
    DWORD length;
    LONG status;
    static const char *const names[] = { ".\\nt-file-probe.tmp", "nt-file-probe.tmp", "C:\\nt-file-probe.tmp", "C:\\windows\\temp\\nt-file-probe.tmp" };
    int i;

    NtOpenFile_ = (void *)GetProcAddress(ntdll, "NtOpenFile");
    NtClose_ = (void *)GetProcAddress(ntdll, "NtClose");
    length = GetCurrentDirectoryW(260, directory);
    result("GetCurrentDirectoryW length", length);
    if (length && length < 260) {
        char text[300];
        int j;
        for (j = 0; j <= (int)length; j++) text[j] = (char)directory[j];
        lstrcpyA(text + length, "\n");
        say("PS5_PROTON_NT_FILE_PROBE current directory: ");
        say(text);
    }
    for (i = 0; i < 4; i++) {
        SetLastError(0);
        file = CreateFileA(names[i], GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, 0);
        say("PS5_PROTON_NT_FILE_PROBE CreateFileA ");
        say(names[i]);
        say("\n");
        result("  handle", (ULONG_PTR)file);
        result("  error", GetLastError());
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            DeleteFileA(names[i]);
        }
    }
    status = open_nt(L"\\??\\C:\\", NULL, 0, &handle);
    result("open directory \\??\\C:\\ (status)", (ULONG_PTR)status);
    status = open_nt(NULL, L"\\??\\C:\\windows\\system32\\kernel32.dll", 0x40 /* NON_DIRECTORY_FILE */, &handle);
    result("open \\??\\C:\\windows\\system32\\kernel32.dll by its full name", (ULONG_PTR)status);
    if (handle) NtClose_(handle);
    status = open_nt(L"\\??\\C:\\", L"windows\\system32\\kernel32.dll", 0x40, &handle);
    result("open windows\\system32\\kernel32.dll relative to \\??\\C:\\", (ULONG_PTR)status);
    if (handle) NtClose_(handle);
    status = open_nt(L"\\??\\C:\\windows", L"system32\\kernel32.dll", 0x40, &handle);
    result("open system32\\kernel32.dll relative to \\??\\C:\\windows", (ULONG_PTR)status);
    if (handle) NtClose_(handle);
    status = open_nt(L"\\??\\C:\\windows", L"system32", 1 /* DIRECTORY_FILE */, &handle);
    result("open system32 (a directory) relative to \\??\\C:\\windows", (ULONG_PTR)status);
    if (handle) NtClose_(handle);
    status = open_nt(NULL, L"\\??\\C:\\windows\\system32\\kernel32.dll\\", 0x40, &handle);
    result("open a file name with a trailing backslash (Windows: name invalid)", (ULONG_PTR)status);
    if (handle) NtClose_(handle);
    say("PS5_PROTON_NT_FILE_PROBE PASS: reported\n");
    ExitProcess(0);
}
