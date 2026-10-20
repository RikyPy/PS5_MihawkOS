/* SPDX-License-Identifier: GPL-3.0-or-later */
/* tools/host-tests.sh mmu4k (built -mgeneral-regs-only and without sanitizers, as the engine's contract needs)
 *
 * The 4 KiB MMU's engine (wine/ps5/pw_wine_mmu4k.h) against the oracle the console lacks: this host's real 4 KiB
 * pages. A memfd stands in for direct memory; the program's view P maps it, a second mapping (an alias window of
 * pw_wine_alias, through the test's ops) maps it again. The same generated program runs
 *   native: P protected per 4 KiB page as Windows would hold it; a store into a read-only page faults, and the
 *           tracker (the program's handler) records the access violation and opens that page;
 *   engine: each 16 KiB "host page" of P protected as patch 1122 holds it (read-only when its pages disagree on
 *           write); a store into a plain page of such a page faults and the glue (the Linux twin of patch 1134's
 *           signal_x86_64.c) starts a run; any other fault is the access violation, recorded the same way.
 * The access violations (address, RIP, registers, the bytes around the address at that moment), the program's
 * exceptions, its final registers, ymm0-15, MXCSR and memory must be identical. Cases (docs/MMU4K.md section 11):
 * M1 random streams near page edges, M2 straddle, M3 partial and overlapping REP, M4 #DE and #UD in a slot, M5 a
 * re-protect at a hand-over, M6 an alias forgotten at a hand-over, M7 zero progress, M8 budgets and hand-overs (one
 * lock per run), M9 encodings,
 * M10 accesses the operand does not bound and code the CPU refuses (BTS/BTR/BTC with a register offset, GS after DS,
 * a CALL into a page that may not be executed; non-canonical branches are in M1). One mismatch prints one line: the case, the field, expected and actual. */
#define _GNU_SOURCE
#include "../wine/ps5/pw_wine_alias.h"
#include "../wine/ps5/pw_wine_mmu4k.h"
#include "../include/prospero_win.h"
#include <asm/prctl.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define P ((uintptr_t)0x200000000000)
#define CODE (P + 0x40000000)
#define ALIASES (P + 0x80000000)
#define BACKDOOR (P + 0xc0000000)
enum { PAGE = 4096, HOST = 16384, HOSTS = 8, PAGES = HOSTS * 4, REGION = HOSTS * HOST, CODE_BYTES = 1 << 20,
       ALIAS_SLOTS = 8, EVENTS = 256 };
#define WINDOW ((uintptr_t)PW_WINE_ALIAS_WINDOW)
enum { NATIVE, ENGINE };
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

/* ---------------------------------------------------------------- the program and its trampoline */
uint64_t prog_in[16], prog_out[16], prog_in_rflags, prog_out_rflags, prog_entry, prog_host_rsp;
uint32_t prog_in_mxcsr, prog_out_mxcsr, prog_host_mxcsr;
uint8_t prog_in_ymm[16 * 32] __attribute__((aligned(32))), prog_out_ymm[16 * 32] __attribute__((aligned(32)));
void prog_run(void);
extern const char prog_exit[];
/* prog_run: the program's ymm0-15, MXCSR, RFLAGS and general registers (RSP last) from prog_in*, then prog_entry.
 * The program ends with a jump to prog_exit, which saves them all into prog_out* and returns to prog_run's caller. */
__asm__(
    "    .text\n"
    "    .globl prog_run\n"
    "prog_run:\n"
    "    push %rbp\n push %rbx\n push %r12\n push %r13\n push %r14\n push %r15\n"
    "    mov %rsp, prog_host_rsp(%rip)\n"
    "    stmxcsr prog_host_mxcsr(%rip)\n"
    "    vmovdqu prog_in_ymm+0(%rip), %ymm0\n vmovdqu prog_in_ymm+32(%rip), %ymm1\n"
    "    vmovdqu prog_in_ymm+64(%rip), %ymm2\n vmovdqu prog_in_ymm+96(%rip), %ymm3\n"
    "    vmovdqu prog_in_ymm+128(%rip), %ymm4\n vmovdqu prog_in_ymm+160(%rip), %ymm5\n"
    "    vmovdqu prog_in_ymm+192(%rip), %ymm6\n vmovdqu prog_in_ymm+224(%rip), %ymm7\n"
    "    vmovdqu prog_in_ymm+256(%rip), %ymm8\n vmovdqu prog_in_ymm+288(%rip), %ymm9\n"
    "    vmovdqu prog_in_ymm+320(%rip), %ymm10\n vmovdqu prog_in_ymm+352(%rip), %ymm11\n"
    "    vmovdqu prog_in_ymm+384(%rip), %ymm12\n vmovdqu prog_in_ymm+416(%rip), %ymm13\n"
    "    vmovdqu prog_in_ymm+448(%rip), %ymm14\n vmovdqu prog_in_ymm+480(%rip), %ymm15\n"
    "    ldmxcsr prog_in_mxcsr(%rip)\n"
    "    pushq prog_in_rflags(%rip)\n popfq\n"
    "    mov prog_in+0(%rip), %rax\n mov prog_in+8(%rip), %rcx\n mov prog_in+16(%rip), %rdx\n"
    "    mov prog_in+24(%rip), %rbx\n mov prog_in+40(%rip), %rbp\n mov prog_in+48(%rip), %rsi\n"
    "    mov prog_in+56(%rip), %rdi\n mov prog_in+64(%rip), %r8\n mov prog_in+72(%rip), %r9\n"
    "    mov prog_in+80(%rip), %r10\n mov prog_in+88(%rip), %r11\n mov prog_in+96(%rip), %r12\n"
    "    mov prog_in+104(%rip), %r13\n mov prog_in+112(%rip), %r14\n mov prog_in+120(%rip), %r15\n"
    "    mov prog_in+32(%rip), %rsp\n"
    "    jmp *prog_entry(%rip)\n"
    "    .globl prog_exit\n"
    "prog_exit:\n"
    "    mov %rax, prog_out+0(%rip)\n mov %rcx, prog_out+8(%rip)\n mov %rdx, prog_out+16(%rip)\n"
    "    mov %rbx, prog_out+24(%rip)\n mov %rsp, prog_out+32(%rip)\n mov %rbp, prog_out+40(%rip)\n"
    "    mov %rsi, prog_out+48(%rip)\n mov %rdi, prog_out+56(%rip)\n mov %r8, prog_out+64(%rip)\n"
    "    mov %r9, prog_out+72(%rip)\n mov %r10, prog_out+80(%rip)\n mov %r11, prog_out+88(%rip)\n"
    "    mov %r12, prog_out+96(%rip)\n mov %r13, prog_out+104(%rip)\n mov %r14, prog_out+112(%rip)\n"
    "    mov %r15, prog_out+120(%rip)\n"
    "    mov prog_host_rsp(%rip), %rsp\n"
    "    pushfq\n popq prog_out_rflags(%rip)\n"
    "    vmovdqu %ymm0, prog_out_ymm+0(%rip)\n vmovdqu %ymm1, prog_out_ymm+32(%rip)\n"
    "    vmovdqu %ymm2, prog_out_ymm+64(%rip)\n vmovdqu %ymm3, prog_out_ymm+96(%rip)\n"
    "    vmovdqu %ymm4, prog_out_ymm+128(%rip)\n vmovdqu %ymm5, prog_out_ymm+160(%rip)\n"
    "    vmovdqu %ymm6, prog_out_ymm+192(%rip)\n vmovdqu %ymm7, prog_out_ymm+224(%rip)\n"
    "    vmovdqu %ymm8, prog_out_ymm+256(%rip)\n vmovdqu %ymm9, prog_out_ymm+288(%rip)\n"
    "    vmovdqu %ymm10, prog_out_ymm+320(%rip)\n vmovdqu %ymm11, prog_out_ymm+352(%rip)\n"
    "    vmovdqu %ymm12, prog_out_ymm+384(%rip)\n vmovdqu %ymm13, prog_out_ymm+416(%rip)\n"
    "    vmovdqu %ymm14, prog_out_ymm+448(%rip)\n vmovdqu %ymm15, prog_out_ymm+480(%rip)\n"
    "    stmxcsr prog_out_mxcsr(%rip)\n"
    "    ldmxcsr prog_host_mxcsr(%rip)\n"
    "    vzeroupper\n"
    "    pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbx\n pop %rbp\n"
    "    ret\n");

