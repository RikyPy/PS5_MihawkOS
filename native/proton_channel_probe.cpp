// SPDX-License-Identifier: GPL-3.0-or-later
// The title's half of the channel test (docs/WINDOWS_PROCESSES.md, "Helpers outside the count"): could the title and a
// process the jailbreak's payload loader starts hand each other descriptors, as a helper needs the wineserver's and
// the GPU broker's sockets? tools/helperhostprobe.c's channel step listens on a Unix-domain socket under /data; this
// side connects, sends one end of a socket pair and an anonymous shared-memory object over SCM_RIGHTS, and checks
// the payload answers through the first and read the pattern written into the second; then it takes a socket end
// the payload sends back and answers through that. tools/run-channel-test.py runs both halves.
#include "proton_gpu_broker_probe.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <arpa/inet.h>
#include <ctime>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern "C" int sceKernelUsleep(unsigned);
extern "C" int sceNetInit(void);

namespace {

// /data cannot hold a socket; the payload binds one where it can and writes its path into this file.
const char channel_note[] = "/data/ps5proton/run/helper-channel.path";
const char shm_pattern[] = "ps5-proton channel shared memory";

unsigned long long monotonic_ms()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000 + (unsigned long long)now.tv_nsec / 1000000;
}

bool wait_readable(int fd, int milliseconds)
{
    pollfd p{fd, POLLIN, 0};
    int ret;
    do ret = poll(&p, 1, milliseconds); while (ret < 0 && errno == EINTR);
    return ret == 1 && (p.revents & POLLIN);
}

int send_fds(int socket, const char *text, const int *fds, unsigned count)
{
    char control[CMSG_SPACE(sizeof(int) * 4)]{};
    iovec io{const_cast<char *>(text), strlen(text) + 1};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = CMSG_SPACE(sizeof(int) * count);
    cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int) * count);
    memcpy(CMSG_DATA(header), fds, sizeof(int) * count);
    return sendmsg(socket, &message, 0) < 0 ? errno : 0;
}

/* Receives one message; returns how many descriptors came with it (into fds), -1 on error. */
int receive_fds(int socket, char *text, size_t size, int *fds, unsigned capacity)
{
    char control[CMSG_SPACE(sizeof(int) * 4)]{};
    iovec io{text, size - 1};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    const ssize_t got = recvmsg(socket, &message, 0);
    if (got < 0) return -1;
    text[got] = 0;
    int count = 0;
    for (cmsghdr *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header))
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS)
            for (size_t i = 0; i < (header->cmsg_len - CMSG_LEN(0)) / sizeof(int) && count < (int)capacity; ++i)
                memcpy(&fds[count++], CMSG_DATA(header) + i * sizeof(int), sizeof(int));
    return count;
}

} // namespace

namespace {

/* The exchange with a payload that listens (tools/helperhostprobe.c, channel step): 0 when every part of it held. */
int channel_exchange(FILE *out)
{
    // The payload listens; it was sent first and may still be starting.
    int connection = -1, error = 0;
    unsigned attempts = 0;
    char channel_path[104]{};
    for (; attempts < 60; ++attempts) {
        if (!channel_path[0]) {
            if (FILE *note = fopen(channel_note, "r")) {
                if (!fgets(channel_path, sizeof(channel_path), note)) channel_path[0] = 0;
                fclose(note);
            }
            if (!channel_path[0]) { error = ENOENT; sceKernelUsleep(500000); continue; }
        }
        connection = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        strncpy(address.sun_path, channel_path, sizeof(address.sun_path) - 1);
        if (connection >= 0 && !connect(connection, reinterpret_cast<sockaddr *>(&address), sizeof(address))) break;
        error = errno;
        if (connection >= 0) close(connection);
        connection = -1;
        sceKernelUsleep(500000);
    }
    fprintf(out, "channel connect path=%s fd=%d errno=%d attempts=%u\n", channel_path[0] ? channel_path : "(none)",
            connection, connection < 0 ? error : 0, attempts + 1);
    if (connection < 0) return 1;

    // Over the channel: one end of a socket pair, as the wineserver's socket is, and a shared-memory object, as the
    // frame pages are.
    int pair[2] = {-1, -1};
    const int pair_status = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair);
    const int memory = shm_open(SHM_ANON, O_RDWR, 0600);
    void *page = MAP_FAILED;
    if (memory >= 0 && !ftruncate(memory, 65536)) page = mmap(nullptr, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, memory, 0);
    if (page != MAP_FAILED) memcpy(page, shm_pattern, sizeof(shm_pattern));
    fprintf(out, "channel made pair=%d memory=%d mapped=%d\n", pair_status, memory, page != MAP_FAILED);
    if (pair_status || memory < 0 || page == MAP_FAILED) return 1;
    const int sent[2] = {pair[1], memory};
    const int send_error = send_fds(connection, "title: a socket end and a shared page", sent, 2);
    fprintf(out, "channel sent descriptors=2 errno=%d\n", send_error);
    close(pair[1]);
    close(memory);
    if (send_error) return 1;

