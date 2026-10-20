/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The real CPU in 32-bit mode, one instruction at a time, single-stepped (docs/IA32_DESIGN.md, M2).
 * Shared by tools/ia32_cpu_run.c and tools/ia32_cpu_decode_check.c; built only with -m32.
 *
 * cpu_worker_setup() maps the code page and data window and installs the signal handler;
 * cpu_run() runs one encoding: a trampoline loads the registers (pointing into the data window),
 * sets EFLAGS.TF and jumps; the first trap arrives before the instruction, the second after it,
 * and the handler records the state and returns to the harness. A fault is recorded by its signal
 * with EIP still at the instruction. Refused, never run: INT 80h, SYSENTER and SYSCALL (random
 * registers must not reach Linux), and SLDT/STR/SGDT/SIDT/SMSW (the kernel's UMIP emulation of them
 * does not return under single-step).
 */
#ifndef IA32_CPU_CORE_H
#define IA32_CPU_CORE_H
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <unistd.h>

#if !defined(__i386__)
#error "the CPU oracle is built with -m32"
#endif

enum {
    CODE = 0x20000000, CODE_BYTES = 0x10000, CODE_AT = CODE + 0x8000,
    DATA = 0x30000000, DATA_BYTES = 0x10000, DATA_REG = DATA + 0x4000, DATA_STACK = DATA + 0xc000,
    BATCH = 256, TF = 0x100, DF = 0x400, AC = 0x40000,
};

/* What the trampoline loads and the handler stores. */
static volatile uint32_t g_regs[8];     /* eax ecx edx ebx esp ebp esi edi */
static volatile uint32_t g_eflags, g_target;
static volatile uint32_t g_host_esp;
static volatile int g_phase, g_signal, g_si_code;
static volatile uint32_t g_eip_after, g_eflags_after, g_regs_after[8];
static volatile uint16_t g_cs, g_ss, g_ds, g_es, g_fs, g_gs;
/* Follow a REP string instruction to its end: a trace trap with EIP still on it is one iteration. */
static volatile int g_follow_rep;
static volatile uint32_t g_follow_count;   /* iterations followed in this run; capped */

void cpu_enter(void);
void cpu_exit(void);
/* cpu_enter: save the callee-saved registers and ESP, POPF the guest's EFLAGS (with TF) from the
 * harness's own stack, load every guest register from g_regs, and jump. The trampoline's own
 * instructions trap too; the handler lets them pass until EIP reaches the instruction. cpu_exit:
 * where the handler sends EIP back. Nothing of the guest's memory is touched. */
__asm__(".text\n"
        ".globl cpu_enter\n"
        "cpu_enter:\n"
        "  pushl %ebp\n  pushl %ebx\n  pushl %esi\n  pushl %edi\n"
        "  movl %esp, g_host_esp\n"
        "  pushl g_eflags\n"           /* EFLAGS (with TF) from the harness's stack, never the guest's */
        "  popfl\n"
        "  movl g_regs+16, %esp\n"
        "  movl g_regs+4, %ecx\n  movl g_regs+8, %edx\n  movl g_regs+12, %ebx\n"
        "  movl g_regs+20, %ebp\n  movl g_regs+24, %esi\n  movl g_regs+28, %edi\n"
        "  movl g_regs+0, %eax\n"
        "  jmp *g_target\n"
        ".globl cpu_exit\n"
        "cpu_exit:\n"
        "  movl g_host_esp, %esp\n"
        "  popl %edi\n  popl %esi\n  popl %ebx\n  popl %ebp\n"
        "  ret\n");

static void on_signal(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    greg_t *g = uc->uc_mcontext.gregs;
    if (sig == SIGTRAP && info->si_code == TRAP_TRACE && g_phase == 0) {
        if ((uint32_t)g[REG_EIP] == g_target) g_phase = 1;   /* the instruction runs next, still with TF */
        return;               /* before that: the trampoline's own instructions */
    }
    if (sig == SIGTRAP && info->si_code == TRAP_TRACE && g_phase == 1 && g_follow_rep &&
        (uint32_t)g[REG_EIP] == g_target) {
        if (++g_follow_count < (1u << 20)) return;   /* one REP iteration done; the next runs */
        sig = SIGALRM;        /* a jump to itself, or a REP that does not end: reported as a timeout */
    }
    g_signal = sig;
    g_si_code = info->si_code;
    g_eip_after = (uint32_t)g[REG_EIP];
    g_eflags_after = (uint32_t)g[REG_EFL] & ~(uint32_t)TF;
    static const int order[8] = { REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_UESP, REG_EBP, REG_ESI, REG_EDI };
    for (int i = 0; i < 8; i++) g_regs_after[i] = (uint32_t)g[order[i]];
    /* Back to the harness, whatever the instruction did to the segment registers, the stack or the
     * flags C code relies on: TF, DF (an STD would run the harness's memset backwards) and AC. */
    g[REG_EFL] &= ~(TF | DF | AC);
    g[REG_EIP] = (greg_t)(uintptr_t)cpu_exit;
    g[REG_CS] = g_cs; g[REG_SS] = g_ss; g[REG_DS] = g_ds; g[REG_ES] = g_es; g[REG_FS] = g_fs; g[REG_GS] = g_gs;
}