/* ---------------------------------------------------------------- Windows' side: protections and the tracker */
static int memory = -1;          /* the "direct memory" */
static uint8_t *slots;
static char vprot[PAGES];        /* each 4 KiB page: 'w' read-write (plain), 'r' read-only, 'n' no access */
static volatile int mode;
static int unix_prot(char v) { return v == 'w' ? PROT_READ | PROT_WRITE : v == 'r' ? PROT_READ : PROT_NONE; }
/* Patch 1122's rule: written only when every page is; readable when any is. */
static int host_prot(unsigned h)
{
    int write = 1, read = 0;
    for (unsigned i = 0; i < 4; i++) {
        write &= vprot[h * 4 + i] == 'w';
        read |= vprot[h * 4 + i] != 'n';
    }
    return write ? PROT_READ | PROT_WRITE : read ? PROT_READ : PROT_NONE;
}
static void protect_page(unsigned q)
{
    if (mode == NATIVE) assert(!mprotect((void *)(P + q * PAGE), PAGE, unix_prot(vprot[q])));
    else assert(!mprotect((void *)(P + q / 4 * HOST), HOST, host_prot(q / 4)));
}
static void protect_all(void)
{
    for (unsigned q = 0; q < PAGES; q++) protect_page(q);
}

typedef struct Event { int sig; uintptr_t address, rip; uint64_t gpr[16]; uint8_t around[32]; } Event;
static Event events[EVENTS];
static volatile unsigned event_count;
static struct { uintptr_t rip; unsigned length; } skips[64];
static unsigned skip_count;

static void fatal(const char *what, uintptr_t a, uintptr_t b)
{
    fprintf(stderr, "mmu4k: %s %#lx %#lx\n", what, (unsigned long)a, (unsigned long)b);
    abort();
}

static void record(int sig, uintptr_t address, const PwWineMmu4kRegs *r)
{
    if (event_count >= EVENTS) fatal("too many events", event_count, 0);
    Event *e = &events[event_count++];
    e->sig = sig;
    e->address = address;
    e->rip = r->rip;
    memcpy(e->gpr, r->gpr, sizeof(e->gpr));
    memset(e->around, 0, sizeof(e->around));
    if (address >= P + 16 && address + 16 <= P + REGION)
        memcpy(e->around, (const void *)(BACKDOOR + (address - P) - 16), sizeof(e->around));
}
/* The program's handler: an access violation is recorded, and its page opened (a write tracker's answer). */
static void violation(uintptr_t address, const PwWineMmu4kRegs *r)
{
    if (address < P || address >= P + REGION) fatal("access violation outside the region", address, r->rip);
    record(SIGSEGV, address, r);
    vprot[(address - P) / PAGE] = 'w';
    protect_page((unsigned)((address - P) / PAGE));
}

/* ---------------------------------------------------------------- the glue (patch 1134's twin) */
static PwWineMmu4kRun *volatile current;
static sigset_t run_mask;
static volatile int step_host = -1;
static volatile unsigned recovers, steps_taken;

static const int gregs_of[16] = { REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                  REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15 };
static void regs_get(const ucontext_t *uc, PwWineMmu4kRegs *r)
{
    for (unsigned i = 0; i < 16; i++) r->gpr[i] = (uint64_t)uc->uc_mcontext.gregs[gregs_of[i]];
    r->rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    r->rflags = (uint64_t)uc->uc_mcontext.gregs[REG_EFL];
}
static void regs_set(ucontext_t *uc, const PwWineMmu4kRegs *r)
{
    for (unsigned i = 0; i < 16; i++) uc->uc_mcontext.gregs[gregs_of[i]] = (greg_t)r->gpr[i];
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)r->rip;
    uc->uc_mcontext.gregs[REG_EFL] = (greg_t)r->rflags;
}
/* 1122's step: the host page opened, one instruction, the trap closes it. */
static void step_open(uintptr_t address, ucontext_t *uc)
{
    const unsigned h = (unsigned)((address - P) / HOST);
    steps_taken++;
    assert(!mprotect((void *)(P + h * HOST), HOST, PROT_READ | PROT_WRITE));
    step_host = (int)h;
    uc->uc_mcontext.gregs[REG_EFL] |= 0x100;
}

static void handler(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    PwWineMmu4kRegs r;
    const uintptr_t fault = (uintptr_t)info->si_addr;
    regs_get(uc, &r);
    if (current) {
        uintptr_t address = 0;
        const int action = pw_wine_mmu4k_signal(current, sig, fault, &r, &address);
        if (action == PW_WINE_MMU4K_NOT_OURS) fatal("fault in the engine at rip, address", r.rip, fault);
        regs_set(uc, &r);
        if (action == PW_WINE_MMU4K_RECOVER) {
            recovers++;
            return;
        }
        current = NULL;
        uc->uc_sigmask = run_mask;
        if (action == PW_WINE_MMU4K_STEP) step_open(address, uc);
        else if (action == PW_WINE_MMU4K_FAULT) violation(address, &r);
        return;
    }
    if (sig == SIGTRAP && step_host >= 0) {
        assert(!mprotect((void *)(P + (unsigned)step_host * HOST), HOST, host_prot((unsigned)step_host)));
        step_host = -1;
        uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;
        return;
    }
    if (sig == SIGFPE || sig == SIGILL || (sig == SIGSEGV && info->si_code == SI_KERNEL)) {   /* #DE, #UD, #GP */
        for (unsigned i = 0; i < skip_count; i++)
            if (skips[i].rip == r.rip) {
                record(sig, 0, &r);
                uc->uc_mcontext.gregs[REG_RIP] += skips[i].length;
                return;
            }
    }
    if (sig == SIGSEGV) {
        const int write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
        if (uc->uc_mcontext.gregs[REG_ERR] & 0x10) {   /* an instruction fetch: recorded, and the program ends */
            record(sig, fault, &r);
            uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)prog_exit;
            return;
        }
        if (mode == ENGINE && write && fault >= P && fault < P + REGION && vprot[(fault - P) / PAGE] == 'w' &&
            !(host_prot((unsigned)((fault - P) / HOST)) & PROT_WRITE)) {
            const int stack = pw_wine_mmu4k_stack_take();
            if (stack < 0) fatal("no run stack", fault, r.rip);
            current = pw_wine_mmu4k_enter(stack, &r, fault & ~(uintptr_t)(HOST - 1), fault);
            if (!current) fatal("enter refused", fault, r.rip);
            regs_set(uc, &r);
            run_mask = uc->uc_sigmask;
            sigaddset(&uc->uc_sigmask, SIGALRM);   /* P4's asynchronous signals wait for the run's end */
            sigaddset(&uc->uc_sigmask, SIGUSR1);
            return;
        }
        violation(fault, &r);
        return;
    }
    fatal("unexpected signal at rip", (uintptr_t)sig, r.rip);
}

/* ---------------------------------------------------------------- Wine's ops for the engine and the aliases */
static unsigned locks, unlocks, classifies, take_next, alias_gives;
static uint64_t waiting;   /* the threads the engine sees waiting on the mutex (PwWineMmu4kOps.waiters) */
static int broken_alias, forget_on_host = -1, alias_used[ALIAS_SLOTS];
static uintptr_t last_given, last_alias;
static void (*on_unlock)(unsigned count);

