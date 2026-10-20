/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Standalone SDK payload: locate saved FP registers using known sentinels. */
#include <signal.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <machine/sysarch.h>

static const uint64_t marker[4] __attribute__((aligned(32))) = {
    0x1122334455667788ULL, 0x8877665544332211ULL, 0x123456789abcdef0ULL, 0xfedcba9876543210ULL
};
static unsigned offsets[16], matches;
static uint64_t words[112];
static unsigned mxcsr_before, mxcsr_after;
static void handler(int signal, siginfo_t *info, void *opaque)
{
    ucontext_t *context = opaque;
    const unsigned char *bytes = opaque;
    (void)signal; (void)info;
    for (unsigned i = 0; i + 16 <= 2048; i += 8)
        if (!memcmp(bytes + i, marker, 16) && matches < 16) offsets[matches++] = i;
    memcpy(words, bytes + 256, sizeof(words));
    context->uc_mcontext.mc_rip += 2; /* movl (%rax), %eax */
}

int main(void)
{
    void *fs;
    struct sigaction action = {0}, old;
    sysarch(128, &fs);
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, &old);
    __asm__ volatile("stmxcsr %0\n\tvmovdqa %2, %%ymm0\n\tvmovdqa %2, %%ymm15\n\t"
                     "xor %%eax, %%eax\n\tmovl (%%rax), %%eax\n\tstmxcsr %1"
                     : "=m"(mxcsr_before), "=m"(mxcsr_after) : "m"(marker)
                     : "rax", "xmm0", "xmm15", "memory");
    sysarch(129, &fs);
    sigaction(SIGSEGV, &old, NULL);
    FILE *out = fopen("/data/ps5proton/logs/signal-probe.log", "w");
    if (!out) return 1;
    fprintf(out, "mc=%zu fp=%zu mxcsr=%x/%x matches=%u\n",
            offsetof(ucontext_t, uc_mcontext), offsetof(ucontext_t, uc_mcontext.mc_fpstate),
            mxcsr_before, mxcsr_after, matches);
    for (unsigned i = 0; i < matches; ++i) fprintf(out, "marker at %u\n", offsets[i]);
    for (unsigned i = 0; i < 112; ++i) fprintf(out, "context+%u=%016llx\n", 256 + i * 8,
                                            (unsigned long long)words[i]);
    fclose(out);
    return 0;
}
