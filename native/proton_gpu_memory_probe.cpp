// SPDX-License-Identifier: GPL-3.0-or-later
// Risk 1 of docs/GRAPHICS_PROCESS_BROKER.md: how much GPU memory one broker client can have, and from which pool.
//
// Arm A, shared memory, as the broker gives clients today: shm_open(SHM_ANON) objects the owner maps with GPU access
// and the helper maps at the same address, in 16 MiB chunks, every page touched on both sides. Both report their
// available flexible memory after each chunk. The fill stops at the first failure, at 1 GiB, or before the owner's
// flexible memory falls below a 96 MiB reserve, so the title's own heap is never starved.
//
// Arm B, device-local memory from the owner's direct-memory pool: radv_ps5_memory_alloc in 256 MiB chunks, mapped
// for the GPU in the owner only. The helper only reserves the same addresses with no access (its own mappings can
// never land there) and names them in the commands it builds; it never has a CPU mapping, which a helper is refused
// (graphics-child-gpu-broker-native-01/-02: EACCES). Up to 8 GiB, or until 2 GiB of direct memory would be left.
//
// Arm C, aliases of direct memory, instead of A and B: can either side map the other's direct memory? The helper
// maps the owner's (the route native-01/-02 found refused, now with other protections and with the kernel choosing
// the address), then the owner maps the helper's own direct memory for its CPU and GPU. A mapping that is accepted
// is checked: the pattern the other side wrote is read, and where the owner has GPU access the GPU writes a marker
// the helper reads. If either direction works, client memory the CPU writes need not come from flexible memory.
//
// Arm D, the charge, instead of A and B: which step of sharing a 16 MiB chunk costs flexible memory, and whether the
// owner's GPU access needs a CPU mapping that costs it. Five variants, each a fresh chunk: the owner maps it read-write
// and grants GPU access (the broker's way); maps it and grants nothing; maps it and gives it GPU access only; maps it
// read-only and gives the GPU read-write; and the broker's way with the helper mapping it without touching a page.
// Each step's flexible figure is logged; where the GPU has access, it writes a marker the helper reads.
//
// The GPU work is the one already-proved packet, RADV's completion write (proton_gpu_probe_packet): the helper builds
// it, the owner accepts exactly that packet and nothing else, submits it and reads where it wrote.
#include "proton_gpu_broker_probe.h"
#include "../src/pw_fd_census.h"
extern "C" {
#include "radv_ps5_platform.h"
}
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
extern "C" {
int sceSystemServiceGetAppStatus(void *);
int sceSystemServiceAddLocalProcess(int, const char *, const char *const *, const void *);
int sceSystemServiceKillLocalProcess(int, int);
int sceSystemServiceGetLocalProcessStatusList(void *, unsigned, unsigned *);
int sceKernelUsleep(unsigned);
int sceKernelMunmap(void *, size_t);
int sceKernelReserveVirtualRange(void **, size_t, int, size_t);
int sceKernelAvailableFlexibleMemorySize(size_t *);
int sceKernelMapDirectMemory(void **, size_t, int, int, int64_t, size_t);
int sceKernelMprotect(const void *, size_t, int);
}