/* What a callee may do to the program's vector state, which pw_wine_mmu4k_call must undo (P2). */
static void clobber_vectors(void)
{
    static const uint32_t odd = 0x7f80;
    __asm__ volatile("vpcmpeqd %%ymm0, %%ymm0, %%ymm0\n vpcmpeqd %%ymm1, %%ymm1, %%ymm1\n"
                     "vpcmpeqd %%ymm7, %%ymm7, %%ymm7\n vpcmpeqd %%ymm8, %%ymm8, %%ymm8\n"
                     "vpcmpeqd %%ymm15, %%ymm15, %%ymm15\n fld1\n ldmxcsr %0\n" : : "m"(odd) : "memory");
}
static int test_classify(void *context, uintptr_t host_page, PwWineMmu4kPage *page)
{
    (void)context;
    classifies++;
    clobber_vectors();
    if (host_page >= CODE && host_page < CODE + CODE_BYTES) {   /* the generated code: RWX, the program's only code */
        page->kernel_write = 1;
        page->plain = page->exec = 0xf;
        page->alias_delta = 0;
        page->generation = pw_wine_alias_generation();
        return 0;
    }
    if (host_page < P || host_page >= P + REGION) return -1;
    const unsigned h = (unsigned)((host_page - P) / HOST);
    if ((int)h == forget_on_host) {   /* this get gives back a window the run holds (M6) */
        forget_on_host = -1;
        pw_wine_alias_forget(0, REGION);
    }
    page->kernel_write = (host_prot(h) & PROT_WRITE) != 0;
    page->plain = page->exec = 0;   /* the region is data: never executable */
    for (unsigned i = 0; i < 4; i++) page->plain |= (uint8_t)((vprot[h * 4 + i] == 'w') << i);
    page->alias_delta = 0;
    if (!page->kernel_write && page->plain) {
        /* broken: the last slot, never mapped, stands for an alias that faults (M7) */
        const uintptr_t alias = broken_alias ? ALIASES + (ALIAS_SLOTS - 1) * WINDOW + (host_page - P)
                                             : pw_wine_alias_get(host_page);
        if (alias) page->alias_delta = (intptr_t)(alias - host_page);
        if (alias && !broken_alias) last_alias = alias - (host_page - P);
    }
    page->generation = pw_wine_alias_generation();   /* after the get, which may have given a window back */
    return 0;
}
static void test_lock(void *context) { (void)context; locks++; clobber_vectors(); }
static void test_unlock(void *context)
{
    (void)context;
    unlocks++;
    clobber_vectors();
    if (on_unlock) on_unlock(unlocks);
}
static const PwWineMmu4kOps engine_ops = { NULL, test_classify, test_lock, test_unlock, &waiting };

static int alias_backing(void *c, uintptr_t at, int64_t *dmem, uintptr_t *low, uintptr_t *high)
{
    (void)c;
    if (at < P || at >= P + REGION) return 0;
    *dmem = (int64_t)(at - P);
    *low = P;
    *high = P + REGION;
    return 1;
}
/* A window slot, never the one given back last, so an old alias address is never reused at once (M6). */
static uintptr_t alias_take(void *c, size_t bytes)
{
    (void)c;
    assert(bytes <= WINDOW);
    for (unsigned k = 0; k < ALIAS_SLOTS - 1; k++) {
        const unsigned slot = take_next++ % (ALIAS_SLOTS - 1);
        const uintptr_t at = ALIASES + slot * WINDOW;
        if (!alias_used[slot] && at != last_given) {
            alias_used[slot] = 1;
            return at;
        }
    }
    return 0;
}
static void alias_give(void *c, uintptr_t at, size_t bytes)
{
    (void)c;
    (void)bytes;
    assert(mmap((void *)at, WINDOW, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0) ==
           (void *)at);
    alias_used[(at - ALIASES) / WINDOW] = 0;
    last_given = at;
    alias_gives++;
}
static int alias_map(void *c, uintptr_t at, size_t bytes, int64_t dmem)
{
    (void)c;
    return mmap((void *)at, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, memory, dmem) != (void *)at;
}
static const PwWineAliasOps alias_ops = { NULL, alias_backing, alias_take, alias_give, alias_map };

/* ---------------------------------------------------------------- code generation */
static uint8_t *pc;
static uint64_t seed = 0x9e3779b97f4a7c15ull;
static uint64_t random64(void)
{
    seed ^= seed >> 12;
    seed ^= seed << 25;
    seed ^= seed >> 27;
    return seed * 0x2545f4914f6cdd1dull;
}
static unsigned below(unsigned n) { return (unsigned)(random64() % n); }
static void emit(unsigned v) { *pc++ = (uint8_t)v; }
static void emit32(uint32_t v) { for (unsigned i = 0; i < 4; i++) emit(v >> 8 * i); }
static void emit64(uint64_t v) { emit32((uint32_t)v); emit32((uint32_t)(v >> 32)); }
static void bytes(const char *hex)   /* "48 89 e0" */
{
    for (char *end; *hex; hex = end) {
        const unsigned long v = strtoul(hex, &end, 16);
        if (end == hex) break;
        emit((unsigned)v);
    }
}
/* A legacy instruction on [base + disp32]: prefixes, a REX when it says anything (or forced), the opcode, ModRM
 * mod 10 (with a SIB for an r12 base; r13 needs no more), disp32; any immediate follows. */
static void legacy(const char *prefix, unsigned w, const char *opcode, unsigned reg, unsigned base, int32_t disp,
                   int force_rex)
{
    bytes(prefix);
    const unsigned rex = 0x40 | w << 3 | (reg >> 3) << 2 | base >> 3;
    if (rex != 0x40 || force_rex) emit(rex);
    bytes(opcode);
    emit(0x80 | (reg & 7) << 3 | (base & 7));
    if ((base & 7) == 4) emit(0x24);
    emit32((uint32_t)disp);
}
/* Register to register: REX when needed, the opcode, ModRM mod 11. */
static void legacy_rr(const char *prefix, unsigned w, const char *opcode, unsigned reg, unsigned rm)
{
    bytes(prefix);
    const unsigned rex = 0x40 | w << 3 | (reg >> 3) << 2 | rm >> 3;
    if (rex != 0x40) emit(rex);
    bytes(opcode);
    emit(0xc0 | (reg & 7) << 3 | (rm & 7));
}
/* VEX3 on [base + disp32] (vvvv unused), or register to register with vvvv. */
static void vex(unsigned map, unsigned pp, unsigned l, unsigned opcode, unsigned reg, unsigned base, int32_t disp)
{
    emit(0xc4);
    emit((~reg >> 3 & 1) << 7 | 1 << 6 | (~base >> 3 & 1) << 5 | map);
    emit(15 << 3 | l << 2 | pp);
    emit(opcode);
    emit(0x80 | (reg & 7) << 3 | (base & 7));
    if ((base & 7) == 4) emit(0x24);
    emit32((uint32_t)disp);
}
static void vex_rr(unsigned map, unsigned pp, unsigned l, unsigned opcode, unsigned reg, unsigned vvvv, unsigned rm)
{
    emit(0xc4);
    emit((~reg >> 3 & 1) << 7 | 1 << 6 | (~rm >> 3 & 1) << 5 | map);
    emit((~vvvv & 15) << 3 | l << 2 | pp);
    emit(opcode);
    emit(0xc0 | (reg & 7) << 3 | (rm & 7));
}
static void mov_imm64(unsigned reg, uint64_t v) { emit(0x48 | reg >> 3); emit(0xb8 | (reg & 7)); emit64(v); }
static void begin(void)
{
    pc = (uint8_t *)CODE;
    skip_count = 0;
}
static void finish(void)
{
    bytes("ff 25 00 00 00 00");   /* jmp [rip]: prog_exit */
    emit64((uint64_t)(uintptr_t)prog_exit);
    assert((uintptr_t)pc < CODE + CODE_BYTES);
}
static void skip_here(unsigned length)
{
    assert(skip_count < sizeof(skips) / sizeof(skips[0]));
    skips[skip_count].rip = (uintptr_t)pc;
    skips[skip_count++].length = length;
}

/* The registers M1's programs keep: three bases into the region (rbx, r12 with its SIB, r13 with its displacement)
 * and the stack in page 11, a plain page of the mixed host page 2. */
