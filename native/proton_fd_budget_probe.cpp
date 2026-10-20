// SPDX-License-Identifier: GPL-3.0-or-later
// What the console's open-file budget counts, and whose it is.
//
// A title can open about 249 files by path (ps5platform/fdv.h). With Windows programs in Sony-managed helper
// processes, a Java launcher and Java ran out within milliseconds of each other (2026-10, records removed from the
// tree; in git history), and the kernel's
// "fdescfree(), but remain BUDGET_FD_FILE" values fell 140 -> 72 -> 52 as each helper ended: the budget looked shared
// by the title and its helpers. Every multi-process Windows program needs that settled before the fix is designed, so
// this probe measures it with nothing else running: no Wine, one helper.
//
// The measure is headroom: open one file by path until the kernel refuses, count the successes, close them all. A
// step changes one thing (who opens, who holds, a descriptor passed over a socket, a dup, a mapping whose descriptor
// is closed, sockets and pipes, a helper that ends holding files) and both processes measure their headroom after
// it. Each process measures alone, the other idle, so the two counts never race.
#include "proton_gpu_broker_probe.h"
#include "../ps5/src/proton_launch.h"
#include "../src/pw_fd_census.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
extern "C" {
int sceSystemServiceGetAppStatus(void *);
int sceSystemServiceAddLocalProcess(int, const char *, const char *const *, const void *);
int sceSystemServiceKillLocalProcess(int, int);
int sceSystemServiceGetLocalProcessStatusList(void *, unsigned, unsigned *);
int sceKernelUsleep(unsigned);
}

namespace {
struct Options { uint32_t size; int32_t fd, crash; uint32_t other[15]; };
static_assert(sizeof(Options) == 72);
constexpr unsigned kStep = 40, kCap = PROTON_FD_BUDGET_CAP;
int helper_socket = -1, helper_pid = -1;

bool ready(int socket, short events)
{
    pollfd p{socket, events, 0};
    int result;
    do { result = poll(&p, 1, 15000); } while (result < 0 && errno == EINTR);
    return result == 1 && (p.revents & events);
}

// One request, its reply, and the descriptor either may carry.
bool ask(uint32_t operation, uint32_t value, ProtonGpuProbeReply &reply, int send_fd = -1, int *received = nullptr)
{
    ProtonGpuProbeMessage message{PROTON_GPU_PROBE_MAGIC, operation, value, 0, 0, 0, 0};
    iovec vector{&message, sizeof(message)};
    union { cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control{};
    msghdr packet{};
    packet.msg_iov = &vector;
    packet.msg_iovlen = 1;
    if (send_fd >= 0) {
        packet.msg_control = control.bytes;
        packet.msg_controllen = sizeof(control.bytes);
        auto *header = CMSG_FIRSTHDR(&packet);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &send_fd, sizeof(send_fd));
    }
    if (!ready(helper_socket, POLLOUT) || sendmsg(helper_socket, &packet, MSG_NOSIGNAL) != sizeof(message)) return false;
    reply = ProtonGpuProbeReply{};
    iovec answer{&reply, sizeof(reply)};
    union { cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } incoming{};
    msghdr in{};
    in.msg_iov = &answer;
    in.msg_iovlen = 1;
    in.msg_control = incoming.bytes;
    in.msg_controllen = sizeof(incoming.bytes);
    if (!ready(helper_socket, POLLIN) || recvmsg(helper_socket, &in, 0) != sizeof(reply)) return false;
    int fd = -1;
    for (cmsghdr *h = CMSG_FIRSTHDR(&in); h; h = CMSG_NXTHDR(&in, h))
        if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS && h->cmsg_len == CMSG_LEN(sizeof(int)))
            memcpy(&fd, CMSG_DATA(h), sizeof(fd));
    if (received) *received = fd;
    else if (fd >= 0) close(fd);
    return reply.magic == PROTON_GPU_PROBE_MAGIC && reply.pid == helper_pid;
}

bool listed(int id)
{
    struct Entry { int32_t id; char name[32]; } entries[16]{};
    unsigned count = 0;
    if (sceSystemServiceGetLocalProcessStatusList(entries, 16, &count) || count > 16) return true;
    for (unsigned i = 0; i < count; ++i)
        if (entries[i].id == id) return true;
    return false;
}

