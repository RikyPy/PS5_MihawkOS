/* SPDX-License-Identifier: GPL-3.0-or-later
 * How fast data moves through this runtime, layer by layer (a launcher's download of a Unity game ran at 2.5 MB/s on a
 * 1 Gbps line, 2026-10-11). It takes `host port` on its command line and a server that sends as fast as it can
 * (tools/run-net-throughput.py), and measures, each for a few seconds:
 *   - plain blocking recv with 4 KiB, 64 KiB and 1 MiB buffers (Wine's winsock over the console's sockets);
 *   - the non-blocking recv + select loop libcurl uses, with a 16 KiB buffer;
 *   - four connections at once;
 *   - writing 128 MiB to a file on C: in 64 KiB writes and flushing it, then reading it back (the prefix's disk).
 * It prints MB/s (decimal megabytes), a measurement and not a verdict: PASS means every step ran. */
#include <winsock2.h>
#include <windows.h>

int _fltused; /* floating point without the C runtime */
static char out[900];
static LARGE_INTEGER frequency;

static double seconds_since(LARGE_INTEGER begin)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - begin.QuadPart) / (double)frequency.QuadPart;
}

static void add(const char *label, double megabytes_per_second)
{
    wsprintfA(out + lstrlenA(out), " %s=%lu.%lu", label, (unsigned long)megabytes_per_second,
              (unsigned long)(megabytes_per_second * 10.0) % 10);
}