static const unsigned bases[3] = { RBX, R12, R13 };
static const uint64_t base_at[3] = { P, P + 0x6000, P + 0xc000 };
static const unsigned value_regs[] = { RAX, RCX, RDX, RSI, RDI, R8, R9, R10, R11, R14, R15 };
enum { STACK_PAGE = 11, STACK_TOP = 0xbf00, EDGE_PAGES = 24 };
static unsigned value(int no_rcx)
{
    unsigned r;
    do r = value_regs[below(sizeof(value_regs) / sizeof(value_regs[0]))];
    while (no_rcx && r == RCX);
    return r;
}
static unsigned page_index(uint64_t at) { return (unsigned)((at - P) / PAGE); }
/* A random [at, +size) within 40 bytes of a 4 KiB edge in the first 24 pages, off the stack's page. */
static uint64_t near_edge(unsigned size, unsigned *base, int32_t *disp)
{
    uint64_t at;
    do at = P + (1 + below(EDGE_PAGES - 1)) * (uint64_t)PAGE + below(80) - 40;
    while (at + size > P + EDGE_PAGES * PAGE || page_index(at) == STACK_PAGE || page_index(at + size - 1) == STACK_PAGE);
    const unsigned b = below(3);
    *base = bases[b];
    *disp = (int32_t)(at - base_at[b]);
    return at;
}
/* One memory access near an edge: stores of 1-32 bytes, read-modify-writes, LOCK forms, SSE/AVX stores, loads. */
static void gen_access(int no_rcx)
{
    static const unsigned sizes[20] = { 8, 4, 2, 1, 8, 8, 8, 4, 8, 4, 8, 8, 16, 16, 32, 16, 8, 8, 16, 4 };
    const unsigned kind = below(20), v = value(no_rcx), x = below(16);
    unsigned base;
    int32_t disp;
    const uint64_t at = near_edge(sizes[kind], &base, &disp);
    switch (kind) {
    case 0: legacy("", 1, "89", v, base, disp, 0); break;               /* mov [m], r64 */
    case 1: legacy("", 0, "89", v, base, disp, 0); break;               /* mov [m], r32 */
    case 2: legacy("66", 0, "89", v, base, disp, 0); break;             /* mov [m], r16 */
    case 3: legacy("", 0, "88", v, base, disp, v >= 4); break;          /* mov [m], r8 (sil, dil with a REX) */
    case 4: legacy("", 1, "c7", 0, base, disp, 0); emit32((uint32_t)random64()); break;
    case 5: legacy("", 1, "01", v, base, disp, 0); break;               /* add [m], r64 */
    case 6: legacy("", 1, "83", 0, base, disp, 0); emit(below(256)); break;
    case 7: legacy("", 0, "ff", 0, base, disp, 0); break;               /* inc dword [m] */
    case 8: legacy("", 1, "d1", 4, base, disp, 0); break;               /* shl qword [m], 1 */
    case 9: legacy("f0", 0, "0f c1", v, base, disp, 0); break;          /* lock xadd [m], r32 */
    case 10: legacy("f0", 1, "0f b1", v, base, disp, 0); break;         /* lock cmpxchg [m], r64 */
    case 11: legacy("", 1, "87", v, base, disp, 0); break;              /* xchg [m], r64 */
    case 12: legacy("f3", 0, "0f 7f", x, base, disp, 0); break;         /* movdqu [m], xmm */
    case 13: legacy("", 0, "0f 11", x, base, disp, 0); break;           /* movups [m], xmm */
    case 14: vex(1, 2, 1, 0x7f, x, base, disp); break;                  /* vmovdqu [m], ymm */
    case 15: vex(1, 0, 0, 0x11, x, base, disp); break;                  /* vmovups [m], xmm */
    case 16: legacy("", 1, "8b", v, base, disp, 0); break;              /* mov r64, [m] */
    case 17: legacy("", 1, "03", v, base, disp, 0); break;              /* add r64, [m] */
    case 18: legacy("f3", 0, "0f 6f", x, base, disp, 0); break;         /* movdqu xmm, [m] */
    default:                                                            /* mov [rip + d], r32 */
        if (v >= 8) emit(0x44);
        emit(0x89);
        emit(0x05 | (v & 7) << 3);
        emit32((uint32_t)(at - ((uintptr_t)pc + 4)));
        break;
    }
}
/* Control flow and the stack's other forms: Jcc on random flags (all 16 conditions, rel8 and rel32), LOOP, LOOPE,
 * LOOPNE, JRCXZ and JECXZ, RET imm16, CALL and JMP through a register and through memory, PUSH imm and memory,
 * 16-bit PUSH and POP, and JMP, CALL and RET to a non-canonical target (#GP at the branch, nothing pushed or popped). */
static void gen_flow(unsigned v, unsigned w)
{
    unsigned base;
    int32_t disp;
    switch (below(8)) {
    case 0: {
        const unsigned cc = below(16);
        legacy_rr("", 1, "39", w, v);                                   /* cmp v, w */
        if (below(2)) {
            emit(0x70 | cc);
            emit(4);
        } else {
            emit(0x0f);
            emit(0x80 | cc);
            emit32(4);
        }
        bytes("48 83 c0 01");                                           /* add rax, 1: skipped when taken */
        break;
    }
    case 1: emit(0xb9); emit32(1 + below(5)); bytes("48 83 c0 03 e2 fa"); break;   /* L: add rax, 3; loop L */
    case 2:                                                             /* L: add rax, 1; test al, 3; loope/loopne L */
        emit(0xb9);
        emit32(1 + below(6));
        bytes("48 83 c0 01 a8 03");
        emit(below(2) ? 0xe1 : 0xe0);
        emit(0xf8);
        break;
    case 3:                                                             /* jrcxz or jecxz over add rax, 1 */
        mov_imm64(RCX, below(3) == 0 ? 0 : below(2) ? 1 : (uint64_t)1 << 32);   /* ECX 0 with RCX not */
        if (below(2)) emit(0x67);
        bytes("e3 04 48 83 c0 01");
        break;
    case 4:
        bytes("50 e8 02 00 00 00 eb 03 c2 08 00");                      /* push rax; call sub; jmp; sub: ret 8 */
        bytes("4c 8d 1d 05 00 00 00 41 ff d3 eb 01 c3");                /* lea r11, [sub]; call r11; jmp; sub: ret */
        bytes("41 53 ff 14 24 48 83 c4 08");                            /* push r11; call [rsp]; add rsp, 8 */
        bytes("4c 8d 1d 03 00 00 00 41 ff e3");                         /* lea r11, [next]; jmp r11 */
        break;
    case 5:
        emit(0x68); emit32((uint32_t)random64());                       /* push imm32 */
        if (v >= 8) emit(0x41);
        emit(0x58 | (v & 7));
        emit(0x6a); emit(below(256));                                   /* push imm8 */
        if (w >= 8) emit(0x41);
        emit(0x58 | (w & 7));
        near_edge(8, &base, &disp);
        legacy("", 0, "ff", 6, base, disp, 0);                          /* push qword [m] */
        if (v >= 8) emit(0x41);
        emit(0x58 | (v & 7));
        break;
    case 6:
        mov_imm64(RCX, 0x8000000000001000ull);
        skip_here(2);
        bytes("ff e1");                                                 /* jmp rcx */
        skip_here(2);
        bytes("ff d1");                                                 /* call rcx */
        bytes("51");                                                    /* push rcx */
        skip_here(1);
        bytes("c3 59");                                                 /* ret; pop rcx */
        break;
    default:
        bytes("66 50 66 59");                                           /* push ax; pop cx */
        break;
    }
}

/* One M1 element: an access, arithmetic, the stack (PUSH/POP, CALL/RET, a frame with LEAVE, RSP as an operand), a
 * REP string, a Jcc loop, control flow. exceptions: also #DE and #UD (M4). */
static void gen_element(int exceptions)
{
    const unsigned v = value(0), w = value(0);
    uint8_t *patch;
    switch (below(exceptions ? 14 : 12)) {
    case 11: gen_flow(v, w); break;
    case 0: case 1: case 2: case 3: gen_access(0); break;
    case 4:
        legacy_rr("", 1, "01", v, w);                                   /* add r64, r64 */
        legacy_rr("", 1, "0f af", v, w);                                /* imul r64, r64 */
        legacy_rr("", 0, "0f 58", below(16), below(16));                /* addps xmm, xmm */
        vex_rr(1, 1, 1, 0xfe, below(16), below(16), below(16));         /* vpaddd ymm, ymm, ymm */
        break;
    case 5:                                                             /* push; access; pop */
        if (v >= 8) emit(0x41);
        emit(0x50 | (v & 7));
        gen_access(0);
        if (w >= 8) emit(0x41);
        emit(0x58 | (w & 7));
        break;
    case 6:                                                             /* jmp over; sub: access; ret; call sub */
        emit(0xeb);
        patch = pc++;
        {
            uint8_t *sub = pc;
            gen_access(0);
            emit(0xc3);
            *patch = (uint8_t)(pc - patch - 1);
            emit(0xe8);
            emit32((uint32_t)(sub - (pc + 4)));
        }
        break;
    case 7:                                                             /* a frame */
        bytes("55 48 89 e5 48 83 ec 20");                               /* push rbp; mov rbp, rsp; sub rsp, 32 */
        emit(0x48 | (v >> 3) << 2); emit(0x89); emit(0x45 | (v & 7) << 3); emit(0xf8);         /* mov [rbp-8], v */
        emit(0x48 | (w >> 3) << 2); emit(0x89); emit(0x44 | (w & 7) << 3); emit(0x24); emit(8); /* mov [rsp+8], w */
        bytes("48 89 e0 48 89 c4 c9");                                  /* mov rax, rsp; mov rsp, rax; leave */
        break;
    case 8: {                                                           /* rep movsb */
        unsigned base;
        int32_t disp;
        const unsigned n = 1 + below(300);
        const uint64_t dst = near_edge(n, &base, &disp), src = near_edge(n, &base, &disp);
        mov_imm64(RDI, dst);
        mov_imm64(RSI, src);
        emit(0xb9); emit32(n);
        bytes("f3 a4");
        break;
    }
    case 9: {                                                           /* rep stosq */
        unsigned base;
        int32_t disp;
        const unsigned n = 1 + below(40);
        mov_imm64(RDI, near_edge(n * 8, &base, &disp));
        emit(0xb9); emit32(n);
        bytes("f3 48 ab");
        break;
    }
    case 10: {                                                          /* mov ecx, n; L: access; dec ecx; jnz L */
        emit(0xb9); emit32(2 + below(20));
        uint8_t *loop = pc;
        gen_access(1);
        bytes("ff c9 75");
        emit((unsigned)(loop - (pc + 1)) & 0xff);
        bytes("48 83 ec 08 48 89 e0 48 83 c4 08");                      /* sub rsp, 8; mov rax, rsp; add rsp, 8 */
        break;
    }
    case 12:                                                            /* xor ecx, ecx; div rcx: #DE */
        bytes("31 c9");
        skip_here(3);
        bytes("48 f7 f1");
        break;
    default:                                                            /* lock add eax, ebx: #UD */
        skip_here(3);
        bytes("f0 01 d8");
        break;
    }
}

