/* SPDX-License-Identifier: GPL-3.0-or-later
 * WriteFile on regular files must mean what it means on Windows whatever the host does underneath: on the PS5 a write
 * goes into a shared mapping of the file (patch 1007) and everything below must still hold. Byte-exact checks of the
 * contents and the size, seen through another handle, a file mapping, and a second process, while the writing handle is
 * still open: sequential writes of every size from 1 byte to 70,000, an overwrite in the middle, a gap past the end,
 * a FILE_APPEND_DATA handle, SetEndOfFile shrinking then writing again, delete and recreate under the same name,
 * a writer that is closed and one that is not. Run as `file-write-probe.exe` (the test) or
 * `file-write-probe.exe child PATH SIZE SEED` (reads PATH and checks it). Prints the speed of 64 KiB writes too. */
#include <windows.h>

int _fltused; /* floating point without the C runtime */
static char report[1500];
static int failures;

static void bad(const char *what, DWORD a, DWORD b)
{
    failures++;
    if (lstrlenA(report) < 1200) wsprintfA(report + lstrlenA(report), " [%s %lu %lu]", what, a, b);
}

/* the byte the file holds at offset o when it was written with this seed */
static BYTE pattern(DWORD o, DWORD seed) { return (BYTE)(o * 131u + (o >> 8) * 7u + seed); }

static DWORD random_next(DWORD *state) { *state = *state * 1664525u + 1013904223u; return *state >> 8; }

static BOOL write_pattern(HANDLE file, DWORD from, DWORD length, DWORD seed)
{
    static BYTE block[70000];
    DWORD done, i;
    for (i = 0; i < length; i++) block[i] = pattern(from + i, seed);
    return WriteFile(file, block, length, &done, NULL) && done == length;
}

/* the file at path holds `size` bytes, each as the pattern says (seed), except where `skip_to` says: bytes below it are not checked */
static BOOL check_file(const char *path, DWORD size, DWORD seed, DWORD skip_from, DWORD skip_to, const char *what)
{
    static BYTE block[65536];
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD got, at = 0, i, wrong = 0;
    BOOL ok = TRUE;
    if (file == INVALID_HANDLE_VALUE) { bad(what, 1000, GetLastError()); return FALSE; }
    if (GetFileSize(file, NULL) != size) { bad(what, 1001, GetFileSize(file, NULL)); ok = FALSE; }
    while (ReadFile(file, block, sizeof(block), &got, NULL) && got)
    {
        for (i = 0; i < got; i++)
            if ((at + i < skip_from || at + i >= skip_to) && block[i] != pattern(at + i, seed)) wrong++;
        at += got;
    }
    if (at != size) { bad(what, 1002, at); ok = FALSE; }
    if (wrong) { bad(what, 1003, wrong); ok = FALSE; }
    CloseHandle(file);
    return ok;
}

static int child_main(char *command)
{
    char path[260];
    DWORD size = 0, seed = 0, i = 0;
    while (*command == ' ') command++;
    while (*command && *command != ' ' && i < sizeof(path) - 1) path[i++] = *command++;
    path[i] = 0;
    while (*command == ' ') command++;
    while (*command >= '0' && *command <= '9') size = size * 10 + (DWORD)(*command++ - '0');
    while (*command == ' ') command++;
    while (*command >= '0' && *command <= '9') seed = seed * 10 + (DWORD)(*command++ - '0');
    return check_file(path, size, seed, 0, 0, "child") ? 0 : 3;
}

/* "watch PATH SIZE": after the parent's event, how long until a fresh open sees SIZE bytes; 0 within 100 ms, 4 later, 5 never */
static int watch_main(char *command)
{
    char path[260];
    DWORD size = 0, i = 0, began, seen = 0;
    HANDLE go = OpenEventA(SYNCHRONIZE, FALSE, "fwp-go");
    while (*command == ' ') command++;
    while (*command && *command != ' ' && i < sizeof(path) - 1) path[i++] = *command++;
    path[i] = 0;
    while (*command == ' ') command++;
    while (*command >= '0' && *command <= '9') size = size * 10 + (DWORD)(*command++ - '0');
    if (!go || WaitForSingleObject(go, 10000) != WAIT_OBJECT_0) return 6;
    began = GetTickCount();
    while (GetTickCount() - began < 1500)
    {
        HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        DWORD now_size = f == INVALID_HANDLE_VALUE ? 0 : GetFileSize(f, NULL);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        if (now_size >= size) { seen = GetTickCount() - began; return seen <= 100 ? 0 : 4; }
        Sleep(2);
    }
    return 5;
}