    // The payload answers through the socket end it was given.
    char text[160]{};
    const bool answered = wait_readable(pair[0], 20000) && recv(pair[0], text, sizeof(text) - 1, 0) > 0;
    fprintf(out, "channel answer through the sent socket: %s \"%s\"\n", answered ? "yes" : "no", answered ? text : "");

    // And it sends a socket end back, through which this side answers.
    int back[4] = {-1, -1, -1, -1};
    char note[160]{};
    const int received = wait_readable(connection, 20000) ? receive_fds(connection, note, sizeof(note), back, 4) : -1;
    fprintf(out, "channel received from the payload descriptors=%d note=\"%s\"\n", received, received >= 0 ? note : "");
    bool replied = false;
    if (received == 1) {
        static const char reply[] = "title: through the socket you sent";
        replied = send(back[0], reply, sizeof(reply), 0) == (ssize_t)sizeof(reply);
        close(back[0]);
    }
    fprintf(out, "channel answer through the received socket: %s\n", replied ? "sent" : "no");

    // The payload's verdict on the shared page and the reply.
    char verdict[200]{};
    const bool judged = wait_readable(connection, 20000) && recv(connection, verdict, sizeof(verdict) - 1, 0) > 0;
    fprintf(out, "channel payload verdict: \"%s\"\n", judged ? verdict : "");
    munmap(page, 65536);
    close(pair[0]);
    close(connection);
    const bool passed = answered && received == 1 && replied && judged && strstr(verdict, "page=1 reply=1");
    fprintf(out, "channel summary connect=1 answer=%d received=%d reply=%d verdict=%d\n", answered, received, replied,
            judged);
    return passed ? 0 : 1;
}

} // namespace

int proton_channel_probe(FILE *out, const char *identity)
{
    fprintf(out, "service-native identity=%s revision=1 parent=%d mode=channel\n", identity, int(getpid()));
    return channel_exchange(out);
}

/* The title starts the payload itself (docs/WINDOWS_PROCESSES.md, "Helpers outside the count"): it posts
 * tools/helperhostprobe.c's payload, which tools/run-payload-start-test.py put under /data, to the console's own
 * payload launcher (websrv's POST /elfldr on 127.0.0.1:8080, the channel step as its argument), then does the channel
 * exchange with the process that starts. How long each part takes is what a helper started this way would wait. */
int proton_payload_start_probe(FILE *out, const char *identity)
{
    fprintf(out, "service-native identity=%s revision=1 parent=%d mode=payload-start\n", identity, int(getpid()));
    static const char image_path[] = "/data/ps5proton/run/helperhostprobe.elf";
    FILE *image = fopen(image_path, "rb");
    std::string elf;
    if (image) {
        char buffer[65536];
        size_t got;
        while ((got = fread(buffer, 1, sizeof(buffer), image)) > 0) elf.append(buffer, got);
        fclose(image);
    }
    fprintf(out, "payload-start image bytes=%zu\n", elf.size());
    if (elf.size() < 64) return 1;
    unlink(channel_note);   // a previous run's path must not be taken for this one's

    const unsigned long long start = monotonic_ms();
    const int net = sceNetInit();
    const int web = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(8080);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const int connected = web >= 0 ? connect(web, reinterpret_cast<sockaddr *>(&address), sizeof(address)) : -1;
    fprintf(out, "payload-start net_init=%#x socket=%d connect 127.0.0.1:8080 -> %d errno=%d\n", (unsigned)net, web,
            connected, connected ? errno : 0);
    if (connected) { if (web >= 0) close(web); return 1; }
    static const char boundary[] = "ProtonTitlePayloadStart";
    std::string body;
    const auto field = [&](const char *name, const std::string &value, bool file) {
        body += std::string("--") + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"";
        if (file) body += "; filename=\"helperhostprobe.elf\"";
        body += "\r\n\r\n" + value + "\r\n";
    };
    field("args", "channel", false);
    field("pipe", "0", false);
    field("elf", elf, true);
    body += std::string("--") + boundary + "--\r\n";
    char header[256];
    snprintf(header, sizeof(header), "POST /elfldr HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nContent-Type: multipart/form-data; "
             "boundary=%s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", boundary, body.size());
    std::string request = std::string(header) + body;
    size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t wrote = send(web, request.data() + sent, request.size() - sent, 0);
        if (wrote <= 0) break;
        sent += (size_t)wrote;
    }
    char reply[256]{};
    const ssize_t got = sent == request.size() && wait_readable(web, 15000) ? recv(web, reply, sizeof(reply) - 1, 0) : -1;
    close(web);
    if (char *end = strpbrk(reply, "\r\n")) *end = 0;
    const unsigned long long answered = monotonic_ms();
    fprintf(out, "payload-start sent %zu of %zu bytes; launcher answered \"%s\" after %llu ms\n", sent, request.size(),
            got > 0 ? reply : "", answered - start);
    if (got <= 0) return 1;
    const int exchange = channel_exchange(out);
    fprintf(out, "payload-start the channel held %d, %llu ms after the request\n", exchange == 0,
            monotonic_ms() - start);
    return exchange;
}