static int refused(const uint8_t *bytes, size_t n)
{
    size_t i = 0;
    while (i < n && strchr("\x66\x67\xf0\xf2\xf3\x26\x2e\x36\x3e\x64\x65", bytes[i])) i++;
    if (i + 1 < n && bytes[i] == 0xcd && bytes[i + 1] == 0x80) return 1;
    if (i + 1 < n && bytes[i] == 0x0f && (bytes[i + 1] == 0x34 || bytes[i + 1] == 0x05)) return 1;
    /* SLDT, STR, SGDT, SIDT, SMSW: with UMIP the kernel emulates them for a 32-bit process, and
     * under single-step that emulation never comes back (seen: one worker spun in the kernel). */
    if (i + 2 < n && bytes[i] == 0x0f && bytes[i + 1] == 0x00 && ((bytes[i + 2] >> 3) & 7) <= 1) return 1;
    if (i + 2 < n && bytes[i] == 0x0f && bytes[i + 1] == 0x01) {
        const unsigned reg = (bytes[i + 2] >> 3) & 7, mod = bytes[i + 2] >> 6;
        if (reg == 4 || (reg <= 1 && mod != 3)) return 1;
    }
    return 0;
}

/* The CPU's verdict on one encoding. */
typedef struct CpuResult {
    int refused;
    int signal, si_code;          /* SIGTRAP with TRAP_TRACE: the instruction completed */
    int32_t delta;                /* EIP after, minus the instruction's address */
    uint32_t eflags, regs[8];
} CpuResult;

static int cpu_completed(const CpuResult *r)
{
    return !r->refused && r->signal == SIGTRAP && r->si_code == TRAP_TRACE;
}

/* Runs one encoding from the given registers and EFLAGS over the data window as the caller left
 * it; the window afterwards is what the instruction made of it. */
static void cpu_run_state(const uint8_t *bytes, size_t n, const uint32_t regs[8], uint32_t eflags, CpuResult *r)
{
    memset(r, 0, sizeof(*r));
    if (refused(bytes, n)) {
        r->refused = 1;
        return;
    }
    memset((void *)CODE, 0xcc, CODE_BYTES);
    memcpy((void *)CODE_AT, bytes, n);
    for (int i = 0; i < 8; i++) g_regs[i] = regs[i];
    g_eflags = (eflags & ~(uint32_t)AC) | 0x202 | TF;
    g_target = CODE_AT;
    g_phase = 0;
    g_signal = 0;
    g_follow_count = 0;
    /* A watchdog: an instruction that clears TF (POPF) and lands in a loop ends as SIGALRM. */
    struct itimerval watchdog = { { 0, 0 }, { 0, 200000 } }, off = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_REAL, &watchdog, NULL);
    cpu_enter();
    setitimer(ITIMER_REAL, &off, NULL);
    r->signal = g_signal;
    r->si_code = g_si_code;
    r->delta = (int32_t)(g_eip_after - CODE_AT);
    r->eflags = g_eflags_after;
    for (int i = 0; i < 8; i++) r->regs[i] = g_regs_after[i];
}

/* Runs one encoding from fixed registers (pointing into the window) over a window of 0x5a. */
static inline void cpu_run(const uint8_t *bytes, size_t n, CpuResult *r)
{
    uint32_t regs[8];
    for (int i = 0; i < 8; i++) regs[i] = DATA_REG + 0x100 * (uint32_t)i;
    regs[4] = DATA_STACK;
    memset((void *)DATA, 0x5a, DATA_BYTES);
    cpu_run_state(bytes, n, regs, 0x202, r);
}

static void cpu_worker_setup(void)
{
    if (mmap((void *)CODE, CODE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED ||
        mmap((void *)DATA, DATA_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
        perror("mmap");
        _exit(3);
    }
    static uint8_t alternate[65536];
    stack_t ss = { .ss_sp = alternate, .ss_size = sizeof(alternate) };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGALRM);   /* the watchdog never lands inside a trap's handler */
    const int signals[] = { SIGTRAP, SIGILL, SIGSEGV, SIGBUS, SIGFPE, SIGALRM };
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) sigaction(signals[i], &sa, NULL);
    __asm__ volatile("movw %%cs, %0\n movw %%ss, %1\n movw %%ds, %2\n movw %%es, %3\n movw %%fs, %4\n movw %%gs, %5"
                     : "=m"(g_cs), "=m"(g_ss), "=m"(g_ds), "=m"(g_es), "=m"(g_fs), "=m"(g_gs));
}

#endif