void mainCRTStartup(void)
{
    char *command = GetCommandLineA();
    static char out[1700];
    HANDLE writer, other;
    DWORD state = 12345, size = 0, done, n, i, want_bytes;
    LARGE_INTEGER frequency, begin, end;
    double seconds;

    {   /* the arguments after the program name: "child ..." runs the checking child */
        char *p = command;
        if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; } else while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        if (p[0] == 'c' && p[1] == 'h' && p[2] == 'i' && p[3] == 'l' && p[4] == 'd') ExitProcess((UINT)child_main(p + 5));
        if (p[0] == 'w' && p[1] == 'a' && p[2] == 't' && p[3] == 'c' && p[4] == 'h') ExitProcess((UINT)watch_main(p + 5));
    }

    /* 1. sequential writes of 1..70,000 bytes through a write-only handle; another handle sees size and bytes as they go */
    DeleteFileA("C:\\fwp-1.tmp");
    writer = CreateFileA("C:\\fwp-1.tmp", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, 0, NULL);
    other = CreateFileA("C:\\fwp-1.tmp", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (writer == INVALID_HANDLE_VALUE || other == INVALID_HANDLE_VALUE) { bad("open", GetLastError(), 0); goto finish; }
    for (i = 0; i < 1500; i++)
    {
        n = 1 + random_next(&state) % 70000;
        if (!write_pattern(writer, size, n, 1)) { bad("write", i, GetLastError()); break; }
        size += n;
        if (i % 50 == 49 || i < 5)
        {
            static BYTE tail[4096];
            DWORD got, t = size < sizeof(tail) ? size : (DWORD)sizeof(tail), k;
            if (GetFileSize(other, NULL) != size) { bad("size seen by the other handle", size, GetFileSize(other, NULL)); break; }
            SetFilePointer(other, (LONG)(size - t), NULL, FILE_BEGIN);
            if (!ReadFile(other, tail, t, &got, NULL) || got != t) { bad("read tail", t, got); break; }
            for (k = 0; k < t; k++) if (tail[k] != pattern(size - t + k, 1)) { bad("tail bytes", size - t + k, 0); break; }
            if (k != t) break;
        }
    }
    CloseHandle(other);
    check_file("C:\\fwp-1.tmp", size, 1, 0, 0, "whole file, writer still open");

    /* 2. overwrite in the middle through the same handle: size and the rest unchanged */
    SetFilePointer(writer, 1000, NULL, FILE_BEGIN);
    if (!write_pattern(writer, 1000, 5000, 2)) bad("overwrite", GetLastError(), 0);
    {
        DWORD saved = size;
        /* bytes 1000..5999 follow seed 2 now, so check the rest and then those apart */
        check_file("C:\\fwp-1.tmp", saved, 1, 1000, 6000, "outside the overwrite");
    }

    /* 3. a gap past the end: the new size, zeros in between */
    SetFilePointer(writer, (LONG)(size + 100000), NULL, FILE_BEGIN);
    if (!write_pattern(writer, size + 100000, 10, 1)) bad("gap write", GetLastError(), 0);
    {
        HANDLE g = CreateFileA("C:\\fwp-1.tmp", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        BYTE hole[256], last[10];
        DWORD got;
        if (GetFileSize(g, NULL) != size + 100010) bad("size after a gap", size + 100010, GetFileSize(g, NULL));
        SetFilePointer(g, (LONG)(size + 50000), NULL, FILE_BEGIN);
        ReadFile(g, hole, sizeof(hole), &got, NULL);
        for (i = 0; i < sizeof(hole); i++) if (hole[i]) { bad("gap not zero", i, hole[i]); break; }
        SetFilePointer(g, (LONG)(size + 100000), NULL, FILE_BEGIN);
        ReadFile(g, last, sizeof(last), &got, NULL);
        for (i = 0; i < 10; i++) if (last[i] != pattern(size + 100000 + i, 1)) { bad("after the gap", i, last[i]); break; }
        CloseHandle(g);
    }
    size += 100010;

    /* 4. shrink with SetEndOfFile and write again: no stale bytes beyond, the new bytes where they go */
    SetFilePointer(writer, 3000, NULL, FILE_BEGIN);
    SetEndOfFile(writer);
    SetFilePointer(writer, 0, NULL, FILE_END);
    if (!write_pattern(writer, 3000, 20000, 3)) bad("write after shrink", GetLastError(), 0);
    CloseHandle(writer);
    {
        HANDLE g = CreateFileA("C:\\fwp-1.tmp", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        static BYTE b[23000];
        DWORD got;
        ReadFile(g, b, sizeof(b), &got, NULL);
        if (got != 23000) bad("size after shrink and write", 23000, got);
        for (i = 6000; i < 23000 && i < got; i++)
            if (b[i] != pattern(i, i < 3000 ? 1 : 3)) { bad("after shrink", i, b[i]); break; }
        CloseHandle(g);
    }

    /* 5. a FILE_APPEND_DATA handle appends at the end whatever the pointer says */
    {
        HANDLE a = CreateFileA("C:\\fwp-1.tmp", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        BYTE chunk[100];
        for (i = 0; i < 100; i++) chunk[i] = pattern(23000 + i, 3);
        SetFilePointer(a, 0, NULL, FILE_BEGIN);
        if (a == INVALID_HANDLE_VALUE || !WriteFile(a, chunk, 100, &done, NULL) || done != 100) bad("append", GetLastError(), 0);
        CloseHandle(a);
        {
            HANDLE g = CreateFileA("C:\\fwp-1.tmp", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            if (GetFileSize(g, NULL) != 23100) bad("size after append", 23100, GetFileSize(g, NULL));
            CloseHandle(g);
        }
    }

    /* 6. a file mapping sees what was written, a second process too, with the writing handle still open */
    DeleteFileA("C:\\fwp-2.tmp");
    writer = CreateFileA("C:\\fwp-2.tmp", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, 0, NULL);
    for (i = 0; i < 16; i++) if (!write_pattern(writer, i * 65536, 65536, 7)) bad("write 1 MiB", i, GetLastError());
    {
        HANDLE g = CreateFileA("C:\\fwp-2.tmp", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        HANDLE section = CreateFileMappingA(g, NULL, PAGE_READONLY, 0, 0, NULL);
        BYTE *view = section ? MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0) : NULL;
        DWORD wrong = 0;
        if (!view) bad("map view", GetLastError(), 0);
        else { for (i = 0; i < 1048576; i += 997) if (view[i] != pattern(i, 7)) wrong++; UnmapViewOfFile(view); }
        if (wrong) bad("mapped bytes", wrong, 0);
        if (section) CloseHandle(section);
        CloseHandle(g);
    }
    {
        STARTUPINFOA si = {sizeof(si)};
        PROCESS_INFORMATION pi;
        char line[400], me[260];
        DWORD code = 99;
        GetModuleFileNameA(NULL, me, sizeof(me));
        wsprintfA(line, "\"%s\" child C:\\fwp-2.tmp 1048576 7", me);
        if (!CreateProcessA(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) bad("child start", GetLastError(), 0);
        else
        {
            WaitForSingleObject(pi.hProcess, 60000);
            GetExitCodeProcess(pi.hProcess, &code);
            if (code) bad("child saw different bytes", code, 0);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        }
    }
    CloseHandle(writer);

    /* 7. delete and recreate under the same name: the new file is the new bytes */
    if (!DeleteFileA("C:\\fwp-2.tmp")) bad("delete", GetLastError(), 0);
    if (GetFileAttributesA("C:\\fwp-2.tmp") != INVALID_FILE_ATTRIBUTES) bad("still there", 0, 0);
    writer = CreateFileA("C:\\fwp-2.tmp", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    for (i = 0; i < 4; i++) if (!write_pattern(writer, i * 4096, 4096, 9)) bad("rewrite", i, GetLastError());
    CloseHandle(writer);
    check_file("C:\\fwp-2.tmp", 16384, 9, 0, 0, "recreated file");
    DeleteFileA("C:\\fwp-2.tmp");
    DeleteFileA("C:\\fwp-1.tmp");

    /* 8. overlapped writes at explicit offsets (what an IOCP-style writer does), another handle watching, then a second process
     *    that must see the bytes within 100 ms of the last write with nothing else flushing them */
    DeleteFileA("C:\\fwp-4.tmp");
    writer = CreateFileA("C:\\fwp-4.tmp", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED, NULL);
    other = CreateFileA("C:\\fwp-4.tmp", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (writer == INVALID_HANDLE_VALUE || other == INVALID_HANDLE_VALUE) bad("open overlapped", GetLastError(), 0);
    else
    {
        HANDLE done_event = CreateEventA(NULL, TRUE, FALSE, NULL);
        static BYTE chunk[65536];
        DWORD at = 0;
        for (i = 0; i < 200; i++)
        {
            OVERLAPPED ov = {0};
            DWORD k, got;
            for (k = 0; k < sizeof(chunk); k++) chunk[k] = pattern(at + k, 4);
            ov.Offset = at;
            ov.hEvent = done_event;
            ResetEvent(done_event);
            if (!WriteFile(writer, chunk, sizeof(chunk), NULL, &ov) && GetLastError() != ERROR_IO_PENDING) { bad("overlapped write", i, GetLastError()); break; }
            if (!GetOverlappedResult(writer, &ov, &got, TRUE) || got != sizeof(chunk)) { bad("overlapped result", i, got); break; }
            at += sizeof(chunk);
            if (i % 20 == 19 || i == 0)
            {
                BYTE tail[256];
                DWORD t;
                if (GetFileSize(other, NULL) != at) { bad("size after overlapped writes", at, GetFileSize(other, NULL)); break; }
                SetFilePointer(other, (LONG)(at - sizeof(tail)), NULL, FILE_BEGIN);
                if (!ReadFile(other, tail, sizeof(tail), &got, NULL) || got != sizeof(tail)) { bad("read after overlapped", got, 0); break; }
                for (t = 0; t < sizeof(tail); t++) if (tail[t] != pattern(at - sizeof(tail) + t, 4)) { bad("bytes after overlapped", at - sizeof(tail) + t, 0); break; }
                if (t != sizeof(tail)) break;
            }
        }
        CloseHandle(done_event);
        CloseHandle(other);
        CloseHandle(writer);
        check_file("C:\\fwp-4.tmp", at, 4, 0, 0, "overlapped file");
    }
    DeleteFileA("C:\\fwp-4.tmp");

    {   /* the timer bound: a small write, the writer's handle left open, nothing else done by the writer; a second PROCESS must see it */
        HANDLE go = CreateEventA(NULL, TRUE, FALSE, "fwp-go");
        STARTUPINFOA si = {sizeof(si)};
        PROCESS_INFORMATION pi;
        char line[400], me[260];
        DWORD code = 99;
        DeleteFileA("C:\\fwp-5.tmp");
        writer = CreateFileA("C:\\fwp-5.tmp", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, 0, NULL);
        GetModuleFileNameA(NULL, me, sizeof(me));
        wsprintfA(line, "\"%s\" watch C:\\fwp-5.tmp 300", me);
        if (!CreateProcessA(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) bad("watch start", GetLastError(), 0);
        else
        {
            Sleep(300);                       /* the watcher is waiting for the event */
            write_pattern(writer, 0, 100, 11);
            write_pattern(writer, 100, 100, 11);
            write_pattern(writer, 200, 100, 11);
            SetEvent(go);                     /* an event is not something that flushes: the 15 ms bound must */
            WaitForSingleObject(pi.hProcess, 20000);
            GetExitCodeProcess(pi.hProcess, &code);
            if (code) bad("a second process saw the bytes late or not at all (4 late, 5 never, 6 no event)", code, 0);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        }
        CloseHandle(writer);
        CloseHandle(go);
        DeleteFileA("C:\\fwp-5.tmp");
    }

    /* 9. speed of 64 KiB writes (information): through the file pointer, then overlapped at explicit offsets as the Agent's writer opens (no buffering) */
    QueryPerformanceFrequency(&frequency);
    writer = CreateFileA("C:\\fwp-3.tmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    QueryPerformanceCounter(&begin);
    for (i = 0; i < 1024; i++) write_pattern(writer, i * 65536, 65536, 5);
    QueryPerformanceCounter(&end);
    seconds = (double)(end.QuadPart - begin.QuadPart) / (double)frequency.QuadPart;
    want_bytes = (DWORD)(67108864.0 / seconds / 1e6 * 10.0);
    wsprintfA(report + lstrlenA(report), " write_64k_MBps=%lu.%lu", want_bytes / 10, want_bytes % 10);
    QueryPerformanceCounter(&begin);
    FlushFileBuffers(writer);
    QueryPerformanceCounter(&end);
    seconds = (double)(end.QuadPart - begin.QuadPart) / (double)frequency.QuadPart;
    wsprintfA(report + lstrlenA(report), " flush_ms=%lu", (DWORD)(seconds * 1000.0));
    CloseHandle(writer);
    DeleteFileA("C:\\fwp-3.tmp");
    writer = CreateFileA("C:\\fwp-6.tmp", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
                         FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, NULL);
    {
        HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        static BYTE chunk[65536];
        QueryPerformanceCounter(&begin);
        for (i = 0; i < 1024; i++)
        {
            OVERLAPPED ov = {0};
            DWORD got;
            ov.Offset = i * 65536;
            ov.hEvent = ev;
            ResetEvent(ev);
            if (!WriteFile(writer, chunk, sizeof(chunk), NULL, &ov) && GetLastError() != ERROR_IO_PENDING) break;
            GetOverlappedResult(writer, &ov, &got, TRUE);
        }
        QueryPerformanceCounter(&end);
        seconds = (double)(end.QuadPart - begin.QuadPart) / (double)frequency.QuadPart;
        want_bytes = (DWORD)(67108864.0 / seconds / 1e6 * 10.0);
        wsprintfA(report + lstrlenA(report), " overlapped_nobuf_64k_MBps=%lu.%lu", want_bytes / 10, want_bytes % 10);
        CloseHandle(ev);
    }
    CloseHandle(writer);
    DeleteFileA("C:\\fwp-6.tmp");

finish:
    wsprintfA(out, "PS5_PROTON_FILE_WRITE %s: failures=%d%s\n", failures ? "FAIL" : "PASS", failures, report);
    OutputDebugStringA(out);
    ExitProcess(failures ? 1 : 0);
}
