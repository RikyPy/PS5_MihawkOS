/* SPDX-License-Identifier: GPL-3.0-or-later
 * Two independently launched processes, one 2 MiB object. Qualify the same
 * SOCK_SEQPACKET/SCM_RIGHTS route as the memory helper before integrating
 * physical backing. No named reopen, title, foreign mapping or kernel access.
 * The creator unlinks immediately and drops its fd before the peer maps it.
 * All waits are bounded; process exit releases maps after any failed check.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef PW_PHYSICAL_SHARED_PS5
extern int physhm_open(const char *, int, unsigned, int);
extern int physhm_unlink(const char *);
extern int sceKernelAvailableFlexibleMemorySize(size_t *);
extern int sceKernelGetResidentFmemCount(int, uint64_t *, uint64_t *);
#endif
enum { PAGE = 16384, BYTES = 2 << 20 };
static unsigned checks, failures;
static int check(int okay, const char *name)
{
    ++checks;
    if (!okay) ++failures;
    printf("physical-transfer check=%s result=%s errno=%d\n", name, okay ? "PASS" : "FAIL", okay ? 0 : errno);
    fflush(stdout);
    return okay;
}
static void census(const char *phase)
{
#ifdef PW_PHYSICAL_SHARED_PS5
    size_t available = 0;
    uint64_t resident = 0, mapped = 0;
    int f = sceKernelAvailableFlexibleMemorySize(&available);
    int r = sceKernelGetResidentFmemCount(getpid(), &resident, &mapped);
    printf("physical-transfer phase=%s flexible_status=%#x available=%zu resident_status=%#x pages=%llu\n",
           phase, f, available, r, (unsigned long long)resident);
#else
    printf("physical-transfer phase=%s oracle=posix-shm\n", phase);
#endif
    fflush(stdout);
}
static int message(int peer, uint32_t *word, int *fd, int sending)
{
    struct pollfd ready = {peer, sending ? POLLOUT : POLLIN, 0};
    if (poll(&ready, 1, 10000) != 1) {
        errno = ETIMEDOUT;
        return -1;
    }
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct iovec io = {word, sizeof(*word)};
    struct msghdr msg = {0};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    if (!sending || *fd >= 0) {
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
    }
    if (sending && *fd >= 0) {
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), fd, sizeof(*fd));
    }
    ssize_t n = sending ? sendmsg(peer, &msg, MSG_NOSIGNAL) : recvmsg(peer, &msg, 0);
    if (!sending) {
        *fd = -1;
        for (struct cmsghdr *c = n > 0 ? CMSG_FIRSTHDR(&msg) : NULL; c; c = CMSG_NXTHDR(&msg, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
                c->cmsg_len >= CMSG_LEN(sizeof(int))) {
                int received;
                memcpy(&received, CMSG_DATA(c), sizeof(received));
                if (*fd < 0)
                    *fd = received;
                else
                    close(received);
            }
    }
    if (n != sizeof(*word) || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}
int main(int argc, char **argv)
{
    const char *tag = NULL;
    int reader = 0;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "reader")) reader = 1;
        if (!strncmp(argv[i], "ps5phys-", 8)) tag = argv[i];
    }
    if (!tag || strlen(tag) > 40 || strspn(tag, "abcdefghijklmnopqrstuvwxyz0123456789-") != strlen(tag))
        return 2;
    char name[64], path[108];
    snprintf(name, sizeof(name), "/%s", tag);
#ifdef PW_PHYSICAL_SHARED_PS5
    snprintf(path, sizeof(path), "/system_tmp/%s.sock", tag);
#else
    snprintf(path, sizeof(path), "/tmp/%s.sock", tag);
#endif
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    int listener = -1, peer = -1, fd = -1, incoming = -1, owned = 0, no_fd = -1;
    unsigned char *map = MAP_FAILED, *alias = MAP_FAILED;
    void *fixed = MAP_FAILED;
    uint32_t word = 0;
    census("baseline");
    if (!reader) {
#ifdef PW_PHYSICAL_SHARED_PS5
        fd = physhm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600, 0x0c);
#else
        fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
#endif
        if (!check(fd >= 0, "create")) goto done;
        owned = 1;
#ifdef PW_PHYSICAL_SHARED_PS5
        int unlinked = !physhm_unlink(name);
#else
        int unlinked = !shm_unlink(name);
#endif
        if (!check(unlinked, "unlink-immediate")) goto done;
        owned = 0;
        if (!check(!ftruncate(fd, BYTES), "size")) goto done;
        map = mmap(NULL, BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (!check(map != MAP_FAILED, "creator-map")) goto done;
        for (unsigned i = 0; i < BYTES; i += PAGE)
            memcpy(map + i, &(uint32_t){0x12500000u + i / PAGE}, 4);
        listener = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (!check(listener >= 0 && !bind(listener, (void *)&address, sizeof(address)) &&
                       !listen(listener, 1),
                   "listen"))
            goto done;
        struct pollfd ready = {listener, POLLIN, 0};
        if (!check(poll(&ready, 1, 10000) == 1, "peer-ready")) goto done;
        peer = accept(listener, NULL, NULL);
        if (!check(peer >= 0, "accept")) goto done;
        word = 0x51;
        if (!check(!message(peer, &word, &fd, 1), "send-fd")) goto done;
        if (!check(!close(fd), "creator-close-fd")) goto done;
        fd = -1;
        word = 0x52;
        if (!check(!message(peer, &word, &no_fd, 1), "send-closed")) goto done;
        if (!check(!message(peer, &word, &incoming, 0) && word == 0x53 && incoming >= 0,
                   "receive-returned-fd"))
            goto done;
        int coherent = 1;
        for (unsigned i = 0; i < BYTES; i += PAGE) {
            uint32_t got;
            memcpy(&got, map + i, 4);
            if (got != 0x62500000u + i / PAGE) coherent = 0;
        }
        if (!check(coherent, "creator-sees-peer-writes")) goto done;
        alias = mmap(NULL, PAGE, PROT_READ, MAP_SHARED, incoming, PAGE);
        if (!check(alias != MAP_FAILED, "returned-fd-offset-map")) goto done;
        memcpy(&word, alias, 4);
        check(word == 0x62500001, "returned-fd-data");
        census("creator-held");
        if (!check(!munmap(alias, PAGE), "creator-unmap-offset")) goto done;
        alias = MAP_FAILED;
        if (!check(!close(incoming), "creator-close-returned-fd")) goto done;
        incoming = -1;
        if (!check(!munmap(map, BYTES), "creator-unmap-all")) goto done;
        map = MAP_FAILED;
        word = 0x54;
        if (!check(!message(peer, &word, &no_fd, 1), "release-peer")) goto done;
        check(!message(peer, &word, &incoming, 0) && incoming < 0 && word == 0x55, "peer-released");
    } else {
        peer = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (!check(peer >= 0, "reader-socket")) goto done;
        int connected = 0;
        for (unsigned i = 0; i < 100; ++i) {
            if (!connect(peer, (void *)&address, sizeof(address))) {
                connected = 1;
                break;
            }
            usleep(100000);
        }
        if (!check(connected, "reader-connect")) goto done;
        if (!check(!message(peer, &word, &fd, 0) && word == 0x51 && fd >= 0, "receive-fd")) goto done;
        if (!check(!message(peer, &word, &incoming, 0) && word == 0x52 && incoming < 0, "creator-fd-closed"))
            goto done;
        struct stat st;
        if (!check(!fstat(fd, &st) && st.st_size == BYTES, "received-size")) goto done;
        map = mmap(NULL, BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (!check(map != MAP_FAILED, "received-map")) goto done;
        int coherent = 1;
        for (unsigned i = 0; i < BYTES; i += PAGE) {
            uint32_t got;
            memcpy(&got, map + i, 4);
            if (got != 0x12500000u + i / PAGE) coherent = 0;
            memcpy(map + i, &(uint32_t){0x62500000u + i / PAGE}, 4);
        }
        if (!check(coherent, "received-data")) goto done;
        /* Replace only a reservation created by this probe, never an arbitrary address. */
        fixed = mmap(NULL, PAGE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (!check(fixed != MAP_FAILED, "own-fixed-reservation")) goto done;
        void *result = mmap(fixed, PAGE, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, fd, PAGE);
        if (!check(result == fixed, "fixed-offset-map")) {
            if (result != MAP_FAILED && result != fixed) munmap(result, PAGE);
            goto done;
        }
        memcpy(&word, fixed, 4);
        if (!check(word == 0x62500001, "fixed-offset-data")) goto done;
        if (!check(!msync(fixed, PAGE, MS_SYNC), "sync-mapped-memory")) goto done;
        if (!check(!munmap(fixed, PAGE), "unmap-fixed")) goto done;
        fixed = MAP_FAILED;
        word = 0x53;
        if (!check(!message(peer, &word, &fd, 1), "return-fd")) goto done;
        if (!check(!close(fd), "reader-close-last-fd")) goto done;
        fd = -1;
        census("reader-held");
        if (!check(!message(peer, &word, &incoming, 0) && word == 0x54 && incoming < 0,
                   "creator-all-released"))
            goto done;
        memcpy(&word, map + PAGE, 4);
        if (!check(word == 0x62500001, "map-survives-last-fd")) goto done;
        if (!check(!munmap(map, BYTES), "reader-unmap-last")) goto done;
        map = MAP_FAILED;
        word = 0x55;
        check(!message(peer, &word, &no_fd, 1), "send-released");
    }
done:
    if (fixed != MAP_FAILED) munmap(fixed, PAGE);
    if (map != MAP_FAILED) munmap(map, BYTES);
    if (alias != MAP_FAILED) munmap(alias, PAGE);
    if (fd >= 0) close(fd);
    if (incoming >= 0) close(incoming);
    if (peer >= 0) close(peer);
    if (listener >= 0) {
        close(listener);
        unlink(path);
    }
    if (owned) {
#ifdef PW_PHYSICAL_SHARED_PS5
        physhm_unlink(name);
#else
        shm_unlink(name);
#endif
    }
    census("released");
    printf("PS5_PHYSICAL_TRANSFER %s role=%s checks=%u failures=%u\n", failures ? "FAIL" : "PASS",
           reader ? "reader" : "creator", checks, failures);
    fflush(stdout);
    return failures ? 1 : 0;
}
