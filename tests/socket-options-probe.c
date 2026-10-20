/* SPDX-License-Identifier: GPL-3.0-or-later
 * Windows send-buffer sizes are advisory, including zero. A launcher exposed a
 * PS5 rejection of SO_SNDBUF=0; Linux Wine accepts it and reads back zero.
 * Check both address families, set/get transitions, malformed sizes, and
 * actual bidirectional TCP traffic with zero send buffers. No external network.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

static unsigned checks, failures;
static void check(int passed, const char *stage, int family)
{
    char line[180];
    ++checks;
    failures += !passed;
    wsprintfA(line, "socket-options: %s family=%d %s error=%d\n",
              stage, family, passed ? "PASS" : "FAIL", passed ? 0 : WSAGetLastError());
    OutputDebugStringA(line);
}

static void options(SOCKET s, int family)
{
    const int sizes[] = {0, 65536, 0};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        int value = -1, bytes = sizeof(value);
        check(!setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&sizes[i], sizeof(sizes[i])),
              "set-send-buffer", family);
        check(!getsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *)&value, &bytes) &&
              bytes == sizeof(value) && value == sizes[i], "get-send-buffer", family);
    }
    int value = 0;
    check(setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&value, 1) == SOCKET_ERROR,
          "reject-short-option", family);
}

static int transfer(SOCKET source, SOCKET target)
{
    char data[64], result[64];
    int sent = 0, received = 0;
    for (unsigned i = 0; i < sizeof(data); ++i) data[i] = (char)(i * 3 + 1);
    while (sent < sizeof(data)) {
        int n = send(source, data + sent, sizeof(data) - sent, 0);
        if (n <= 0) return 0;
        sent += n;
    }
    while (received < sizeof(result)) {
        int n = recv(target, result + received, sizeof(result) - received, 0);
        if (n <= 0) return 0;
        received += n;
    }
    for (unsigned i = 0; i < sizeof(data); ++i) if (data[i] != result[i]) return 0;
    return 1;
}

static void family_probe(int family)
{
    SOCKET listener = socket(family, SOCK_STREAM, IPPROTO_TCP), client = INVALID_SOCKET, peer = INVALID_SOCKET;
    struct sockaddr_storage address = {0};
    int bytes, zero = 0;
    DWORD timeout = 5000;
    check(listener != INVALID_SOCKET, "socket", family);
    if (listener == INVALID_SOCKET) return;
    options(listener, family);
    if (family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&address;
        a->sin_family = AF_INET; a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bytes = sizeof(*a);
    } else {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&address;
        a->sin6_family = AF_INET6; a->sin6_addr.u.Byte[15] = 1;
        bytes = sizeof(*a);
    }
    int bound = !bind(listener, (struct sockaddr *)&address, bytes) &&
                !getsockname(listener, (struct sockaddr *)&address, &bytes) && !listen(listener, 1);
    check(bound, "listen-loopback", family);
    if (!bound) goto done;
    client = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) { check(0, "client-socket", family); goto done; }
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    check(!setsockopt(client, SOL_SOCKET, SO_SNDBUF, (const char *)&zero, sizeof(zero)), "client-zero-buffer", family);
    int connected = !connect(client, (struct sockaddr *)&address, bytes);
    check(connected, "connect-loopback", family);
    if (!connected) goto done;
    peer = accept(listener, NULL, NULL);
    check(peer != INVALID_SOCKET, "accept-loopback", family);
    if (peer == INVALID_SOCKET) goto done;
    setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    check(!setsockopt(peer, SOL_SOCKET, SO_SNDBUF, (const char *)&zero, sizeof(zero)), "peer-zero-buffer", family);
    check(transfer(client, peer), "client-to-peer", family);
    check(transfer(peer, client), "peer-to-client", family);
done:
    if (peer != INVALID_SOCKET) closesocket(peer);
    if (client != INVALID_SOCKET) closesocket(client);
    closesocket(listener);
}

void mainCRTStartup(void)
{
    WSADATA data;
    char line[120];
    if (WSAStartup(MAKEWORD(2, 2), &data)) {
        OutputDebugStringA("PS5_PROTON_SOCKET_OPTIONS FAIL: startup\n");
        ExitProcess(1);
    }
    family_probe(AF_INET);
    family_probe(AF_INET6);
    WSACleanup();
    wsprintfA(line, "PS5_PROTON_SOCKET_OPTIONS %s checks=%u failures=%u\n",
              failures ? "FAIL" : "PASS", checks, failures);
    OutputDebugStringA(line);
    ExitProcess(failures ? 1 : 0);
}