/* ---------------------------------------------------------------- running and comparing */
typedef struct Result {
    Event events[EVENTS];
    unsigned count;
    uint64_t gpr[16], rflags;
    uint32_t mxcsr;
    uint8_t ymm[16 * 32];
    uint8_t memory[REGION];
    PwWineMmu4kStats stats;
} Result;
static Result native_result, engine_result;
static uint8_t initial[REGION];
static char layout[PAGES + 1];

static void stats_delta(PwWineMmu4kStats *d, const PwWineMmu4kStats *a, const PwWineMmu4kStats *b)
{
    d->runs = b->runs - a->runs;
    d->insns = b->insns - a->insns;
    for (unsigned i = 0; i < PW_WINE_MMU4K_EXITS; i++) d->exits[i] = b->exits[i] - a->exits[i];
    d->stepfall = b->stepfall - a->stepfall;
    d->stack_damage = b->stack_damage - a->stack_damage;
    memcpy(d->top, b->top, sizeof(d->top));
    memcpy(d->top_count, b->top_count, sizeof(d->top_count));
}

static void run_program(int which, Result *out)
{
    PwWineMmu4kStats before, after;
    mode = which;
    memcpy((void *)BACKDOOR, initial, REGION);
    memcpy(vprot, layout, PAGES);
    protect_all();
    event_count = 0;
    prog_entry = CODE;
    pw_wine_mmu4k_stats(&before);
    prog_run();
    pw_wine_mmu4k_stats(&after);
    assert(!current && step_host < 0);
    out->count = event_count;
    memcpy(out->events, events, sizeof(events));
    memcpy(out->gpr, prog_out, sizeof(out->gpr));
    out->rflags = prog_out_rflags;
    out->mxcsr = prog_out_mxcsr;
    memcpy(out->ymm, prog_out_ymm, sizeof(out->ymm));
    memcpy(out->memory, (const void *)BACKDOOR, REGION);
    stats_delta(&out->stats, &before, &after);
}

static void differ(const char *what, const char *field, unsigned index, uint64_t expected, uint64_t actual)
{
    printf("MMU4K-MISMATCH case=%s field=%s index=%u expected=%#llx actual=%#llx\n", what, field, index,
           (unsigned long long)expected, (unsigned long long)actual);
    for (unsigned i = 0; i < native_result.count; i++)
        printf("  native event %u: sig %d address %#lx rip %#lx\n", i, native_result.events[i].sig,
               (unsigned long)native_result.events[i].address, (unsigned long)native_result.events[i].rip);
    for (unsigned i = 0; i < engine_result.count; i++)
        printf("  engine event %u: sig %d address %#lx rip %#lx\n", i, engine_result.events[i].sig,
               (unsigned long)engine_result.events[i].address, (unsigned long)engine_result.events[i].rip);
    exit(1);
}
static void compare(const char *what, const Result *a, const Result *b)
{
    if (a->count != b->count) differ(what, "events", 0, a->count, b->count);
    for (unsigned i = 0; i < a->count; i++) {
        const Event *x = &a->events[i], *y = &b->events[i];
        if (x->sig != y->sig) differ(what, "event.sig", i, (uint64_t)x->sig, (uint64_t)y->sig);
        if (x->address != y->address) differ(what, "event.address", i, x->address, y->address);
        if (x->rip != y->rip) differ(what, "event.rip", i, x->rip, y->rip);
        for (unsigned g = 0; g < 16; g++)
            if (x->gpr[g] != y->gpr[g]) differ(what, "event.gpr", i * 100 + g, x->gpr[g], y->gpr[g]);
        for (unsigned k = 0; k < 32; k++)
            if (x->around[k] != y->around[k]) differ(what, "event.around", i * 100 + k, x->around[k], y->around[k]);
    }
    for (unsigned g = 0; g < 16; g++)
        if (a->gpr[g] != b->gpr[g]) differ(what, "gpr", g, a->gpr[g], b->gpr[g]);
    if ((a->rflags & 0xcd5) != (b->rflags & 0xcd5)) differ(what, "rflags", 0, a->rflags, b->rflags);
    if (a->mxcsr != b->mxcsr) differ(what, "mxcsr", 0, a->mxcsr, b->mxcsr);
    for (unsigned k = 0; k < sizeof(a->ymm); k++)
        if (a->ymm[k] != b->ymm[k]) differ(what, "ymm byte", k, a->ymm[k], b->ymm[k]);
    for (unsigned k = 0; k < REGION; k++)
        if (a->memory[k] != b->memory[k]) differ(what, "memory", k, a->memory[k], b->memory[k]);
}

/* Fresh random memory, registers and vector state, and the three bases and the stack set. */
static void random_state(void)
{
    for (unsigned i = 0; i < REGION; i += 8) {
        const uint64_t v = random64();
        memcpy(initial + i, &v, 8);
    }
    for (unsigned i = 0; i < 16; i++) prog_in[i] = random64();
    for (unsigned i = 0; i < sizeof(prog_in_ymm); i += 8) {
        const uint64_t v = random64();
        memcpy(prog_in_ymm + i, &v, 8);
    }
    prog_in[RBX] = base_at[0];
    prog_in[R12] = base_at[1];
    prog_in[R13] = base_at[2];
    prog_in[RSP] = P + STACK_TOP;
    prog_in_rflags = 0x202 | (random64() & 0x8d5);
    prog_in_mxcsr = 0x3f80;   /* round down, all masked: what a call out must give back */
}
/* host page h's four pages from a pattern ("wrww"). */
static void host_layout(unsigned h, const char *four) { memcpy(layout + h * 4, four, 4); }
static void plain_layout(void)
{
    memset(layout, 'w', PAGES);
    layout[PAGES] = 0;
}

static void both(const char *what)
{
    run_program(NATIVE, &native_result);
    run_program(ENGINE, &engine_result);
    compare(what, &native_result, &engine_result);
}

