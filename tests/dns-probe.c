/* SPDX-License-Identifier: GPL-3.0-or-later
 * Names and the network through Winsock, as a Windows program uses them: getaddrinfo for each name on the command line
 * (example.com when there is none), then a TCP connection to port 80 of the first name's first address and an HTTP
 * HEAD request, whose status line it reads. It passes when every name resolved and the server answered "HTTP/".
 * The computer's own name and "localhost" must resolve too, to loopback, without the network. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

static void say(const char *text)
{
    OutputDebugStringA(text);
}

/* shown: the name as the log gives it ("the computer's name" for that one, which stays private). */
static int resolve_shown(const char *name, const char *shown, struct in_addr *first);
static int resolve(const char *name, struct in_addr *first)
{
    return resolve_shown(name, name, first);
}
static int resolve_shown(const char *name, const char *shown, struct in_addr *first)
{
    struct addrinfo hints = {0}, *result = NULL, *entry;
    char line[300];
    int count = 0, error;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    error = getaddrinfo(name, "80", &hints, &result);
    for (entry = result; entry; entry = entry->ai_next) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)entry->ai_addr;
        const unsigned char *b = (const unsigned char *)&in->sin_addr;
        if (!count && first) *first = in->sin_addr;
        /* Only the first address is printed: the rest are the same server's. */
        if (!count) {
            wsprintfA(line, "dns: %s -> %u.%u.%u.%u\n", shown, b[0], b[1], b[2], b[3]);
            say(line);
        }
        ++count;
    }
    if (result) freeaddrinfo(result);
    wsprintfA(line, "dns: %s error=%d addresses=%d\n", shown, error, count);
    say(line);
    return !error && count;
}

void mainCRTStartup(void)
{
    WSADATA data;
    char *command = GetCommandLineA(), host[256] = "example.com", computer[64] = "";
    struct in_addr address = {0};
    int passed = 1, names = 0;

    if (WSAStartup(MAKEWORD(2, 2), &data)) {
        say("PS5_PROTON_DNS_PROBE FAIL: WSAStartup\n");
        ExitProcess(1);
    }
    /* The words after the program name are names to resolve. */
    if (*command == '"') { ++command; while (*command && *command != '"') ++command; if (*command) ++command; }
    else while (*command && *command != ' ') ++command;
    for (;;) {
        char name[256];
        unsigned length = 0;
        while (*command == ' ') ++command;
        if (!*command) break;
        while (*command && *command != ' ' && length < sizeof(name) - 1) name[length++] = *command++;
        name[length] = 0;
        if (!names) lstrcpyA(host, name);
        passed &= resolve(name, names ? NULL : &address);
        ++names;
    }
    if (!names) { passed &= resolve(host, &address); names = 1; }
    passed &= resolve("localhost", NULL);
    if (!gethostname(computer, sizeof(computer))) passed &= resolve_shown(computer, "the computer's name", NULL);

    if (passed) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in to = {0};
        char request[400], reply[64] = {0};
        DWORD timeout = 10000;
        int got = 0;
        to.sin_family = AF_INET;
        to.sin_port = htons(80);
        to.sin_addr = address;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
        if (s != INVALID_SOCKET && !connect(s, (struct sockaddr *)&to, sizeof(to))) {
            wsprintfA(request, "HEAD / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
            send(s, request, lstrlenA(request), 0);
            got = recv(s, reply, sizeof(reply) - 1, 0);
        }
        if (got > 0) {
            char line[120];
            int i;
            for (i = 0; i < got && reply[i] != '\r' && reply[i] != '\n'; ++i) {}
            reply[i] = 0;
            wsprintfA(line, "dns: %s port 80 answered \"%s\"\n", host, reply);
            say(line);
        } else {
            char line[120];
            wsprintfA(line, "dns: %s port 80: no answer (error %d)\n", host, WSAGetLastError());
            say(line);
        }
        passed = got > 5 && reply[0] == 'H' && reply[1] == 'T' && reply[2] == 'T' && reply[3] == 'P' && reply[4] == '/';
        if (s != INVALID_SOCKET) closesocket(s);
    }
    WSACleanup();
    say(passed ? "PS5_PROTON_DNS_PROBE PASS: names resolved and a server answered\n"
               : "PS5_PROTON_DNS_PROBE FAIL\n");
    ExitProcess(passed ? 0 : 1);
}