namespace {
constexpr uint64_t kSharedChunk = 16ull << 20, kSharedCap = 1ull << 30, kFlexibleReserve = 96ull << 20;
constexpr uint64_t kDirectChunk = 256ull << 20, kDirectCap = 8ull << 30, kDirectReserve = 2ull << 30;
constexpr uint64_t kSharedBase = 0x6000000000ull, kCommandAddress = 0x6f00000000ull;
constexpr uint64_t kAliasBytes = 16ull << 20, kHelperAlias = 0x6400000000ull, kOwnerAlias = 0x6500000000ull;
constexpr uint64_t kChargeBase = 0x6600000000ull, kChargeChunk = 16ull << 20;
constexpr unsigned kMaxShared = unsigned(kSharedCap / kSharedChunk), kMaxDirect = unsigned(kDirectCap / kDirectChunk);

struct Options { uint32_t size; int32_t fd, crash; uint32_t other[15]; };
static_assert(sizeof(Options) == 72);
struct Shared { uint64_t address = 0; int fd = -1; };

int helper_socket = -1, helper_pid = -1;

bool ready(int socket, short events)
{
    pollfd p{socket, events, 0};
    int result;
    do { result = poll(&p, 1, 15000); } while (result < 0 && errno == EINTR);
    return result == 1 && (p.revents & events);
}

// One request and its reply; fd, when given, travels with the request.
bool ask(uint32_t operation, uint64_t address, uint64_t bytes, int64_t extra, uint32_t value, ProtonGpuProbeReply &reply,
         int fd = -1)
{
    ProtonGpuProbeMessage message{PROTON_GPU_PROBE_MAGIC, operation, value, 0, extra, address, bytes};
    iovec vector{&message, sizeof(message)};
    union { cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control{};
    msghdr packet{};
    packet.msg_iov = &vector;
    packet.msg_iovlen = 1;
    if (fd >= 0) {
        packet.msg_control = control.bytes;
        packet.msg_controllen = sizeof(control.bytes);
        auto *header = CMSG_FIRSTHDR(&packet);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    }
    reply = ProtonGpuProbeReply{};
    return ready(helper_socket, POLLOUT) && sendmsg(helper_socket, &packet, MSG_NOSIGNAL) == sizeof(message) &&
           ready(helper_socket, POLLIN) && recv(helper_socket, &reply, sizeof(reply), 0) == sizeof(reply) &&
           reply.magic == PROTON_GPU_PROBE_MAGIC && reply.pid == helper_pid;
}

size_t flexible()
{
    size_t available = 0;
    return sceKernelAvailableFlexibleMemorySize(&available) ? 0 : available;
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

[[noreturn]] void retain(FILE *out, const char *reason)
{
    fprintf(out, "gpu-memory unsafe_to_close=1 reason=%s\n", reason);
    fsync(fileno(out));
    for (;;) sceKernelUsleep(100000);
}

// The helper builds RADV's completion packet at command (inside the shared command page) naming marker; the owner
// accepts exactly that packet, submits it and waits for the GPU's write, which it reads at marker (owner_marker is
// the owner's CPU view of it).
bool gpu_write(FILE *out, uint8_t *command, uint64_t marker, volatile uint32_t *owner_marker, uint32_t value,
               int &submit, uint32_t &gpu)
{
    *owner_marker = 0;
    radv_ps5_cpu_flush((const void *)owner_marker, 4);
    ProtonGpuProbeReply reply{};
    if (!ask(PROTON_GPU_MEM_BUILD, (uint64_t)command, 32, (int64_t)marker, value, reply) || reply.status) return false;
    uint32_t expected[8];
    proton_gpu_probe_packet(expected, marker, value);
    radv_ps5_cpu_flush(command, 32);
    if (memcmp(command, expected, sizeof(expected))) {
        submit = EINVAL;
        return false;
    }
    submit = radv_ps5_submit(reinterpret_cast<uint32_t *>(command), 8, owner_marker, value);
    if (submit) retain(out, "submit_error");
    const uint64_t deadline = radv_ps5_now_ns() + 5000000000ull;
    for (;;) {
        radv_ps5_cpu_flush((const void *)owner_marker, 4);
        if (*owner_marker == value || radv_ps5_now_ns() >= deadline) break;
        sceKernelUsleep(1000);
    }
    gpu = *owner_marker;
    if (gpu != value) retain(out, "marker_timeout");
    return true;
}
} // namespace

int proton_gpu_memory_probe(FILE *out, const char *identity, bool owner_touch, bool direct_arm, unsigned arm)
{
    const bool alias_arm = arm == 1, charge_arm = arm == 2;
    uint32_t app[4]{};
    bool okay = sceSystemServiceGetAppStatus(app) == 0 && app[0] > 0 && radv_ps5_platform_init();
    if (okay) okay = proton_gpu_broker_warm(out);
    PwFdCensus before{}, after{};
    pw_fd_census(&before);
    const size_t flexible_before = flexible();
    const uint64_t direct_before = radv_ps5_memory_available_bytes();
    fprintf(out, "gpu-memory baseline owner=%d flexible=%zu direct=%llu pool=%llu descriptors=%u owner_touch=%d direct_arm=%d "
                 "alias_arm=%d\n",
            int(getpid()), flexible_before, (unsigned long long)direct_before,
            (unsigned long long)radv_ps5_memory_pool_bytes(), before.open, owner_touch, direct_arm, arm);
    if (arm) direct_arm = false;
    fsync(fileno(out));

    // One helper, spawned as the broker spawns its clients.
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
        const char *args[]{"/app0/service-native-helper.bin", "--service-native-memory-helper", identity, nullptr};
        service = sceSystemServiceAddLocalProcess(int(app[0]), args[0], args, &options);
        close(pair[1]);
        helper_socket = pair[0];
        ProtonGpuProbeReply hello{};
        okay = service > 0 && ready(helper_socket, POLLIN) &&
               recv(helper_socket, &hello, sizeof(hello), 0) == sizeof(hello) && hello.magic == PROTON_GPU_PROBE_MAGIC &&
               hello.pid > 0 && hello.pid != getpid() && hello.status == 0;
        helper_pid = hello.pid;
        fprintf(out, "gpu-memory helper service=%d pid=%d hello=%d elevation=%d helper_flexible_mib=%u\n", service,
                helper_pid, okay, hello.elevation, hello.value);
        fsync(fileno(out));
    }

    // The command page both arms' commands are built in: shared, mapped in both, GPU-readable in the owner.
    Shared command{};
    bool command_ready = false;
    if (okay) {
        void *at = (void *)kCommandAddress;
        command.fd = shm_open(SHM_ANON, O_RDWR, 0600);
        command_ready = command.fd >= 0 && ftruncate(command.fd, 16384) == 0 &&
                        sceKernelReserveVirtualRange(&at, 16384, 0, 16384) == 0 && at == (void *)kCommandAddress &&
                        mmap(at, 16384, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, command.fd, 0) == at &&
                        radv_ps5_memory_grant_gpu(at, 16384);
        if (command_ready) {
            command.address = kCommandAddress;
            memset(at, 0, 16384);
            ProtonGpuProbeReply reply{};
            command_ready = ask(PROTON_GPU_MEM_MAP, kCommandAddress, 16384, 0, 0, reply, command.fd) && !reply.status;
        }
        fprintf(out, "gpu-memory command_page=%d\n", command_ready);
        fsync(fileno(out));
        okay = command_ready;
    }
    auto *command_words = reinterpret_cast<uint8_t *>(kCommandAddress);

    // Arm A: shared memory.
    Shared shared[kMaxShared];
    unsigned shared_count = 0, shared_checks = 0, shared_passed = 0;
    uint64_t shared_total = 0;
    const char *shared_stop = "cap";
    while (okay && !arm && shared_count < kMaxShared) {
        const size_t owner_before = flexible();
        if (owner_before < kFlexibleReserve + kSharedChunk) { shared_stop = "owner_flexible_reserve"; break; }
        Shared &s = shared[shared_count];
        const uint64_t address = kSharedBase + shared_count * kSharedChunk;
        void *at = (void *)address;
        int stage = 0;
        s.fd = shm_open(SHM_ANON, O_RDWR, 0600);
        if (s.fd >= 0) stage = 1;
        if (stage == 1 && ftruncate(s.fd, kSharedChunk) == 0) stage = 2;
        if (stage == 2 && sceKernelReserveVirtualRange(&at, kSharedChunk, 0, 16384) == 0 && at == (void *)address)
            stage = 3;
        if (stage == 3 && mmap(at, kSharedChunk, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, s.fd, 0) == at) stage = 4;
        if (stage == 4) {
            // The owner's mapping exists only to carry the GPU's access: owner_touch=0 leaves its pages to the helper.
            if (owner_touch)
                for (uint64_t page = 0; page < kSharedChunk; page += 16384)
                    *reinterpret_cast<volatile uint32_t *>(address + page) = uint32_t(page >> 14);
            stage = 5;
        }
        if (stage == 5 && radv_ps5_memory_grant_gpu(at, kSharedChunk)) stage = 6;
        const int error = errno;
        if (stage < 6) {
            if (stage >= 3) sceKernelMunmap(at, kSharedChunk);
            if (s.fd >= 0) close(s.fd);
            s = Shared{};
            fprintf(out, "gpu-memory shared chunk=%u owner_stage=%d errno=%d owner_flexible=%zu\n", shared_count, stage,
                    error, flexible());
            shared_stop = "owner_failure";
            break;
        }
        s.address = address;
        const size_t owner_touched = flexible();
        ProtonGpuProbeReply reply{};
        const bool mapped = ask(PROTON_GPU_MEM_MAP, address, kSharedChunk, 0, 0, reply, s.fd) && !reply.status;
        ++shared_count;
        if (mapped) shared_total += kSharedChunk;
        fprintf(out,
                "gpu-memory shared chunk=%u address=%#llx bytes=%llu owner_flexible_before=%zu owner_flexible_touched=%zu "
                "owner_flexible_after=%zu helper_map=%d helper_stage=%d helper_flexible_mib=%u total=%llu\n",
                shared_count - 1, (unsigned long long)address, (unsigned long long)kSharedChunk, owner_before,
                owner_touched, flexible(), mapped, reply.map_stage, reply.value, (unsigned long long)shared_total);
        fsync(fileno(out));
        if (!mapped) { shared_stop = "helper_failure"; break; }
        if (reply.value < 32) { shared_stop = "helper_flexible_reserve"; break; }
    }
    fprintf(out, "gpu-memory shared stop=%s chunks=%u total=%llu owner_flexible=%zu\n", shared_stop, shared_count,
            (unsigned long long)shared_total, flexible());
    fsync(fileno(out));
    // The GPU writes into the first and the last chunk the helper mapped; both sides read it.
    for (unsigned pick = 0; okay && shared_total && pick < 2; ++pick) {
        const unsigned chunk = pick ? unsigned(shared_total / kSharedChunk) - 1 : 0;
        const uint64_t marker = shared[chunk].address + kSharedChunk - 1024;
        const uint32_t value = 0x51000000u + chunk;
        int submit = -1;
        uint32_t gpu = 0;
        const bool built = gpu_write(out, command_words + 64 * pick, marker, (volatile uint32_t *)marker, value, submit, gpu);
        ProtonGpuProbeReply seen{};
        const bool read = built && ask(PROTON_GPU_MEM_READ, marker, 4, 0, 0, seen) && !seen.status;
        const bool verdict = built && !submit && gpu == value && read && seen.value == value;
        ++shared_checks;
        shared_passed += verdict;
        fprintf(out, "gpu-memory shared gpu chunk=%u marker=%#llx value=%#x submit=%d gpu=%#x helper=%#x verdict=%d\n",
                chunk, (unsigned long long)marker, value, submit, gpu, seen.value, verdict);
        fsync(fileno(out));
        if (!built) break;
    }
    // Release arm A: the helper's aliases first, then the backing.
    if (shared_count) {
        ProtonGpuProbeReply released{};
        const bool helper_released = ask(PROTON_GPU_MEM_RELEASE, kSharedBase, kSharedCap, 0, 0, released) && !released.status;
        for (unsigned i = 0; i < shared_count; ++i) {
            if (shared[i].address) sceKernelMunmap((void *)shared[i].address, kSharedChunk);
            if (shared[i].fd >= 0) close(shared[i].fd);
            shared[i] = Shared{};
        }
        fprintf(out, "gpu-memory shared released helper=%d helper_flexible_mib=%u owner_flexible=%zu\n", helper_released,
                released.value, flexible());
        fsync(fileno(out));
        okay &= helper_released;
    }

    // Arm B: device-local memory from the owner's direct pool.
    radv_ps5_memory direct[kMaxDirect]{};
    unsigned direct_count = 0, direct_checks = 0, direct_passed = 0;
    uint64_t direct_total = 0;
    const char *direct_stop = "cap";
    const size_t flexible_arm_b = flexible();
    while (okay && direct_arm && direct_count < kMaxDirect) {
        const uint64_t available = radv_ps5_memory_available_bytes();
        if (available < kDirectReserve + kDirectChunk) { direct_stop = "direct_reserve"; break; }
        radv_ps5_memory &d = direct[direct_count];
        if (!radv_ps5_memory_alloc(kDirectChunk, 2u << 20, false, &d)) {
            fprintf(out, "gpu-memory direct chunk=%u owner_alloc=0 direct_available=%llu\n", direct_count,
                    (unsigned long long)available);
            direct_stop = "owner_failure";
            break;
        }
        ++direct_count;
        ProtonGpuProbeReply reply{};
        const bool reserved = ask(PROTON_GPU_MEM_RESERVE, (uint64_t)d.cpu, d.bytes, 0, 0, reply) && !reply.status;
        if (reserved) direct_total += d.bytes;
        fprintf(out,
                "gpu-memory direct chunk=%u address=%#llx bytes=%llu helper_reserve=%d helper_stage=%d "
                "direct_available=%llu owner_flexible=%zu helper_flexible_mib=%u total=%llu\n",
                direct_count - 1, (unsigned long long)(uint64_t)d.cpu, (unsigned long long)d.bytes, reserved,
                reply.map_stage, (unsigned long long)radv_ps5_memory_available_bytes(), flexible(), reply.value,
                (unsigned long long)direct_total);
        fsync(fileno(out));
        if (!reserved) { direct_stop = "helper_reserve_failure"; break; }
    }
    fprintf(out, "gpu-memory direct stop=%s chunks=%u total=%llu direct_available=%llu owner_flexible=%zu "
                 "flexible_before_arm=%zu\n",
            direct_stop, direct_count, (unsigned long long)direct_total,
            (unsigned long long)radv_ps5_memory_available_bytes(), flexible(), flexible_arm_b);
    fsync(fileno(out));
    // Every chunk: the GPU writes its last kilobyte from a command the helper built; the owner reads it.
    const unsigned direct_reserved = unsigned(direct_total / kDirectChunk);
    for (unsigned chunk = 0; okay && chunk < direct_reserved; ++chunk) {
        const uint64_t marker = (uint64_t)direct[chunk].cpu + direct[chunk].bytes - 1024;
        const uint32_t value = 0xd1000000u + chunk;
        int submit = -1;
        uint32_t gpu = 0;
        const bool built =
            gpu_write(out, command_words + 64 * (chunk % 64), marker, (volatile uint32_t *)marker, value, submit, gpu);
        const bool verdict = built && !submit && gpu == value;
        ++direct_checks;
        direct_passed += verdict;
        fprintf(out, "gpu-memory direct gpu chunk=%u marker=%#llx value=%#x submit=%d gpu=%#x verdict=%d\n", chunk,
                (unsigned long long)marker, value, submit, gpu, verdict);
        fsync(fileno(out));
        if (!built) break;
    }
    // Release arm B: the helper's reservations, then the direct memory.
    if (direct_count) {
        ProtonGpuProbeReply released{};
        const bool helper_released = ask(PROTON_GPU_MEM_RELEASE, 0, 0, 0, 0, released) && !released.status;
        for (unsigned i = 0; i < direct_count; ++i) radv_ps5_memory_free(&direct[i]);
        fprintf(out, "gpu-memory direct released helper=%d direct_available=%llu owner_flexible=%zu\n", helper_released,
                (unsigned long long)radv_ps5_memory_available_bytes(), flexible());
        fsync(fileno(out));
        okay &= helper_released;
    }

    // Arm C: aliases of direct memory, in both directions.
    unsigned alias_mapped = 0, alias_checks = 0, alias_passed = 0, alias_steps = 0;
    bool alias_complete = false;
    unsigned check_slot = 0;
    const auto alias_gpu = [&](uint64_t marker, volatile uint32_t *owner_marker, uint64_t helper_marker, uint32_t value) {
        int submit = -1;
        uint32_t gpu = 0;
        const bool built = gpu_write(out, command_words + 64 * (check_slot++ % 64), marker, owner_marker, value, submit, gpu);
        ProtonGpuProbeReply seen{};
        const bool read = built && ask(PROTON_GPU_MEM_READ, helper_marker, 4, 0, 0, seen) && !seen.status;
        const bool verdict = built && !submit && gpu == value && read && seen.value == value;
        ++alias_checks;
        alias_passed += verdict;
        fprintf(out, "gpu-memory alias gpu marker=%#llx helper_marker=%#llx value=%#x submit=%d gpu=%#x helper=%#x verdict=%d\n",
                (unsigned long long)marker, (unsigned long long)helper_marker, value, submit, gpu, seen.value, verdict);
        fsync(fileno(out));
    };
    if (okay && alias_arm) {
        // C1: the helper maps the owner's direct memory. The owner marks every page first.
        radv_ps5_memory owned{};
        const bool allocated = radv_ps5_memory_alloc(kAliasBytes, 2u << 20, false, &owned);
        if (allocated) {
            for (uint64_t page = 0; page < kAliasBytes; page += 16384)
                reinterpret_cast<volatile uint32_t *>((uint8_t *)owned.cpu + page)[3] = 0x0e0e0000u | uint32_t(page >> 14);
            radv_ps5_cpu_flush(owned.cpu, kAliasBytes);
        }
        fprintf(out, "gpu-memory alias owner_dmem=%d address=%#llx physical=%#llx\n", allocated,
                (unsigned long long)(uint64_t)owned.cpu, (unsigned long long)owned.physical);
        fsync(fileno(out));
        struct Variant { const char *name; uint32_t protection, flags; };
        static const Variant into_helper[]{{"cpu_rw_fixed", 0x03, 0x10}, {"cpu_rw_any", 0x03, 0}, {"cpu_r_fixed", 0x01, 0x10}};
        for (const Variant &v : into_helper) {
            if (!allocated || !okay) break;
            ProtonGpuProbeReply reply{};
            const bool asked = ask(PROTON_GPU_MEM_DMAP, kOwnerAlias, kAliasBytes, owned.physical,
                                   v.protection | (v.flags << 16), reply);
            const bool mapped = asked && !reply.status;
            const uint64_t at = mapped ? uint64_t(reply.value) << 14 : 0;
            ++alias_steps;
            alias_mapped += mapped;
            fprintf(out, "gpu-memory alias helper_maps_owner variant=%s asked=%d stage=%d result=%#x mapped=%d address=%#llx\n",
                    v.name, asked, reply.map_stage, unsigned(reply.elevation), mapped, (unsigned long long)at);
            fsync(fileno(out));
            if (!asked) { okay = false; break; }
            if (mapped && v.protection == 0x03) {
                // The helper reads the owner's marks on the first and last page, then the GPU writes the owner's
                // memory and the helper reads that through its own alias.
                unsigned marks = 0;
                for (const uint64_t page : (const uint64_t[]){0, kAliasBytes - 16384}) {
                    ProtonGpuProbeReply seen{};
                    marks += ask(PROTON_GPU_MEM_READ, at + page + 12, 4, 0, 0, seen) && !seen.status &&
                             seen.value == (0x0e0e0000u | uint32_t(page >> 14));
                }
                fprintf(out, "gpu-memory alias helper_reads_owner marks=%u/2\n", marks);
                const uint64_t offset = kAliasBytes - 1024;
                alias_gpu((uint64_t)owned.cpu + offset, (volatile uint32_t *)((uint8_t *)owned.cpu + offset), at + offset,
                          0xa1000000u + alias_steps);
            }
            if (mapped) {
                ProtonGpuProbeReply released{};
                if (!ask(PROTON_GPU_MEM_RELEASE, at, kAliasBytes, 0, 0, released) || released.status) okay = false;
            }
        }
        if (allocated) radv_ps5_memory_free(&owned);

        // C2: the owner maps the helper's own direct memory.
        const uint64_t direct_c2 = radv_ps5_memory_available_bytes();
        ProtonGpuProbeReply own{};
        const bool helper_dmem = okay && ask(PROTON_GPU_MEM_DMEM, kHelperAlias, kAliasBytes, 0, 0, own) && !own.status;
        const int64_t helper_physical = helper_dmem ? int64_t(own.value) << 16 : -1;
        fprintf(out, "gpu-memory alias helper_dmem=%d stage=%d result=%#x physical=%#llx direct_drop=%lld\n", helper_dmem,
                own.map_stage, unsigned(own.elevation), (unsigned long long)helper_physical,
                (long long)(direct_c2 - radv_ps5_memory_available_bytes()));
        fsync(fileno(out));
        static const Variant into_owner[]{
            {"cpu_gpu_fixed", 0x33, 0x10}, {"cpu_gpu_any", 0x33, 0}, {"cpu_rw_fixed", 0x03, 0x10}, {"gpu_rw_fixed", 0x30, 0x10}};
        for (const Variant &v : into_owner) {
            if (!helper_dmem || !okay) break;
            void *at = (void *)kHelperAlias;
            bool reserved = true;
            if (v.flags & 0x10) {
                reserved = sceKernelReserveVirtualRange(&at, kAliasBytes, 0, 16384) == 0 && at == (void *)kHelperAlias;
                if (!reserved && at && at != (void *)kHelperAlias) sceKernelMunmap(at, kAliasBytes);
            } else {
                at = nullptr;
            }
            int result = -1;
            if (reserved) result = sceKernelMapDirectMemory(&at, kAliasBytes, int(v.protection), int(v.flags),
                                                            helper_physical, 16384);
            const bool mapped = reserved && result == 0 && at;
            if (!mapped && reserved && (v.flags & 0x10)) sceKernelMunmap((void *)kHelperAlias, kAliasBytes);
            ++alias_steps;
            alias_mapped += mapped;
            unsigned marks = 0;
            if (mapped && (v.protection & 1))
                for (uint64_t page = 0; page < kAliasBytes; page += 16384) {
                    radv_ps5_cpu_flush((uint8_t *)at + page + 8, 4);
                    marks += reinterpret_cast<volatile uint32_t *>((uint8_t *)at + page)[2] == (0xc0de0000u | uint32_t(page >> 14));
                }
            fprintf(out, "gpu-memory alias owner_maps_helper variant=%s reserved=%d result=%#x mapped=%d address=%#llx "
                         "marks=%u/%llu\n",
                    v.name, reserved, unsigned(result), mapped, (unsigned long long)(uint64_t)at, marks,
                    (unsigned long long)(kAliasBytes / 16384));
            fsync(fileno(out));
            if (mapped && v.protection == 0x33) {
                const uint64_t offset = kAliasBytes - 2048;
                alias_gpu((uint64_t)at + offset, (volatile uint32_t *)((uint8_t *)at + offset), kHelperAlias + offset,
                          0xa2000000u + alias_steps);
            }
            if (mapped) sceKernelMunmap(at, kAliasBytes);
        }
        if (helper_dmem) {
            ProtonGpuProbeReply released{};
            const bool helper_released =
                ask(PROTON_GPU_MEM_RELEASE, kHelperAlias, kAliasBytes, 0, 0, released) && !released.status;
            fprintf(out, "gpu-memory alias helper_released=%d direct_available=%llu\n", helper_released,
                    (unsigned long long)radv_ps5_memory_available_bytes());
            fsync(fileno(out));
            okay &= helper_released;
        }
        // Complete: every variant was tried, or the helper could not have direct memory of its own at all.
        alias_complete = alias_steps == 7 || (alias_steps == 3 && !helper_dmem);
        fprintf(out, "gpu-memory alias summary steps=%u mapped=%u gpu_checks=%u/%u complete=%d\n", alias_steps,
                alias_mapped, alias_passed, alias_checks, alias_complete);
        fsync(fileno(out));
    }

    // Arm D: what sharing costs, step by step.
    unsigned charge_variants = 0, charge_checks = 0, charge_passed = 0;
    if (okay && charge_arm) {
        struct Variant { const char *name; int map_prot; int mprotect; bool gpu; bool helper_touch; };
        static const Variant variants[]{
            {"cpu_rw_grant", PROT_READ | PROT_WRITE, 0x33, true, true},
            {"cpu_rw_nogrant", PROT_READ | PROT_WRITE, 0, false, true},
            {"gpu_only", PROT_READ | PROT_WRITE, 0x30, true, true},
            {"cpu_r_gpu_rw", PROT_READ, 0x31, true, true},
            {"grant_helper_untouched", PROT_READ | PROT_WRITE, 0x33, true, false},
        };
        unsigned index = 0;
        for (const Variant &v : variants) {
            if (!okay) break;
            const uint64_t address = kChargeBase + uint64_t(index++) * 2 * kChargeChunk;
            void *at = (void *)address;
            const size_t f0 = flexible();
            const int fd = shm_open(SHM_ANON, O_RDWR, 0600);
            const bool sized = fd >= 0 && ftruncate(fd, kChargeChunk) == 0;
            const size_t f1 = flexible();
            const bool placed = sized && sceKernelReserveVirtualRange(&at, kChargeChunk, 0, 16384) == 0 && at == (void *)address;
            const bool mapped = placed && mmap(at, kChargeChunk, v.map_prot, MAP_SHARED | MAP_FIXED, fd, 0) == at;
            const size_t f2 = flexible();
            const int protect = mapped && v.mprotect ? sceKernelMprotect(at, kChargeChunk, v.mprotect) : 0;
            const size_t f3 = flexible();
            ProtonGpuProbeReply reply{};
            const bool helper = mapped && !protect &&
                                ask(PROTON_GPU_MEM_MAP, address, kChargeChunk, 0, v.helper_touch ? 0 : 1, reply, fd) && !reply.status;
            const size_t f4 = flexible();
            int verdict = -1;
            if (helper && v.gpu) {
                // The owner may have no CPU view: the helper builds the packet and reads the GPU's write.
                const uint64_t marker = address + kChargeChunk - 4096;
                const uint32_t value = 0xc4000000u + index;
                ProtonGpuProbeReply built{}, seen{};
                verdict = 0;
                if (ask(PROTON_GPU_MEM_BUILD, (uint64_t)(command_words + 64 * (index % 64)), 32, (int64_t)marker, value, built) &&
                    !built.status) {
                    uint32_t expected[8];
                    proton_gpu_probe_packet(expected, marker, value);
                    radv_ps5_cpu_flush(command_words + 64 * (index % 64), 32);
                    if (!memcmp(command_words + 64 * (index % 64), expected, sizeof(expected))) {
                        const int submit = radv_ps5_submit(reinterpret_cast<uint32_t *>(command_words + 64 * (index % 64)), 8,
                                                           nullptr, value);
                        if (submit) retain(out, "charge_submit_error");
                        const uint64_t deadline = radv_ps5_now_ns() + 5000000000ull;
                        while (radv_ps5_now_ns() < deadline) {
                            if (ask(PROTON_GPU_MEM_READ, marker, 4, 0, 0, seen) && !seen.status && seen.value == value) break;
                            sceKernelUsleep(2000);
                        }
                        verdict = seen.value == value;
                        if (!verdict) retain(out, "charge_marker_timeout");
                    }
                }
                ++charge_checks;
                charge_passed += verdict == 1;
            }
            ProtonGpuProbeReply released{};
            const bool helper_released = !helper || (ask(PROTON_GPU_MEM_RELEASE, address, kChargeChunk, 0, 0, released) && !released.status);
            const size_t f5 = flexible();
            if (placed) sceKernelMunmap(at, kChargeChunk);
            if (fd >= 0) close(fd);
            const size_t f6 = flexible();
            ++charge_variants;
            fprintf(out,
                    "gpu-memory charge variant=%s sized=%d mapped=%d mprotect=%#x helper=%d helper_mib=%u gpu=%d "
                    "f0=%zu after_object=%zu after_owner_map=%zu after_protect=%zu after_helper_map=%zu "
                    "after_helper_release=%zu after_owner_release=%zu\n",
                    v.name, sized, mapped, unsigned(protect), helper, reply.value, verdict, f0, f1, f2, f3, f4, f5, f6);
            fsync(fileno(out));
            okay &= helper_released && f6 == f0;
        }
        fprintf(out, "gpu-memory charge summary variants=%u gpu_checks=%u/%u\n", charge_variants, charge_passed, charge_checks);
        fsync(fileno(out));
    }

    // Close the helper through Sony, then the command page.
    if (service > 0) {
        if (command_ready) {
            ProtonGpuProbeReply released{};
            (void)ask(PROTON_GPU_MEM_RELEASE, 0, 0, 0, 0, released);
        }
        const int closed = sceSystemServiceKillLocalProcess(int(app[0]), service);
        for (unsigned n = 0; n < 200 && listed(service); ++n) sceKernelUsleep(10000);
        const bool absent = !listed(service);
        fprintf(out, "gpu-memory helper closed=%#x absent=%d\n", unsigned(closed), absent);
        fsync(fileno(out));
        if (!absent) retain(out, "live_helper");
        okay &= closed == 0;
    }
    if (helper_socket >= 0) close(helper_socket);
    if (command.address) sceKernelMunmap((void *)command.address, 16384);
    if (command.fd >= 0) close(command.fd);

    pw_fd_census(&after);
    const size_t flexible_after = flexible();
    const uint64_t direct_after = radv_ps5_memory_available_bytes();
    const bool restored = flexible_after == flexible_before && direct_after == direct_before && after.open == before.open;
    fprintf(out,
            "gpu-memory summary shared_bytes=%llu shared_gpu_checks=%u/%u direct_bytes=%llu direct_gpu_checks=%u/%u "
            "flexible_before=%zu flexible_after=%zu direct_before=%llu direct_after=%llu descriptors_before=%u "
            "descriptors_after=%u restored=%d\n",
            (unsigned long long)shared_total, shared_passed, shared_checks, (unsigned long long)direct_total, direct_passed,
            direct_checks, flexible_before, flexible_after, (unsigned long long)direct_before,
            (unsigned long long)direct_after, before.open, after.open, restored);
    fsync(fileno(out));
    okay &= (arm || shared_checks > 0) && shared_passed == shared_checks && (!direct_arm || direct_checks > 0) &&
            direct_passed == direct_checks && alias_passed == alias_checks && restored;
    if (alias_arm) okay &= alias_complete;
    if (charge_arm) okay &= charge_variants == 5 && charge_passed == charge_checks && charge_checks == 4;
    return okay ? 0 : 1;
}