/* ---------------------------------------------------------------- the cases */
static void api(void)
{
    PwWineMmu4kStats stats;
    PwWineMmu4kRegs regs = { { 0 }, 0x1234, 0x202 };
    uintptr_t address = 0;
    assert(!pw_wine_mmu4k_ready());
    assert(pw_wine_mmu4k_init(NULL, slots, PW_WINE_MMU4K_SLOT_BYTES, 7) == PW_ERR_PRECONDITION);
    assert(pw_wine_mmu4k_init(&engine_ops, NULL, PW_WINE_MMU4K_SLOT_BYTES, 7) == PW_ERR_PRECONDITION);
    assert(pw_wine_mmu4k_init(&engine_ops, slots, 4096, 7) == PW_ERR_PRECONDITION);
    assert(pw_wine_mmu4k_init(&engine_ops, slots, PW_WINE_MMU4K_SLOT_BYTES, 1) == PW_ERR_PRECONDITION);
    assert(pw_wine_mmu4k_init(&engine_ops, slots, PW_WINE_MMU4K_SLOT_BYTES, 15) == PW_ERR_PRECONDITION);
    assert(!pw_wine_mmu4k_enter(0, &regs, P, P));
    uint32_t low, high;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    assert((low & 7) == 7);   /* the generated programs use AVX */
    assert(pw_wine_mmu4k_init(&engine_ops, slots, PW_WINE_MMU4K_SLOT_BYTES, low & 7) == PW_OK);
    assert(pw_wine_mmu4k_ready());
    assert(pw_wine_mmu4k_init(&engine_ops, slots, PW_WINE_MMU4K_SLOT_BYTES, low & 7) == PW_ERR_STATE);
    for (int i = 0; i < PW_WINE_MMU4K_STACKS; i++) assert(pw_wine_mmu4k_stack_take() == i);
    assert(pw_wine_mmu4k_stack_take() == -1);
    pw_wine_mmu4k_stats(&stats);
    assert(stats.stack_full == 1);
    for (int i = 1; i < PW_WINE_MMU4K_STACKS; i++) pw_wine_mmu4k_stack_give(i);
    assert(!pw_wine_mmu4k_enter(-1, &regs, P, P) && !pw_wine_mmu4k_enter(5, &regs, P, P));
    PwWineMmu4kRun *run = pw_wine_mmu4k_enter(0, &regs, P, P + 8);
    assert(run && regs.rip == (uintptr_t)pw_wine_mmu4k_start && regs.gpr[RDI] == (uintptr_t)run &&
           regs.gpr[RSP] == (uintptr_t)run && !(regs.gpr[RSP] & 15) && regs.rflags == 0x202);
    regs.rip = 0x1234;
    assert(pw_wine_mmu4k_signal(run, SIGSEGV, 0, &regs, &address) == PW_WINE_MMU4K_NOT_OURS && regs.rip == 0x1234);
    regs.rip = (uintptr_t)pw_wine_mmu4k_exit_trap;
    assert(pw_wine_mmu4k_signal(run, SIGSEGV, 0, &regs, &address) == PW_WINE_MMU4K_NOT_OURS);   /* not SIGILL */
    assert(pw_wine_mmu4k_signal(NULL, SIGILL, 0, &regs, &address) == PW_WINE_MMU4K_NOT_OURS);
    pw_wine_mmu4k_stack_give(0);
    assert(pw_wine_mmu4k_stack_take() == 0);
    pw_wine_mmu4k_stack_give(0);
    puts("mmu4k M0: init checks its arguments and runs once, 16 run stacks then -1 (counted), enter and signal refuse "
         "what is not a run's");
}

/* M1 (and M4 with exceptions): random streams near page edges, random 4 KiB protections. */
static void streams(unsigned count, unsigned length, int exceptions, PwWineMmu4kStats *total)
{
    for (unsigned s = 0; s < count; s++) {
        random_state();
        plain_layout();
        for (unsigned h = 0; h < 6; h++) {
            const unsigned pick = below(6);
            for (unsigned i = 0; i < 4; i++)
                layout[h * 4 + i] = pick == 0 ? 'w' : pick == 1 ? 'r' : below(2) ? 'w' : 'r';
        }
        layout[8] = 'r';
        layout[STACK_PAGE] = 'w';
        begin();
        for (unsigned i = 0; i < length; i++) gen_element(exceptions);
        finish();
        char what[48];
        snprintf(what, sizeof(what), "%s-%u", exceptions ? "M4" : "M1", s);
        both(what);
        total->runs += engine_result.stats.runs;
        total->insns += engine_result.stats.insns;
        total->stepfall += engine_result.stats.stepfall;
        for (unsigned i = 0; i < PW_WINE_MMU4K_EXITS; i++) total->exits[i] += engine_result.stats.exits[i];
        total->stack_damage += engine_result.stats.stack_damage;
    }
}

static void m2_straddle(void)
{
    random_state();
    plain_layout();
    host_layout(0, "wrww");
    host_layout(1, "wrww");
    begin();
    legacy("", 1, "89", RAX, RBX, 0x1000 - 4, 0);   /* 8 bytes across page 0 | page 1 */
    legacy("f3", 0, "0f 7f", 3, RBX, 0x5000 - 8, 0);  /* 16 bytes across page 4 | page 5 */
    vex(1, 2, 1, 0x7f, 5, RBX, 0x1000 - 28);         /* 32 bytes, now that page 1 is open: no fault */
    finish();
    both("M2");
    assert(engine_result.count == 2 && engine_result.events[0].address == P + 0x1000 &&
           engine_result.events[1].address == P + 0x5000);
    /* Neither page had changed when the handler ran. */
    assert(!memcmp(engine_result.events[0].around, initial + 0x1000 - 16, 32));
    assert(!memcmp(engine_result.events[1].around, initial + 0x5000 - 16, 32));
    assert(engine_result.stats.exits[PW_WINE_MMU4K_EXIT_STRADDLE] == 2);
    puts("mmu4k M2: 8- and 16-byte stores across a plain | read-only edge fault once at the read-only page's first "
         "byte with both pages unchanged, as the 4 KiB CPU does");
}

static void m3_rep(void)
{
    random_state();
    plain_layout();
    host_layout(0, "wrww");              /* rep movsb runs from page 0 into read-only page 1 */
    host_layout(1, "wrww");              /* rep stosq's first element straddles page 4 | read-only page 5 */
    host_layout(2, "wrww");              /* rep movsq's destination, page 8 */
    host_layout(3, "wwrw");              /* overlapping copies in page 12 */
    host_layout(5, "nnnn");              /* rep movsq's source runs into this no-access host page */
    begin();
    mov_imm64(RDI, P + 0x1000 - 100);
    mov_imm64(RSI, P + 0x10007);
    bytes("b9 2c 01 00 00 f3 a4");       /* mov ecx, 300; rep movsb */
    mov_imm64(RDI, P + 0x5000 - 3);
    bytes("b9 03 00 00 00 f3 48 ab");    /* mov ecx, 3; rep stosq */
    mov_imm64(RDI, P + 0x8100);
    mov_imm64(RSI, P + 0x14000 - 64);
    bytes("b9 40 00 00 00 f3 48 a5");    /* mov ecx, 64; rep movsq: 8 elements, then a source fault */
    mov_imm64(RSI, P + 0xc800);
    mov_imm64(RDI, P + 0xc803);
    bytes("b9 00 01 00 00 f3 a4");       /* rep movsb onto its own source, dst = src + 3 */
    mov_imm64(RSI, P + 0xca43);
    mov_imm64(RDI, P + 0xca40);
    bytes("b9 00 01 00 00 f3 a4");       /* dst = src - 3 */
    mov_imm64(RSI, P + 0xcc00);
    mov_imm64(RDI, P + 0xcc08);
    bytes("b9 40 00 00 00 f3 48 a5");    /* rep movsq, dst = src + 8 */
    finish();
    both("M3");
    const Event *e = engine_result.events;
    assert(engine_result.count == 3 && e[0].address == P + 0x1000 && e[0].gpr[RCX] == 200 &&
           e[1].address == P + 0x5000 && e[1].gpr[RCX] == 3 && e[2].address == P + 0x14000 && e[2].gpr[RCX] == 56);
    assert(engine_result.stats.exits[PW_WINE_MMU4K_EXIT_FAULT] >= 1 &&
           engine_result.stats.exits[PW_WINE_MMU4K_EXIT_STRADDLE] == 1);
    puts("mmu4k M3: rep movsb into a read-only page, rep stosq with a straddling element, rep movsq whose source "
         "runs into a no-access page (8 elements done), and copies onto their own source: RCX/RSI/RDI and memory as "
         "native at every fault");
}

