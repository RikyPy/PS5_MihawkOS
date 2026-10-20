// SPDX-License-Identifier: GPL-3.0-or-later
#include "proton_child_probe.h"
#include "../ps5/src/proton_launch.h"
#include "../src/pw_fd_census.h"
#include <ps5platform/kernel.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" int sceKernelDlsym(int module, const char *name, void *address);

namespace {
constexpr size_t StackSize = 64 * 1024;
constexpr unsigned ParentValue = 0x1357, ChildValue = 0x2468;
constexpr int ChildStatus = 37;
struct Context { int parent, child; volatile unsigned *memory; };
struct Reply { pid_t pid; uid_t uid; unsigned value; };

bool ready(int fd, short events)
{
    pollfd descriptor{fd, events, 0};
    int result;
    do { result = poll(&descriptor, 1, 3000); } while (result < 0 && errno == EINTR);
    return result == 1 && (descriptor.revents & events);
}

int child(void *argument)
{
    // No allocation, logging, Wine or process-global initialization in the clone.
    auto *context = static_cast<Context *>(argument);
    close(context->parent);
    *context->memory = ChildValue;
    const Reply reply{getpid(), geteuid(), *context->memory};
    if (!ready(context->child, POLLOUT) ||
        write(context->child, &reply, sizeof(reply)) != sizeof(reply)) return 91;
    char release = 0;
    if (!ready(context->child, POLLIN) || read(context->child, &release, 1) != 1 || release != 'R') return 92;
    close(context->child);
    // rfork_thread's libkernel wrapper terminates this native child on return.
    return ChildStatus;
}
}

bool proton_child_probe(bool elevated, int &status)
{
    const auto request = proton_title_path("/app0/child-probe.txt");
    FILE *input = fopen(request.c_str(), "r");
    if (!input) return false;
    char identity[65]{};
    const bool valid = fscanf(input, "%64s", identity) == 1 && strlen(identity) == 32 &&
        strspn(identity, "0123456789abcdef") == 32;
    fclose(input);
    unlink(request.c_str());
    status = 1;
    FILE *output = fopen(proton_title_path("/app0/child-probe-results.txt").c_str(), "w");
    if (!output) return true;
    setvbuf(output, nullptr, _IONBF, 0);
    fprintf(output, "child-probe identity=%s revision=1 parent=%d elevated=%d uid=%u euid=%u\n",
        valid ? identity : "invalid", int(getpid()), elevated, unsigned(getuid()), unsigned(geteuid()));
    ps5_kernel_sw_version firmware{};
    firmware.size = sizeof(firmware);
    const int version = sceKernelGetSystemSwVersion(&firmware);
    fprintf(output, "firmware status=%d version=%08x\n", version, firmware.version);
    using Spawn = pid_t (*)(int, void *, int (*)(void *), void *);
    // Check the actual relocated import through a volatile pointer. A missing
    // title binding can be zero even though the SDK declared the symbol.
    Spawn volatile imported = &rfork_thread;
    Spawn spawn = imported;
    fprintf(output, "binding source=title_import name=rfork_thread present=%d\n", spawn != nullptr);
    for (int module : {1, 0x2001}) {
        Spawn queried = nullptr;
        const int resolved = sceKernelDlsym(module, "rfork_thread", &queried);
        fprintf(output, "lookup module=%#x name=rfork_thread status=%d present=%d same_import=%d\n",
            module, resolved, queried != nullptr, queried != nullptr && queried == spawn);
    }
    fsync(fileno(output)); // Last durable record if the system rejects the call.
    if (!valid || !elevated || !spawn) {
        fprintf(output, "result=PRECONDITION_FAILED\n");
        fclose(output);
        return true;
    }
    PwFdCensus before{}, after{};
    pw_fd_census(&before);
    void *stack = mmap(nullptr, StackSize, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    void *memory = mmap(nullptr, 16384, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    int sockets[2]{-1, -1};
    const int paired = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets);
    fprintf(output, "setup stack=%d memory=%d socketpair=%d errno=%d\n",
        stack != MAP_FAILED, memory != MAP_FAILED, paired, errno);
    if (stack != MAP_FAILED && memory != MAP_FAILED && paired == 0) {
        auto *word = static_cast<volatile unsigned *>(memory);
        *word = ParentValue;
        Context context{sockets[0], sockets[1], word};
        struct sigaction ignore{}, previous{}, previous_pipe{};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        const int ignored = sigaction(SIGSYS, &ignore, &previous);
        const int ignored_pipe = sigaction(SIGPIPE, &ignore, &previous_pipe);
        fprintf(output, "spawn flags=%#x sigsys_ignore=%d\n", RFPROC | RFFDG, ignored);
        fsync(fileno(output));
        errno = 0;
        const pid_t pid = spawn(RFPROC | RFFDG, static_cast<char *>(stack) + StackSize, child, &context);
        const int error = errno;
        fprintf(output, "spawn returned=%d errno=%d\n", int(pid), error);
        if (pid > 0) {
            close(sockets[1]); sockets[1] = -1;
            Reply reply{};
            const bool acknowledged = ready(sockets[0], POLLIN) &&
                read(sockets[0], &reply, sizeof(reply)) == sizeof(reply);
            const char release = 'R';
            const bool released = ready(sockets[0], POLLOUT) && write(sockets[0], &release, 1) == 1;
            fprintf(output, "ack=%d child=%d child_euid=%u child_value=%#x parent_value=%#x release=%d\n",
                acknowledged, int(reply.pid), unsigned(reply.uid), reply.value, *word, released);
            int waited_status = 0;
            pid_t waited = 0;
            for (int attempt = 0; attempt < 300 && waited == 0; ++attempt) {
                waited = waitpid(pid, &waited_status, WNOHANG);
                if (waited < 0 && errno == EINTR) waited = 0;
                if (!waited) usleep(10000);
            }
            const bool normal = waited == pid && WIFEXITED(waited_status) && WEXITSTATUS(waited_status) == ChildStatus;
            fprintf(output, "wait returned=%d status=%#x normal=%d errno=%d\n", int(waited), waited_status, normal, errno);
            if (waited != pid) {
                // Watchdog only for the PID created by this probe; never the title or another app.
                const int killed = kill(pid, SIGKILL);
                pid_t reaped = 0;
                for (int attempt = 0; attempt < 300 && reaped == 0; ++attempt) {
                    reaped = waitpid(pid, &waited_status, WNOHANG);
                    if (reaped < 0 && errno == EINTR) reaped = 0;
                    if (!reaped) usleep(10000);
                }
                fprintf(output, "watchdog kill=%d reap=%d status=%#x\n", killed, int(reaped), waited_status);
            }
            status = !(acknowledged && released && reply.pid == pid && pid != getpid() &&
                reply.value == ChildValue && *word == ParentValue && normal);
        }
        if (ignored == 0) sigaction(SIGSYS, &previous, nullptr);
        if (ignored_pipe == 0) sigaction(SIGPIPE, &previous_pipe, nullptr);
    }
    for (int fd : sockets) if (fd >= 0) close(fd);
    if (stack != MAP_FAILED) munmap(stack, StackSize);
    if (memory != MAP_FAILED) munmap(memory, 16384);
    pw_fd_census(&after);
    fprintf(output, "descriptors before=%u after=%u\n", before.open, after.open);
    if (before.open != after.open) status = 1;
    fprintf(output, "result=%s\n", status == 0 ? "PASS" : "FAIL");
    fsync(fileno(output));
    fclose(output);
    return true;
}
