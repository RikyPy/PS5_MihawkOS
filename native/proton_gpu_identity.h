/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PROTON_GPU_IDENTITY_H
#define PROTON_GPU_IDENTITY_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#ifdef __cplusplus
extern "C" {
#endif
int sceKernelGetProcessAbiType(int);
int sceKernelGetAppInfo(int, void *);
#ifdef __cplusplus
}
#endif

/* Read-only diagnostics. The pinned 10.01 GetAppInfo wrapper requests 88
 * bytes. Do not print its title ID or opaque trailing application metadata. */
static inline void proton_gpu_identity(char *line, size_t capacity, const char *role)
{
    union { uint64_t alignment; unsigned char bytes[120]; } info;
    memset(&info, 0xa5, sizeof(info));
    const int pid = (int)getpid();
    const int abi = sceKernelGetProcessAbiType(pid);
    const int status = sceKernelGetAppInfo(pid, info.bytes);
    uint32_t words[4] = {0};
    if (!status) memcpy(words, info.bytes, sizeof(words));
    int intact = 1;
    for (size_t i = 88; i < sizeof(info.bytes); ++i)
        if (info.bytes[i] != 0xa5) intact = 0;
    snprintf(line, capacity,
             "gpu identity role=%s pid=%d abi=%#x app_status=%#x guard=%d app=%u header4=%#x header8=%#x header12=%#x\n",
             role, pid, (unsigned)abi, (unsigned)status, intact,
             words[0], words[1], words[2], words[3]);
}
#endif