static unsigned m5_store_at;
/* The waiting thread's turn: the first run hands the mutex over at its end, and the waiter acts and leaves. */
static void reprotect_at_first_yield(unsigned count)
{
    if (count != 1) return;
    vprot[0] = 'r';
    assert(!mprotect((void *)P, HOST, host_prot(0)));
    waiting = 0;
}
static void m5_reprotect(void)
{
    random_state();
    plain_layout();
    host_layout(0, "wrww");
    begin();
    bytes("b9 d0 07 00 00");   /* mov ecx, 2000 */
    uint8_t *loop = pc;
    m5_store_at = (unsigned)((uintptr_t)pc - CODE);
    legacy("", 0, "89", RCX, RBX, 0x10, 0);   /* L: mov [rbx+0x10], ecx */
    bytes("ff c9 75");
    emit((unsigned)(loop - (pc + 1)) & 0xff);
    finish();
    on_unlock = reprotect_at_first_yield;
    unlocks = 0;
    waiting = 1;
    run_program(ENGINE, &engine_result);
    on_unlock = NULL;
    assert(!waiting && engine_result.count == 1);
    const Event *e = &engine_result.events[0];
    uint32_t old;
    memcpy(&old, e->around + 16, 4);
    assert(e->address == P + 0x10 && e->rip == CODE + m5_store_at && old == (uint32_t)e->gpr[RCX] + 1);
    uint32_t last;
    memcpy(&last, engine_result.memory + 0x10, 4);
    assert(last == 1 && engine_result.stats.exits[PW_WINE_MMU4K_EXIT_YIELD] == 1);
    puts("mmu4k M5: a page made read-only by a thread the run handed the mutex over to faults at the next store to "
         "it, natively, with its old bytes in place");
}

static uintptr_t m6_first;
static void forget_at_first_yield(unsigned count)
{
    if (count != 1) return;
    m6_first = last_alias;
    pw_wine_alias_forget(0, REGION);   /* the region's direct memory is about to go: every window over it first */
    waiting = 0;
}
static void m6_forget(void)
{
    PwWineAliasStats before, after;
    random_state();
    plain_layout();
    host_layout(0, "wrww");
    begin();
    bytes("b9 dc 05 00 00");   /* mov ecx, 1500 */
    uint8_t *loop = pc;
    legacy("", 0, "89", RCX, RBX, 0x10, 0);
    bytes("ff c9 75");
    emit((unsigned)(loop - (pc + 1)) & 0xff);
    finish();
    pw_wine_alias_forget(0, REGION);   /* no window from earlier cases */
    pw_wine_alias_stats(&before);
    const uint64_t generation = pw_wine_alias_generation();
    on_unlock = forget_at_first_yield;
    unlocks = 0;
    waiting = 1;
    run_program(ENGINE, &engine_result);
    on_unlock = NULL;
    pw_wine_alias_stats(&after);
    uint32_t last;
    memcpy(&last, engine_result.memory + 0x10, 4);
    assert(!waiting && engine_result.stats.exits[PW_WINE_MMU4K_EXIT_YIELD] == 1);
    assert(last == 1 && engine_result.count == 0 && !engine_result.stats.exits[PW_WINE_MMU4K_EXIT_FAULT]);
    assert(after.forgets - before.forgets == 1 && after.maps - before.maps == 2 &&
           pw_wine_alias_generation() > generation && m6_first && last_alias != m6_first);
    /* The old window is the helper's reservation again: no longer the memory, and nothing stored there. */
    assert(!mprotect((void *)m6_first, PAGE, PROT_READ));
    assert(*(volatile uint32_t *)(m6_first + 0x10) == 0);
    assert(!mprotect((void *)m6_first, PAGE, PROT_NONE));
    /* Inside a run: classifying host page 1 gives back the window the run holds for host page 0. */
    host_layout(1, "wrww");
    begin();
    legacy("", 0, "89", RAX, RBX, 0x10, 0);
    legacy("", 0, "89", RCX, RBX, 0x4010, 0);
    legacy("", 0, "89", RDX, RBX, 0x20, 0);
    finish();
    pw_wine_alias_forget(0, REGION);   /* no window yet: the run maps one, then a second after the first goes back */
    pw_wine_alias_stats(&before);
    forget_on_host = 1;
    run_program(ENGINE, &engine_result);
    run_program(NATIVE, &native_result);
    compare("M6-generation", &native_result, &engine_result);
    pw_wine_alias_stats(&after);
    assert(forget_on_host < 0 && engine_result.stats.runs == 1 && !engine_result.stats.exits[PW_WINE_MMU4K_EXIT_FAULT]);
    assert(after.forgets - before.forgets == 1 && after.maps - before.maps == 2);
    puts("mmu4k M6: an alias forgotten at a hand-over, or by the run's own classify, is never used again: the next "
         "classify maps a new window, every store lands, the old range holds nothing");
}

static void m7_zero_progress(void)
{
    /* An instruction the engine does not run (maskmovdqu: IMPL), first: the step. */
    random_state();
    plain_layout();
    host_layout(0, "wrww");
    begin();
    mov_imm64(RDI, P + 0x20);
    bytes("66 0f f7 c1");   /* maskmovdqu xmm0, xmm1 */
    finish();
    both("M7-form");
    assert(engine_result.stats.stepfall == 1 && engine_result.stats.exits[PW_WINE_MMU4K_EXIT_FORM] == 1 &&
           engine_result.stats.runs == 1);
    int found = 0;
    for (unsigned i = 0; i < 4; i++) found |= engine_result.stats.top[i] == 0x01f7 && engine_result.stats.top_count[i];
    assert(found);
    /* An alias that faults: each store's run ends with nothing done, and steps. */
    begin();
    legacy("", 1, "89", RAX, RBX, 0x10, 0);
    legacy("", 1, "89", RCX, RBX, 0x20, 0);
    legacy("", 1, "89", RDX, RBX, 0x30, 0);
    finish();
    broken_alias = 1;
    run_program(ENGINE, &engine_result);
    broken_alias = 0;
    run_program(NATIVE, &native_result);
    compare("M7-alias", &native_result, &engine_result);
    assert(engine_result.stats.stepfall == 3 && engine_result.stats.exits[PW_WINE_MMU4K_EXIT_FAULT] == 3);
    puts("mmu4k M7: a first instruction the engine does not run, and an alias that faults, end in 1122's step (counted, "
         "its opcode in the top), never a loop");
}

static void m8_budgets(void)
{
    struct timespec t0, t1;
    random_state();
    plain_layout();
    host_layout(0, "wrww");
    /* Idle: one alias store, then 2,000 instructions without one. */
    begin();
    legacy("", 0, "89", RAX, RBX, 0x10, 0);
    for (unsigned i = 0; i < 2000; i++) bytes("83 c0 01");   /* add eax, 1 */
    finish();
    locks = unlocks = 0;
    both("M8-idle");
    assert(engine_result.stats.runs == 1 && engine_result.stats.exits[PW_WINE_MMU4K_EXIT_IDLE] == 1 &&
           engine_result.stats.insns == 1 + PW_WINE_MMU4K_IDLE);
    assert(locks == 1 && unlocks == 1);   /* nobody waits: one hold, nothing in between */
    /* Budget: an alias store every 4 instructions for 20,000. */
    begin();
    bytes("b9 88 13 00 00");   /* mov ecx, 5000 */
    uint8_t *loop = pc;
    legacy("", 0, "89", RAX, RBX, 0x10, 0);
    bytes("83 c0 01 ff c9 75");
    emit((unsigned)(loop - (pc + 1)) & 0xff);
    finish();
    locks = unlocks = 0;
    run_program(NATIVE, &native_result);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    run_program(ENGINE, &engine_result);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    compare("M8-budget", &native_result, &engine_result);
    const PwWineMmu4kStats *s = &engine_result.stats;
    assert(s->exits[PW_WINE_MMU4K_EXIT_BUDGET] == 2 && s->runs == 3 && !s->exits[PW_WINE_MMU4K_EXIT_YIELD]);
    assert(locks == s->runs && unlocks == locks);
    const uint64_t ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull + (uint64_t)(t1.tv_nsec - t0.tv_nsec);
    printf("mmu4k M8: idle exit at %u instructions without an alias store, budget at %u, one lock per run (%u); "
           "%llu instructions in %llu us: %llu ns each with the run's entries and exits\n", PW_WINE_MMU4K_IDLE,
           PW_WINE_MMU4K_BUDGET, locks, (unsigned long long)s->insns, (unsigned long long)(ns / 1000),
           (unsigned long long)(ns / s->insns));
    /* A thread waits throughout: every run hands the mutex over at its first look, PW_WINE_MMU4K_YIELD instructions
     * (exactly, as the yields' count is the instructions' quotient), but the last, which ends at the program's exit. */
    waiting = 1;
    locks = unlocks = 0;
    run_program(ENGINE, &engine_result);
    waiting = 0;
    compare("M8-yield", &native_result, &engine_result);
    const uint64_t yields = s->exits[PW_WINE_MMU4K_EXIT_YIELD];
    assert(yields > 1 && yields == s->runs - 1 && s->exits[PW_WINE_MMU4K_EXIT_FETCH] == 1 &&
           s->insns / PW_WINE_MMU4K_YIELD == yields && locks == s->runs && unlocks == locks);
    printf("mmu4k M8-yield: with a waiter, %llu runs each handed the mutex over at %u instructions, results as "
           "native\n", (unsigned long long)yields, PW_WINE_MMU4K_YIELD);
}

