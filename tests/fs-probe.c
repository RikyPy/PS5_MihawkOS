/* SPDX-License-Identifier: GPL-3.0-or-later
 * Walks a directory tree the way Java's NIO does (FindFirstFileW, a handle on
 * each directory, CreateFileW and ReadFile on each file), reports every call
 * that fails with its error, and measures how many handles one process can
 * hold open at once. FS_PROBE_ROOT is the tree (an absolute Windows path);
 * FS_PROBE_HANDLES=1 also holds FS_PROBE_COUNT (default 600, at most 2000)
 * files open at once in the temporary directory and checks every one's
 * content and position. Ends by itself. */
#include <windows.h>

static unsigned dirs, files, errors, bytes_read;

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void number(char *out, unsigned value)
{
    char digits[12];
    int count = 0;
    do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (count) *out++ = digits[--count];
    *out = 0;
}

static void report(const char *what, const WCHAR *path, DWORD error)
{
    static char line[1200];
    char code[12];
    int length = 0, i;

    errors++;
    number(code, error);
    for (i = 0; what[i]; i++) line[length++] = what[i];
    line[length++] = ' ';
    length += WideCharToMultiByte(CP_UTF8, 0, path, -1, line + length, 900, NULL, NULL) - 1;
    line[length++] = ' ';
    line[length++] = '=';
    line[length++] = '>';
    line[length++] = ' ';
    for (i = 0; code[i]; i++) line[length++] = code[i];
    line[length++] = '\n';
    line[length] = 0;
    say(line);
}

static void join(WCHAR *out, const WCHAR *directory, const WCHAR *name)
{
    int length = lstrlenW(directory);
    lstrcpyW(out, directory);
    if (length && out[length - 1] != '\\') out[length++] = '\\';
    lstrcpyW(out + length, name);
}

static void read_file(const WCHAR *path)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    static char buffer[65536];
    DWORD got;

    if (file == INVALID_HANDLE_VALUE) { report("CreateFileW", path, GetLastError()); return; }
    if (!ReadFile(file, buffer, sizeof(buffer), &got, NULL)) report("ReadFile", path, GetLastError());
    else bytes_read += got;
    files++;
    CloseHandle(file);
}

static void walk(const WCHAR *directory)
{
    static WCHAR pattern[4][2048];
    WIN32_FIND_DATAW found;
    HANDLE find, handle;
    BY_HANDLE_FILE_INFORMATION information;
    static int depth;
    int mine = depth++;

    /* what Java does for a directory: a handle with backup semantics and its attributes */
    handle = CreateFileW(directory, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) report("CreateFileW(dir)", directory, GetLastError());
    else {
        if (!GetFileInformationByHandle(handle, &information)) report("GetFileInformationByHandle", directory, GetLastError());
        CloseHandle(handle);
    }
    dirs++;
    join(pattern[mine], directory, L"*");
    find = FindFirstFileW(pattern[mine], &found);
    if (find == INVALID_HANDLE_VALUE) { report("FindFirstFileW", directory, GetLastError()); depth--; return; }
    do {
        static WCHAR path[4][2048];
        if (!lstrcmpW(found.cFileName, L".") || !lstrcmpW(found.cFileName, L"..")) continue;
        join(path[mine], directory, found.cFileName);
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (mine < 3) walk(path[mine]); }
        else read_file(path[mine]);
    } while (FindNextFileW(find, &found));
    if (GetLastError() != ERROR_NO_MORE_FILES) report("FindNextFileW", directory, GetLastError());
    FindClose(find);
    depth--;
}

/* Many files held open at once, as Java holds its jars and region files: each
 * is created with 64 bytes, then every handle is read in a scrambled order,
 * several times, without seeking in between, so that what the file position
 * does across the runtime's open-file limit is visible. The limit is about
 * 256 files per process (ps5platform/fdv.h); a count above it passes only
 * when files can be parked and reopened without losing content or offsets. */
#define MAX_FILES 2000

static unsigned long long scramble_state = 88172645463325252ULL;

static unsigned scramble(unsigned bound)
{
    scramble_state ^= scramble_state << 13;
    scramble_state ^= scramble_state >> 7;
    scramble_state ^= scramble_state << 17;
    return (unsigned)(scramble_state % bound);
}

static void file_name(WCHAR *out, const WCHAR *directory, unsigned index)
{
    WCHAR name[24] = L"f0000.bin";
    name[1] = (WCHAR)(L'0' + index / 1000 % 10);
    name[2] = (WCHAR)(L'0' + index / 100 % 10);
    name[3] = (WCHAR)(L'0' + index / 10 % 10);
    name[4] = (WCHAR)(L'0' + index % 10);
    join(out, directory, name);
}

