/* SPDX-License-Identifier: GPL-3.0-or-later
 * Wine's own reading of a .lnk file, the oracle for src/pw_shell_link.c (tools/check-shell-link.sh). It loads the
 * file as Explorer's load_shelllink does (programs/explorer/desktop.c:296: CoCreateInstance(CLSID_ShellLink),
 * IPersistFile::Load with mode 0) and prints what IShellLinkW gives back, one "field=value" line each, as UTF-8:
 *
 *   path               GetPath(SLGP_RAWPATH), as extract_icon reads it
 *   arguments          GetArguments
 *   working_directory  GetWorkingDirectory
 *   icon_path          GetIconLocation
 *   icon_index         GetIconLocation, decimal
 *   description        GetDescription
 *
 *   shell-link-oracle.exe <file.lnk>     exit 0; 2 usage; 3 COM; 4 the load failed; 5 a getter failed
 *   shell-link-oracle.exe --open <file.lnk>
 *                                        opens the link as Explorer's double click does (desktop.c:835,
 *                                        ShellExecuteW(NULL, "open", link, NULL, NULL, 0)); exit 0, or 6 when
 *                                        ShellExecuteW returned 32 or less */
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

/* Spelled out: no uuid import library is built for the probes. */
static const GUID oracle_clsid_shell_link = {0x00021401, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID oracle_iid_shell_link_w = {0x000214F9, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID oracle_iid_persist_file = {0x0000010B, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

static WCHAR text[65536];          /* a string is at most 32767 units (dlls/shell32/shelllink.c:503) */
static char utf8[65536 * 3 + 16];

static void put(DWORD handle, const char *bytes, int size)
{
    DWORD written;
    WriteFile(GetStdHandle(handle), bytes, (DWORD)size, &written, NULL);
}

static void put_text(DWORD handle, const char *text_a)
{
    put(handle, text_a, lstrlenA(text_a));
}

static void put_hex(DWORD handle, unsigned long value)
{
    char digits[11] = "0x";
    for (int i = 0; i < 8; ++i) digits[2 + i] = "0123456789abcdef"[(value >> (28 - 4 * i)) & 0xF];
    put(handle, digits, 10);
}

static void field(const char *name, const WCHAR *value)
{
    put_text(STD_OUTPUT_HANDLE, name);
    put(STD_OUTPUT_HANDLE, "=", 1);
    int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8, sizeof utf8, NULL, NULL);
    if (size > 1) put(STD_OUTPUT_HANDLE, utf8, size - 1);
    put(STD_OUTPUT_HANDLE, "\n", 1);
}

static void fail(const char *what, HRESULT hr, UINT code)
{
    put_text(STD_ERROR_HANDLE, "shell-link-oracle: ");
    put_text(STD_ERROR_HANDLE, what);
    put_text(STD_ERROR_HANDLE, " failed, hr ");
    put_hex(STD_ERROR_HANDLE, (unsigned long)hr);
    put_text(STD_ERROR_HANDLE, "\n");
    ExitProcess(code);
}

void mainCRTStartup(void)
{
    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc == 3 && !lstrcmpW(argv[1], L"--open")) {
        HINSTANCE opened = ShellExecuteW(NULL, L"open", argv[2], NULL, NULL, 0);
        if ((UINT_PTR)opened <= 32) fail("ShellExecuteW", (HRESULT)(UINT_PTR)opened, 6);
        ExitProcess(0);
    }
    if (!argv || argc != 2) {
        put_text(STD_ERROR_HANDLE, "usage: shell-link-oracle [--open] <file.lnk>\n");
        ExitProcess(2);
    }
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    HRESULT hr = CoInitialize(NULL);
    if (FAILED(hr)) fail("CoInitialize", hr, 3);
    hr = CoCreateInstance(&oracle_clsid_shell_link, NULL, CLSCTX_INPROC_SERVER, &oracle_iid_shell_link_w,
                          (void **)&link);
    if (FAILED(hr)) fail("CoCreateInstance(CLSID_ShellLink)", hr, 3);
    hr = IShellLinkW_QueryInterface(link, &oracle_iid_persist_file, (void **)&file);
    if (FAILED(hr)) fail("QueryInterface(IPersistFile)", hr, 3);
    hr = IPersistFile_Load(file, argv[1], 0);
    if (FAILED(hr)) fail("IPersistFile::Load", hr, 4);

    text[0] = 0;
    hr = IShellLinkW_GetPath(link, text, ARRAYSIZE(text), NULL, SLGP_RAWPATH);
    if (FAILED(hr)) fail("GetPath", hr, 5);
    field("path", text);
    text[0] = 0;
    hr = IShellLinkW_GetArguments(link, text, ARRAYSIZE(text));
    if (FAILED(hr)) fail("GetArguments", hr, 5);
    field("arguments", text);
    text[0] = 0;
    hr = IShellLinkW_GetWorkingDirectory(link, text, ARRAYSIZE(text));
    if (FAILED(hr)) fail("GetWorkingDirectory", hr, 5);
    field("working_directory", text);
    int index = 0x7FFFFFFF;
    text[0] = 0;
    hr = IShellLinkW_GetIconLocation(link, text, ARRAYSIZE(text), &index);
    if (FAILED(hr)) fail("GetIconLocation", hr, 5);
    field("icon_path", text);
    WCHAR number[16], *p = number + 15;
    unsigned magnitude = index < 0 ? 0u - (unsigned)index : (unsigned)index;
    *p = 0;
    do *--p = (WCHAR)('0' + magnitude % 10); while (magnitude /= 10);
    if (index < 0) *--p = '-';
    field("icon_index", p);
    text[0] = 0;
    hr = IShellLinkW_GetDescription(link, text, ARRAYSIZE(text));
    if (FAILED(hr)) fail("GetDescription", hr, 5);
    field("description", text);

    IPersistFile_Release(file);
    IShellLinkW_Release(link);
    CoUninitialize();
    LocalFree(argv);
    ExitProcess(0);
}
