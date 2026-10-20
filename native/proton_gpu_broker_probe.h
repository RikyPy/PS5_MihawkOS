/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PROTON_GPU_BROKER_PROBE_H
#define PROTON_GPU_BROKER_PROBE_H
#include <stdint.h>

enum { PROTON_GPU_PROBE_MAGIC = 0x47505531, PROTON_GPU_MAP = 1,
       PROTON_GPU_SUBMIT = 2, PROTON_GPU_UNMAP = 3 };
/* The memory-ceiling probe's helper (proton_gpu_memory_probe): map a shared chunk at its address and touch it,
 * reserve a range with no access, build the completion packet in a mapped range naming any GPU address (physical),
 * read a mapped word, release every range (or those inside address..address+bytes). */
enum { PROTON_GPU_MEM_MAP = 10, PROTON_GPU_MEM_RESERVE = 11, PROTON_GPU_MEM_BUILD = 12, PROTON_GPU_MEM_READ = 13,
       PROTON_GPU_MEM_RELEASE = 14, PROTON_GPU_MEM_DMEM = 15, PROTON_GPU_MEM_DMAP = 16 };
/* The open-file budget probe's helper (proton_fd_budget_probe): measure its headroom (value: files it could open,
 * elevation: the refusal's errno, map_stage: regular files it holds), open value files and keep them, keep the
 * descriptor that came with the request, hand back the value'th kept file (and close its own copy), close them all. */
enum { PROTON_FD_BUDGET_HEADROOM = 20, PROTON_FD_BUDGET_OPEN = 21, PROTON_FD_BUDGET_RECEIVE = 22,
       PROTON_FD_BUDGET_GIVE = 23, PROTON_FD_BUDGET_CLOSE = 24, PROTON_FD_BUDGET_CAP = 1024 };
struct ProtonGpuProbeMessage {
    uint32_t magic, operation, value, reserved;
    int64_t physical;
    uint64_t address, bytes;
};
struct ProtonGpuProbeReply {
    uint32_t magic; int32_t pid, status; uint32_t value;
    int32_t elevation, map_stage;
};

/* Mesa's MIT-licensed RADV PS5 completion packet, radv_ps5_cs.c:
 * RELEASE_MEM, CACHE_FLUSH_AND_INV_TS_EVENT, GCR actions 0x30c.
 * Only this already-tested packet is accepted by the bounded probe. */
#include "../.deps/graphics-foundation/tooling/gpu-broker/completion.h"
static inline void proton_gpu_probe_packet(uint32_t words[8],uint64_t marker,uint32_t value)
{ ps5_gpu_completion_packet(words,marker,value); }

#ifdef __cplusplus
#include <stdio.h>
bool proton_gpu_broker_warm(FILE *output);
int proton_gpu_server_probe(FILE *output,const char *identity,bool draw,bool present,bool local);
int proton_gpu_broker_probe(FILE *output, const char *identity, bool shared_descriptor,
                           uint64_t shared_bytes, bool window32);
/* owner_touch: the owner writes every page of each shared chunk itself (else only the helper does);
 * direct_arm: also fill device-local memory from the owner's direct pool. */
/* arm: 0 the shared and direct fills, 1 direct-memory aliases, 2 what sharing costs in flexible memory. */
int proton_gpu_memory_probe(FILE *output, const char *identity, bool owner_touch, bool direct_arm, unsigned arm);
int proton_fd_budget_probe(FILE *output, const char *identity);
/* The title's half of the channel test with a payload-loader process (native/proton_channel_probe.cpp). */
int proton_channel_probe(FILE *output, const char *identity);
/* The title starts the channel test's payload itself, through the console's payload launcher. */
int proton_payload_start_probe(FILE *output, const char *identity);
#endif
#endif