static void expected_text(char *out, unsigned index)
{
    unsigned i;
    for (i = 0; i < 64; i++) out[i] = (char)('A' + (index * 7 + i) % 26);
}

static void many_files(const WCHAR *directory, unsigned wanted)
{
    static HANDLE held[MAX_FILES];
    static WCHAR path[2048];
    static unsigned position[MAX_FILES];
    char text[160], digits[12], expected[64], got[8];
    unsigned count = 0, i, round, bad = 0, failed_open = 0;
    DWORD error = 0, done;

    if (wanted > MAX_FILES) wanted = MAX_FILES;
    CreateDirectoryW(directory, NULL);
    for (i = 0; i < wanted; i++) {
        file_name(path, directory, i);
        held[i] = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (held[i] == INVALID_HANDLE_VALUE) { error = GetLastError(); failed_open++; break; }
        expected_text(expected, i);
        if (!WriteFile(held[i], expected, 64, &done, NULL) || done != 64) bad++;
        count++;
    }
    for (i = 0; i < count; i++) {
        if (SetFilePointer(held[i], 0, NULL, FILE_BEGIN) != 0) bad++;
        position[i] = 0;
    }
    /* eight rounds of eight bytes: after a file was last used its descriptor
     * may have been parked, and its position must have survived */
    for (round = 0; round < 8; round++) {
        unsigned step;
        for (step = 0; step < count; step++) {
            unsigned k = scramble(count), j;
            expected_text(expected, k);
            if (!ReadFile(held[k], got, 8, &done, NULL) || done != 8) { bad++; continue; }
            for (j = 0; j < 8; j++)
                if (got[j] != expected[position[k] + j]) { bad++; break; }
            position[k] += 8;
            if (position[k] >= 64) {
                if (SetFilePointer(held[k], 0, NULL, FILE_BEGIN) != 0) bad++;
                position[k] = 0;
            }
        }
    }
    number(digits, count);
    lstrcpyA(text, bad || failed_open ? "PS5_PROTON_FS_PROBE FAIL: " : "PS5_PROTON_FS_PROBE PASS: ");
    lstrcatA(text, digits);
    lstrcatA(text, " files held open at once");
    if (failed_open) { char code[12]; number(code, error); lstrcatA(text, ", open failed with error "); lstrcatA(text, code); }
    number(digits, bad);
    lstrcatA(text, ", content or position errors: ");
    lstrcatA(text, digits);
    lstrcatA(text, "\n");
    say(text);
    for (i = 0; i < count; i++) {
        CloseHandle(held[i]);
        file_name(path, directory, i);
        DeleteFileW(path);
    }
    RemoveDirectoryW(directory);
    if (bad || failed_open) errors++;
}

void mainCRTStartup(void)
{
    static WCHAR root[2048], flag[8];
    char text[96], a[12], b[12], c[12], d[12];

    if (!GetEnvironmentVariableW(L"FS_PROBE_ROOT", root, 2048)) lstrcpyW(root, L"Z:\\data\\ps5proton\\games\\7\\instances\\26.3\\game\\saves");
    walk(root);
    number(a, dirs); number(b, files); number(c, bytes_read); number(d, errors);
    lstrcpyA(text, errors ? "PS5_PROTON_FS_PROBE FAIL: dirs=" : "PS5_PROTON_FS_PROBE PASS: dirs=");
    lstrcatA(text, a); lstrcatA(text, " files="); lstrcatA(text, b); lstrcatA(text, " bytes="); lstrcatA(text, c);
    lstrcatA(text, " errors="); lstrcatA(text, d); lstrcatA(text, "\n");
    say(text);
    if (GetEnvironmentVariableW(L"FS_PROBE_HANDLES", flag, 8)) {
        static WCHAR directory[2048], count_text[12];
        unsigned wanted = 600;
        if (GetEnvironmentVariableW(L"FS_PROBE_COUNT", count_text, 12)) {
            unsigned i;
            wanted = 0;
            for (i = 0; count_text[i] >= L'0' && count_text[i] <= L'9'; i++) wanted = wanted * 10 + (unsigned)(count_text[i] - L'0');
        }
        GetTempPathW(2048, directory);
        lstrcatW(directory, L"fs-probe");
        many_files(directory, wanted);
    }
    ExitProcess(errors ? 1 : 0);
}