static int in_slots(const char *hex)
{
    uint8_t want[32];
    unsigned n = 0;
    for (char *end; *hex && n < sizeof(want); hex = end) {
        want[n++] = (uint8_t)strtoul(hex, &end, 16);
        if (*end == 0) break;
    }
    return memmem(slots, PW_WINE_MMU4K_SLOT_BYTES, want, n) != NULL;
}
static void m9_encoding(void)
{
    random_state();
    plain_layout();
    host_layout(0, "wrww");
    host_layout(1, "rwww");
    prog_in[R8] = random64();
    begin();
    bytes("89 83 23 01 00 00");            /* mov [rbx+0x123], eax: no REX */
    bytes("c5 fe 7f 83 00 02 00 00");      /* vmovdqu [rbx+0x200], ymm0: VEX2 */
    bytes("4d 89 84 24 10 20 00 00");      /* mov [r12+0x2010], r8: SIB base, r8 named */
    bytes("48 89 e0");                     /* mov rax, rsp */
    bytes("48 83 ec 08 48 83 c4 08");      /* sub rsp, 8; add rsp, 8 */
    bytes("67 48 8d 04 0b");               /* lea rax, [ebx+ecx]: a 32-bit address, zero-extended */
    bytes("88 a3 00 03 00 00");            /* mov [rbx+0x300], ah: needs a REX it cannot take */
    finish();
    both("M9");
    assert(in_slots("41 89 00 c3"));       /* REX.B added: [r8] */
    assert(in_slots("c4 c1 7e 7f 00 c3")); /* VEX2 widened to VEX3 with B */
    assert(in_slots("4d 89 01 c3"));       /* r8 named: [r9] */
    assert(in_slots("4c 89 c0 c3"));       /* RSP as r8 */
    assert(in_slots("49 83 e8 08 c3"));    /* sub r8, 8 */
    assert(engine_result.stats.exits[PW_WINE_MMU4K_EXIT_FORM] == 2 && engine_result.stats.stepfall == 1);
    puts("mmu4k M9: REX added for [scratch], VEX2 widened to VEX3, a named r8 skipped, RSP operands through a second "
         "scratch, an AH store ends the run (form) and steps");
}

/* M10: what an operand does not bound, and code the CPU refuses, each inside a run (a store into page 0 of the mixed
 * host page 0 starts it), each faulting as natively: the run ends before it and the native retry decides. */
static void m10_beyond(void)
{
    static const char *const bit_ops[3] = { "0f ab", "0f b3", "0f bb" };   /* bts, btr, btc qword [m], rcx */
    static const struct { const char *host1; uint64_t rcx; int32_t disp; uint64_t fault; } offsets[3] = {
        { "wwww", 0x8003, 0x10, P + 0x1010 },                   /* +0x1000: the read-only neighbour */
        { "rwww", 0x20003, 0x10, P + 0x4010 },                  /* +0x4000: another host page */
        { "wwww", (uint64_t)-0x7ffd, 0x2010, P + 0x1010 },      /* -0x1000: back into the read-only neighbour */
    };
    for (unsigned o = 0; o < 3; o++)
        for (unsigned k = 0; k < 3; k++) {
            char what[32];
            random_state();
            plain_layout();
            host_layout(0, "wrww");
            host_layout(1, offsets[o].host1);
            prog_in[RCX] = offsets[o].rcx;
            begin();
            legacy("", 1, "89", RAX, RBX, 0x20, 0);
            legacy("", 1, bit_ops[k], RCX, RBX, offsets[o].disp, 0);
            finish();
            snprintf(what, sizeof(what), "M10-bit-%u-%u", o, k);
            both(what);
            assert(engine_result.count == 1 && engine_result.events[0].address >= offsets[o].fault &&
                   engine_result.events[0].address < offsets[o].fault + 8 &&
                   engine_result.stats.exits[PW_WINE_MMU4K_EXIT_FORM] == 1);
        }
    /* GS, then a DS the CPU ignores in long mode: the store goes to the GS base + EA, into the read-only page 1. */
    random_state();
    plain_layout();
    host_layout(0, "wrww");
    begin();
    legacy("", 1, "89", RAX, RBX, 0x20, 0);
    bytes("65 3e 48 89 43 10");   /* mov gs:[rbx+0x10], rax */
    finish();
    assert(!syscall(SYS_arch_prctl, ARCH_SET_GS, 0x1000ul));
    both("M10-gs");
    assert(!syscall(SYS_arch_prctl, ARCH_SET_GS, 0ul));
    assert(engine_result.count == 1 && engine_result.events[0].address == P + 0x1010 &&
           engine_result.stats.exits[PW_WINE_MMU4K_EXIT_FORM] == 1);
    /* A CALL into a read-write data page holding a ret: the execute fault at the target, the return address pushed. */
    random_state();
    plain_layout();
    host_layout(0, "wrww");
    initial[0x10000] = 0xc3;
    prog_in[RDX] = P + 0x10000;
    begin();
    legacy("", 1, "89", RAX, RBX, 0x20, 0);
    bytes("ff d2");               /* call rdx */
    finish();
    both("M10-exec");
    assert(engine_result.count == 1 && engine_result.events[0].rip == P + 0x10000 &&
           engine_result.events[0].gpr[RSP] == P + STACK_TOP - 8 &&
           engine_result.stats.exits[PW_WINE_MMU4K_EXIT_FETCH] == 1);
    puts("mmu4k M10: bts/btr/btc with a register offset into a read-only neighbour, another host page and backwards, a "
         "GS store with a DS after it, and a call into a page that may not be executed end the run and fault natively");
}

int main(void)
{
    alarm(120);   /* a livelock fails the test instead of hanging it */
    memory = memfd_create("direct-memory", 0);
    assert(memory >= 0 && !ftruncate(memory, REGION));
    assert(mmap((void *)P, REGION, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, memory, 0) == (void *)P);
    assert(mmap((void *)BACKDOOR, REGION, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, memory, 0) ==
           (void *)BACKDOOR);
    assert(mmap((void *)CODE, CODE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)CODE);
    assert(mmap((void *)ALIASES, ALIAS_SLOTS * WINDOW, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0) == (void *)ALIASES);
    slots = mmap(NULL, PW_WINE_MMU4K_SLOT_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(slots != MAP_FAILED);
    stack_t alt = { .ss_sp = malloc(1 << 16), .ss_size = 1 << 16 };
    assert(alt.ss_sp && !sigaltstack(&alt, NULL));
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    const int caught[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP };
    for (unsigned i = 0; i < sizeof(caught) / sizeof(caught[0]); i++) assert(!sigaction(caught[i], &action, NULL));
    assert(pw_wine_alias_init(&alias_ops) == PW_OK);

    api();
    m9_encoding();
    m10_beyond();
    m2_straddle();
    m3_rep();
    PwWineMmu4kStats total;
    memset(&total, 0, sizeof(total));
    streams(40, 60, 1, &total);
    assert(total.exits[PW_WINE_MMU4K_EXIT_FAULT] > 0 && recovers > 0);
    puts("mmu4k M4: #DE and #UD raised in a slot reach the program at the same RIP with the same registers, ymm0-15 "
         "and MXCSR intact through every call out");
    streams(200, 60, 0, &total);
    printf("mmu4k M1: 240 random streams of 60 elements, native 4 KiB pages against the engine: identical faults, "
           "registers, vectors and memory (%llu runs, %llu instructions, exits idle %llu budget %llu form %llu "
           "protected %llu straddle %llu fault %llu fetch %llu noalias %llu, stepfall %llu)\n",
           (unsigned long long)total.runs, (unsigned long long)total.insns,
           (unsigned long long)total.exits[0], (unsigned long long)total.exits[1], (unsigned long long)total.exits[2],
           (unsigned long long)total.exits[3], (unsigned long long)total.exits[4], (unsigned long long)total.exits[5],
           (unsigned long long)total.exits[6], (unsigned long long)total.exits[7], (unsigned long long)total.stepfall);
    assert(!total.stack_damage && total.runs > 1000);
    m5_reprotect();
    m6_forget();
    m7_zero_progress();
    m8_budgets();
    puts("mmu4k: passed");
    return 0;
}