static SOCKET connect_to(const char *host, unsigned port)
{
    struct sockaddr_in address;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ZeroMemory(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((u_short)port);
    address.sin_addr.s_addr = inet_addr(host);
    if (s == INVALID_SOCKET || connect(s, (struct sockaddr *)&address, sizeof(address))) return INVALID_SOCKET;
    return s;
}

/* Blocking recv for `limit` seconds. */
static double receive_blocking(const char *host, unsigned port, int buffer_bytes, double limit)
{
    static char buffer[1 << 20];
    SOCKET s = connect_to(host, port);
    LARGE_INTEGER begin;
    double total = 0;
    if (s == INVALID_SOCKET) return -1;
    QueryPerformanceCounter(&begin);
    while (seconds_since(begin) < limit)
    {
        int got = recv(s, buffer, buffer_bytes, 0);
        if (got <= 0) break;
        total += got;
    }
    {
        double elapsed = seconds_since(begin);
        closesocket(s);
        return elapsed > 0 ? total / elapsed / 1e6 : 0;
    }
}

/* What libcurl does: a non-blocking socket, select, then recv into 16 KiB. */
static double receive_select(const char *host, unsigned port, int buffer_bytes, double limit)
{
    static char buffer[1 << 16];
    SOCKET s = connect_to(host, port);
    LARGE_INTEGER begin;
    u_long nonblocking = 1;
    double total = 0;
    if (s == INVALID_SOCKET) return -1;
    ioctlsocket(s, FIONBIO, &nonblocking);
    QueryPerformanceCounter(&begin);
    while (seconds_since(begin) < limit)
    {
        fd_set readable;
        struct timeval wait = {1, 0};
        int got;
        FD_ZERO(&readable);
        FD_SET(s, &readable);
        if (select(0, &readable, NULL, NULL, &wait) <= 0) continue;
        got = recv(s, buffer, buffer_bytes, 0);
        if (got == 0) break;
        if (got < 0) { if (WSAGetLastError() == WSAEWOULDBLOCK) continue; break; }
        total += got;
    }
    {
        double elapsed = seconds_since(begin);
        closesocket(s);
        return elapsed > 0 ? total / elapsed / 1e6 : 0;
    }
}

struct worker { const char *host; unsigned port; double seconds, megabytes_per_second; };

static DWORD WINAPI worker_main(void *argument)
{
    struct worker *w = argument;
    w->megabytes_per_second = receive_blocking(w->host, w->port, 65536, w->seconds);
    return 0;
}

static double disk(BOOL write, double *seconds_out)
{
    static char block[65536];
    HANDLE file = CreateFileA("C:\\net-throughput-probe.tmp", write ? GENERIC_WRITE : GENERIC_READ, 0, NULL,
                              write ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER begin;
    unsigned i;
    DWORD done;
    double elapsed;
    if (file == INVALID_HANDLE_VALUE) return -1;
    QueryPerformanceCounter(&begin);
    for (i = 0; i < 2048; i++) /* 2048 * 64 KiB = 128 MiB */
    {
        if (write) { block[0] = (char)i; if (!WriteFile(file, block, sizeof(block), &done, NULL) || done != sizeof(block)) break; }
        else if (!ReadFile(file, block, sizeof(block), &done, NULL) || done != sizeof(block)) break;
    }
    if (write) FlushFileBuffers(file);
    elapsed = seconds_since(begin);
    CloseHandle(file);
    *seconds_out = elapsed;
    return i == 2048 && elapsed > 0 ? 134217728.0 / elapsed / 1e6 : -1;
}

void mainCRTStartup(void)
{
    char *command = GetCommandLineA(), host[64] = "";
    unsigned port = 0, k = 0;
    WSADATA data;
    int failures = 0;
    double rate, spent;

    /* the arguments after the program name: host and port */
    if (*command == '"') { command++; while (*command && *command != '"') command++; if (*command) command++; }
    else while (*command && *command != ' ') command++;
    while (*command == ' ') command++;
    while (*command && *command != ' ' && k < sizeof(host) - 1) host[k++] = *command++;
    while (*command == ' ') command++;
    while (*command >= '0' && *command <= '9') port = port * 10 + (unsigned)(*command++ - '0');
    QueryPerformanceFrequency(&frequency);
    WSAStartup(MAKEWORD(2, 2), &data);
    lstrcpyA(out, "PS5_PROTON_NET_THROUGHPUT ");
    if (!host[0] || !port) { lstrcatA(out, "FAIL: usage: host port\n"); OutputDebugStringA(out); ExitProcess(2); }
    lstrcatA(out, "MB/s:");

    rate = receive_blocking(host, port, 4096, 4.0);   if (rate < 0) failures++; add("recv4k", rate);
    rate = receive_blocking(host, port, 65536, 4.0);  if (rate < 0) failures++; add("recv64k", rate);
    rate = receive_blocking(host, port, 1 << 20, 4.0); if (rate < 0) failures++; add("recv1m", rate);
    rate = receive_select(host, port, 16384, 4.0);    if (rate < 0) failures++; add("select16k", rate);
    {
        struct worker w[4];
        HANDLE threads[4];
        double sum = 0;
        int i;
        for (i = 0; i < 4; i++)
        {
            w[i].host = host; w[i].port = port; w[i].seconds = 4.0; w[i].megabytes_per_second = 0;
            threads[i] = CreateThread(NULL, 0, worker_main, &w[i], 0, NULL);
        }
        WaitForMultipleObjects(4, threads, TRUE, 20000);
        for (i = 0; i < 4; i++) { sum += w[i].megabytes_per_second > 0 ? w[i].megabytes_per_second : 0; CloseHandle(threads[i]); }
        add("four_connections", sum);
    }
    rate = disk(TRUE, &spent);  if (rate < 0) failures++; add("disk_write", rate);
    rate = disk(FALSE, &spent); if (rate < 0) failures++; add("disk_read", rate);
    DeleteFileA("C:\\net-throughput-probe.tmp");
    wsprintfA(out + lstrlenA(out), " failures=%d\n", failures);
    {
        char line[1000];
        wsprintfA(line, "%s", out);
        /* the marker line carries the verdict first */
        lstrcpyA(line, "PS5_PROTON_NET_THROUGHPUT ");
        lstrcatA(line, failures ? "FAIL " : "PASS ");
        lstrcatA(line, out + lstrlenA("PS5_PROTON_NET_THROUGHPUT "));
        OutputDebugStringA(line);
    }
    ExitProcess(failures ? 1 : 0);
}