// Opens path until the kernel refuses; the count, and the refusal's errno.
unsigned headroom(const char *path, int &error)
{
    static int held[kCap];
    unsigned n = 0;
    error = 0;
    for (; n < kCap; ++n) {
        const int fd = open(path, O_RDONLY);
        if (fd < 0) { error = errno; break; }
        held[n] = fd;
    }
    for (unsigned i = 0; i < n; ++i) close(held[i]);
    return n;
}
} // namespace

int proton_fd_budget_probe(FILE *out, const char *identity)
{
    const std::string target = proton_title_path("/app0/sce_sys/param.json");
    const std::string image = proton_title_path("/app0/eboot.bin");
    const char *path = target.c_str();
    uint32_t app[4]{};
    bool okay = sceSystemServiceGetAppStatus(app) == 0 && app[0] > 0;
    PwFdCensus census{};
    int error = 0;
    pw_fd_census(&census);
    const unsigned alone = headroom(path, error);
    fprintf(out, "fd-budget alone owner=%u errno=%d owner_files=%u dirs=%u devices=%u sockets=%u open=%u path=%s\n", alone,
            error, census.files, census.dirs, census.devices, census.sockets, census.open, path);
    fsync(fileno(out));

    int service = -1, pair[2]{-1, -1};
    if (okay) okay = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0;
    if (okay) {
        Options options{};
        options.size = sizeof(options);
        options.fd = pair[1];
        options.crash = 1;
        options.other[0] = uint32_t(-1);
        options.other[3] = 2;
        options.other[4] = 0x80000000;
        const char *args[]{"/app0/service-native-helper.bin", "--service-native-budget-helper", identity, nullptr};
        service = sceSystemServiceAddLocalProcess(int(app[0]), args[0], args, &options);
        close(pair[1]);
        helper_socket = pair[0];
        ProtonGpuProbeReply hello{};
        okay = service > 0 && ready(helper_socket, POLLIN) &&
               recv(helper_socket, &hello, sizeof(hello), 0) == sizeof(hello) && hello.magic == PROTON_GPU_PROBE_MAGIC &&
               hello.pid > 0 && hello.pid != getpid() && hello.status == 0;
        helper_pid = hello.pid;
        fprintf(out, "fd-budget helper service=%d pid=%d hello=%d elevation=%d helper_files=%u\n", service, helper_pid,
                okay, hello.elevation, hello.value);
        fsync(fileno(out));
    }

    unsigned steps = 0, measured = 0;
    // Both headrooms after a step: the owner's first, then the helper's.
    auto measure = [&](const char *step) {
        int owner_error = 0;
        pw_fd_census(&census);
        const unsigned owner = headroom(path, owner_error);
        ProtonGpuProbeReply reply{};
        const bool asked = okay && ask(PROTON_FD_BUDGET_HEADROOM, 0, reply);
        fprintf(out, "fd-budget step=%s owner=%u owner_errno=%d owner_files=%u helper=%u helper_errno=%d helper_files=%d "
                     "asked=%d\n",
                step, owner, owner_error, census.files, asked ? reply.value : 0u, asked ? reply.elevation : -1,
                asked ? reply.map_stage : -1, asked);
        fsync(fileno(out));
        ++steps;
        measured += asked && reply.status == 0;
    };
    int held[kStep];
    auto open_all = [&](const char *name) {
        unsigned n = 0;
        for (; n < kStep; ++n)
            if ((held[n] = open(name, O_RDONLY)) < 0) break;
        return n;
    };
    auto close_all = [&] { for (int &fd : held) if (fd >= 0) { close(fd); fd = -1; } };
    for (int &fd : held) fd = -1;

    if (okay) {
        measure("baseline");

        // The owner opens files: whose headroom falls?
        const unsigned opened = open_all(path);
        fprintf(out, "fd-budget owner_opened=%u\n", opened);
        measure("owner_holds_40");

        // The owner passes them to the helper and closes its own copies: does the cost follow the file, the holder or
        // the opener?
        unsigned given = 0;
        for (int &fd : held) {
            ProtonGpuProbeReply reply{};
            if (fd >= 0 && ask(PROTON_FD_BUDGET_RECEIVE, 0, reply, fd) && !reply.status) ++given;
            if (fd >= 0) { close(fd); fd = -1; }
        }
        fprintf(out, "fd-budget owner_gave=%u\n", given);
        measure("helper_holds_owners_40");
        ProtonGpuProbeReply reply{};
        (void)ask(PROTON_FD_BUDGET_CLOSE, 0, reply);
        measure("helper_closed");

        // The helper opens files and passes them to the owner.
        (void)ask(PROTON_FD_BUDGET_OPEN, kStep, reply);
        fprintf(out, "fd-budget helper_opened=%u status=%d\n", reply.value, reply.status);
        measure("helper_holds_40");
        unsigned taken = 0;
        for (unsigned n = 0; n < kStep; ++n) {
            int fd = -1;
            if (ask(PROTON_FD_BUDGET_GIVE, n, reply, -1, &fd) && !reply.status && fd >= 0) held[taken++] = fd;
            else if (fd >= 0) close(fd);
        }
        fprintf(out, "fd-budget owner_took=%u\n", taken);
        measure("owner_holds_helpers_40");
        close_all();
        measure("owner_closed");

        // A dup is another descriptor of the same open file.
        int first = open(path, O_RDONLY);
        held[0] = first;
        for (unsigned n = 1; n < kStep && first >= 0; ++n) held[n] = dup(first);
        measure("owner_dup_40");
        close_all();

        // A file mapped and its descriptor closed, as Wine maps a DLL.
        void *views[kStep]{};
        unsigned mapped = 0;
        for (unsigned n = 0; n < kStep; ++n) {
            const int fd = open(image.c_str(), O_RDONLY);
            if (fd < 0) break;
            void *view = mmap(nullptr, 16384, PROT_READ, MAP_SHARED, fd, 0);
            close(fd);
            if (view == MAP_FAILED) break;
            views[mapped++] = view;
        }
        fprintf(out, "fd-budget owner_mapped=%u\n", mapped);
        measure("owner_maps_40_closed");
        for (unsigned n = 0; n < mapped; ++n) munmap(views[n], 16384);
        measure("owner_unmapped");

        // What is not a file: sockets, pipes, kqueues, anonymous shared memory.
        int others[200];
        unsigned count = 0;
        for (unsigned n = 0; n < 20; ++n) {
            int sockets[2];
            if (!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) { others[count++] = sockets[0]; others[count++] = sockets[1]; }
            int pipes[2];
            if (!pipe(pipes)) { others[count++] = pipes[0]; others[count++] = pipes[1]; }
            const int queue = kqueue();
            if (queue >= 0) others[count++] = queue;
            const int shm = shm_open(SHM_ANON, O_RDWR, 0600);
            if (shm >= 0) others[count++] = shm;
        }
        fprintf(out, "fd-budget owner_other_descriptors=%u\n", count);
        measure("owner_holds_other_120");
        for (unsigned n = 0; n < count; ++n) close(others[n]);

        // Directories.
        count = 0;
        for (unsigned n = 0; n < 10; ++n) {
            const int fd = open(proton_app_root(), O_RDONLY | O_DIRECTORY);
            if (fd >= 0) others[count++] = fd;
        }
        fprintf(out, "fd-budget owner_dirs=%u\n", count);
        measure("owner_holds_10_dirs");
        for (unsigned n = 0; n < count; ++n) close(others[n]);

        // A helper that ends holding files: does the budget get them back?
        (void)ask(PROTON_FD_BUDGET_OPEN, kStep, reply);
        fprintf(out, "fd-budget helper_opened_before_close=%u status=%d\n", reply.value, reply.status);
        measure("helper_holds_40_before_close");
    }

    bool absent = true;
    if (service > 0) {
        const int closed = sceSystemServiceKillLocalProcess(int(app[0]), service);
        for (unsigned n = 0; n < 300 && listed(service); ++n) sceKernelUsleep(10000);
        absent = !listed(service);
        fprintf(out, "fd-budget helper closed=%#x absent=%d\n", unsigned(closed), absent);
        okay &= closed == 0 && absent;
    }
    if (helper_socket >= 0) close(helper_socket);
    // The kernel may return a dead process's files a little after Sony lists it gone.
    unsigned after = 0;
    for (unsigned n = 0; n < 5; ++n) {
        after = headroom(path, error);
        fprintf(out, "fd-budget after_helper_closed attempt=%u owner=%u errno=%d\n", n, after, error);
        if (after >= alone) break;
        sceKernelUsleep(200000);
    }
    pw_fd_census(&census);
    fprintf(out, "fd-budget summary alone=%u after=%u steps=%u measured=%u absent=%d open=%u\n", alone, after, steps, measured,
            absent, census.open);
    fsync(fileno(out));
    return okay && steps == measured && after == alone ? 0 : 1;
}
