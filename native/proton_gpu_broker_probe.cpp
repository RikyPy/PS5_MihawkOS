// SPDX-License-Identifier: GPL-3.0-or-later
// Isolated two-client proof. No Wine, display or helper-side AGC initialization.
#include "proton_gpu_broker_probe.h"
#include "../src/pw_fd_census.h"
extern "C" {
#include "radv_ps5_platform.h"
}
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
extern "C" {
int sceSystemServiceGetAppStatus(void *);
int sceSystemServiceAddLocalProcess(int, const char *, const char *const *, const void *);
int sceSystemServiceKillLocalProcess(int, int);
int sceSystemServiceGetLocalProcessStatusList(void *, unsigned, unsigned *);
int sceKernelUsleep(unsigned);
int sceKernelMunmap(void *,size_t);
}

namespace {
struct Options { uint32_t size; int32_t fd, crash; uint32_t other[15]; };
static_assert(sizeof(Options) == 72);
struct Client {
    int socket = -1, service = -1, pid = -1, shared_fd = -1;
    radv_ps5_memory memory{};
    bool mapped = false;
};
bool ready(int socket, short events)
{
    pollfd p{socket, events, 0};
    int result;
    do { result = poll(&p, 1, 15000); } while (result < 0 && errno == EINTR);
    return result == 1 && (p.revents & events);
}
bool exchange(Client &c, const ProtonGpuProbeMessage &message, ProtonGpuProbeReply &reply, int fd = -1)
{
    iovec vector{const_cast<ProtonGpuProbeMessage *>(&message),sizeof(message)};
    union { cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control{};
    msghdr packet{};packet.msg_iov=&vector;packet.msg_iovlen=1;
    if(fd>=0) {
        packet.msg_control=control.bytes;packet.msg_controllen=sizeof(control.bytes);
        auto *header=CMSG_FIRSTHDR(&packet);header->cmsg_level=SOL_SOCKET;header->cmsg_type=SCM_RIGHTS;
        header->cmsg_len=CMSG_LEN(sizeof(int));memcpy(CMSG_DATA(header),&fd,sizeof(fd));
    }
    return ready(c.socket, POLLOUT) &&
           sendmsg(c.socket, &packet, MSG_NOSIGNAL) == sizeof(message) &&
           ready(c.socket, POLLIN) && recv(c.socket, &reply, sizeof(reply), 0) == sizeof(reply) &&
           reply.magic == PROTON_GPU_PROBE_MAGIC && reply.pid == c.pid;
}
bool listed(int id)
{
    struct Entry { int32_t id; char name[32]; } entries[16]{};
    unsigned count = 0;
    if (sceSystemServiceGetLocalProcessStatusList(entries,16,&count) || count > 16) return true;
    for (unsigned i = 0; i < count; ++i) if (entries[i].id == id) return true;
    return false;
}
bool request(Client &c, unsigned value, bool invalid, FILE *out, unsigned client)
{
    const bool last_page=!invalid && value%1000==3;
    ProtonGpuProbeMessage message{PROTON_GPU_PROBE_MAGIC, PROTON_GPU_SUBMIT, value, (invalid ? 1u : 0u) | (last_page ? 2u : 0u),
                                  c.memory.physical, (uint64_t)c.memory.cpu, c.memory.bytes};
    ProtonGpuProbeReply reply{};
    // Helper builds the packet and returns its own preparation verdict first.
    if (!exchange(c,message,reply) || reply.status) return false;
    uint32_t expected[8];
    auto *marker = reinterpret_cast<volatile uint32_t *>(c.memory.cpu + (last_page ? c.memory.bytes-1024 : 1024));
    proton_gpu_probe_packet(expected,(uint64_t)marker,value);
    radv_ps5_cpu_flush(c.memory.cpu,2048);
    radv_ps5_cpu_flush((const void *)marker,sizeof(*marker));
    const bool valid = memcmp(c.memory.cpu,expected,sizeof(expected)) == 0;
    int status = EINVAL;
    if (valid) {
        status = radv_ps5_submit(reinterpret_cast<uint32_t *>(c.memory.cpu),8,marker,value);
        if (!status) {
            const uint64_t deadline = radv_ps5_now_ns() + 5000000000ull;
            while (*marker != value && radv_ps5_now_ns() < deadline) sceKernelUsleep(1000);
            if (*marker != value) {
                fprintf(out,"gpu-broker unsafe_to_close=1 marker_timeout=1 client=%u\n",client);
                fsync(fileno(out));
                // Retain all GPU backing. Never replace a native title with a live submission.
                for (;;) sceKernelUsleep(100000);
            }
        }
    }
    ProtonGpuProbeReply completed{PROTON_GPU_PROBE_MAGIC,c.pid,status,*marker};
    const bool sent = ready(c.socket,POLLOUT) &&
        send(c.socket,&completed,sizeof(completed),MSG_NOSIGNAL) == sizeof(completed);
    ProtonGpuProbeReply observed{};
    const bool received = sent && ready(c.socket,POLLIN) &&
        recv(c.socket,&observed,sizeof(observed),0) == sizeof(observed) &&
        observed.magic == PROTON_GPU_PROBE_MAGIC && observed.pid == c.pid && observed.status == 0;
    fprintf(out,"gpu-broker client=%u pid=%d value=%u invalid=%d packet_valid=%d submit=%d gpu_value=%u child_observed=%u verdict=%d\n",
            client,c.pid,value,invalid,valid,status,*marker,observed.value,
            received && (invalid ? !valid && status == EINVAL && observed.value == 0 : valid && !status && observed.value == value));
    fsync(fileno(out));
    return received && (invalid ? !valid && status == EINVAL && observed.value == 0
                                : valid && !status && observed.value == value);
}
}


int proton_gpu_broker_probe(FILE *out, const char *identity, bool shared_descriptor,
                           uint64_t shared_bytes, bool window32)
{
    uint32_t app[4]{};
    PwFdCensus before{},after{};
    Client clients[2];
    bool okay = sceSystemServiceGetAppStatus(app) == 0 && app[0] > 0 && radv_ps5_platform_init();
    const uint64_t cold_memory=radv_ps5_memory_available_bytes();
    if(okay)okay=proton_gpu_broker_warm(out);
    pw_fd_census(&before);
    const uint64_t memory_before = radv_ps5_memory_available_bytes();
    fprintf(out,"gpu-broker owner=%d initialized=%d available_before=%llu shared_descriptor=%d cold_available=%llu\n",int(getpid()),okay,(unsigned long long)memory_before,shared_descriptor,(unsigned long long)cold_memory);
    fsync(fileno(out));
    fprintf(out,"gpu-broker shared_bytes=%llu window32=%d last_page_checks=2\n",(unsigned long long)shared_bytes,window32);
    for (unsigned i = 0; i < 2 && okay; ++i) {
        auto &c=clients[i];int pair[2]{-1,-1};
        if(shared_descriptor) {
            c.shared_fd=shm_open(SHM_ANON,O_RDWR,0600);
            okay=c.shared_fd>=0 && ftruncate(c.shared_fd,shared_bytes)==0;
            if(okay) {
                const uint64_t hint=(window32 ? 0x220000000ull : 0x5000000000ull)+i*0x4000000ull;
                void *address=mmap((void *)hint,shared_bytes,
                                   PROT_READ|PROT_WRITE,MAP_SHARED,c.shared_fd,0);
                okay=address!=MAP_FAILED;
                if(okay) {
                    c.memory.cpu=(uint8_t *)address;c.memory.bytes=shared_bytes;c.memory.physical=-1;
                    const uint64_t va=(uint64_t)address;
                    okay=va>=0x200000000ull && va<(1ull<<47) && !(va&16383) &&
                         (!window32 || (va>=0x200000000ull && va+shared_bytes<=0x300000000ull)) &&
                         radv_ps5_memory_grant_gpu(address,shared_bytes);
                }
            }
            fprintf(out,"gpu-broker client=%u shared_create=%d gpu_protection=%d error=%d\n",i,c.shared_fd>=0,okay,okay ? 0 : errno);
            fsync(fileno(out));
        } else okay=radv_ps5_memory_alloc(16384,16384,false,&c.memory);
        okay=okay && socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair)==0;
        if (!okay) break;
        memset(c.memory.cpu,0,c.memory.bytes);
        Options options{};options.size=sizeof(options);options.fd=pair[1];options.crash=1;
        options.other[0]=uint32_t(-1);options.other[3]=2;options.other[4]=0x80000000;
        const char *args[]{"/app0/service-native-helper.bin","--service-native-gpu-helper",identity,nullptr};
        c.service=sceSystemServiceAddLocalProcess(int(app[0]),args[0],args,&options);
        close(pair[1]);c.socket=pair[0];
        ProtonGpuProbeReply hello{};
        okay = c.service > 0 && ready(c.socket,POLLIN) && recv(c.socket,&hello,sizeof(hello),0) == sizeof(hello) &&
               hello.magic == PROTON_GPU_PROBE_MAGIC && hello.pid > 0 && hello.pid != getpid() && hello.status == 0;
        c.pid=hello.pid;
        fprintf(out,"gpu-broker client=%u hello=%d elevation=%d euid=%u\n",i,okay,hello.elevation,hello.value);
        fsync(fileno(out));
        if (!okay) break;
        ProtonGpuProbeMessage map{PROTON_GPU_PROBE_MAGIC,PROTON_GPU_MAP,0,0,c.memory.physical,
                                  (uint64_t)c.memory.cpu,c.memory.bytes};
        ProtonGpuProbeReply reply{};
        c.mapped=exchange(c,map,reply,c.shared_fd) && reply.status == 0;
        fprintf(out,"gpu-broker client=%u service=%d pid=%d cpu_alias=%d map_status=%#x elevation=%d euid=%u map_stage=%d\n",i,c.service,c.pid,c.mapped,unsigned(reply.status),reply.elevation,reply.value,reply.map_stage);
        fsync(fileno(out));okay=c.mapped;
    }
    if (okay) {
        fprintf(out,"gpu-broker live_clients=2 separate_native_processes=%d\n",clients[0].pid != clients[1].pid);
        okay = clients[0].pid != clients[1].pid;
        if(okay)okay=request(clients[0],999,true,out,0);
        for (unsigned cycle=1; cycle<=3 && okay; ++cycle)
            for (unsigned i=0; i<2 && okay; ++i)
                okay=request(clients[i],1000*(i+1)+cycle,false,out,i);
    }
    // One allocation belongs to each client. Unmap aliases before releasing backing.
    for (unsigned i=0;i<2;++i) {
        auto &c=clients[i];
        if (c.mapped) {
            ProtonGpuProbeMessage unmap{PROTON_GPU_PROBE_MAGIC,PROTON_GPU_UNMAP,0,0,0,0,0};
            ProtonGpuProbeReply reply{};
            const bool unmapped=exchange(c,unmap,reply) && !reply.status;
            okay &= unmapped;fprintf(out,"gpu-broker client=%u unmapped=%d\n",i,unmapped);
        }
        if(c.service>0) {
            const int closed=sceSystemServiceKillLocalProcess(int(app[0]),c.service);
            for(unsigned n=0;n<200 && listed(c.service);++n)sceKernelUsleep(10000);
            const bool absent=!listed(c.service);okay &= closed==0 && absent;
            fprintf(out,"gpu-broker client=%u closed=%#x absent=%d\n",i,unsigned(closed),absent);
            if(!absent) {
                fprintf(out,"gpu-broker unsafe_to_close=1 live_helper=1\n");fsync(fileno(out));
                for(;;)sceKernelUsleep(100000);
            }
        }
        if(c.socket>=0)close(c.socket);
        if(c.shared_fd>=0) {
            if(c.memory.cpu)okay &= sceKernelMunmap(c.memory.cpu,c.memory.bytes)==0;
            close(c.shared_fd);
        } else if(c.memory.cpu)radv_ps5_memory_free(&c.memory);
    }
    const uint64_t memory_after=radv_ps5_memory_available_bytes();
    pw_fd_census(&after);
    fprintf(out,"gpu-broker descriptors before=%u after=%u available_before=%llu available_after=%llu\n",
            before.open,after.open,(unsigned long long)memory_before,(unsigned long long)memory_after);
    okay &= memory_before==memory_after && before.open==after.open;
    return okay ? 0 : 1;
}
