/* SPDX-License-Identifier: GPL-3.0-or-later
 * M0 (docs/IA32_DESIGN.md): can a payload process - the kind every Windows program of ours runs in,
 * started through websrv/elfldr - run 32-bit code natively, and at what cost? Run through
 * tools/run-m0-probe.py, one mode per process (a crash ends only its own mode):
 *
 *   ident        the process's selectors and FS/GS bases
 *   low4g        free address space below 4 GiB, in 16 MiB slots
 *   switch       enter compatibility mode (CS 0x33) and back; a 32-bit computation; 100,000 round trips timed
 *   fs           the cost of switching the FS base through sysarch and back
 *   signal32     a fault inside 32-bit code: is it delivered to our 64-bit handler, and can we resume?
 *   fsselector   load FS with the 32-bit FS selector inside 32-bit code and read fs:[0]
 *   signal32fs   a fault inside 32-bit code with a guest FS base loaded (what Windows code always has)
 *   fsguest      a guest FS base set through sysarch serves fs:[0x18] (the TEB's Self) to 32-bit code
 *   sigpath      which raw signal entry points libkernel exports by name (for a host-FS-first entry)
 *   signal32fsraw  signal32fs with the handler registered through libkernel_sys's _sigaction, resolved by
 *                name: does the kernel then call our handler before anything touches the guest's FS?
 *   ldt          an LDT descriptor whose base is the TEB (sysarch I386_SET_LDT), loaded into FS inside
 *                32-bit code with no system call: does fs:[0x18] read the TEB's Self?
 *   ldtsignal    a fault in 32-bit code with that LDT FS loaded, and an ordinary sigaction handler
 *   rawsig       a host-FS-first entry registered with the kernel's own sigaction (system call 416, no
 *                libkernel wrapper): a fault with the guest FS loaded is handled, the ucontext's layout is
 *                found by searching it for planted values, and the guest resumes after the instruction
 *   fsreload     after a sysarch changes the FS base, does reloading the 32-bit FS selector (0x13) inside
 *                32-bit code give the new base, or the descriptor's stale one? AMD64_SET_FSBASE and
 *                I386_SET_FSBASE, each at once and after 20 ms of spinning. A page mapped at the host base's
 *                low 32 bits holds a marker of its own, so a stale base reads it instead of faulting (M3:
 *                wow64cpu reloads %fs when it enters 32-bit code with a full context)
 *   fsalias      with the host FS base left alone, does 32-bit code address the low 32 bits of it? A page there
 *                holds markers: fs:[0x18] read without a reload, fs:[0] after reloading 0x13, and the cost of a
 *                round trip that reads fs:[0x18] (M4: a 32-bit TEB placed there needs no FS switch at all)
 *   tcbs         the FS base (libkernel's thread pointer) of 16 threads alive at once: how far apart they are,
 *                which decides whether every thread's 32-bit TEB can sit at the low half of its own
 *   stop pid=N   ends an M0 probe left running (a mode that looped): only a process named "m0", never itself
 *
 * Every step prints one "M0 <mode> <step> key=value..." line before the next can fail, so a crash
 * shows how far it got. The 32-bit code is assembled in tools/run-m0-probe.py's comment and checked
 * with tools/ia32-dump.sh; here it is bytes with its result and return addresses patched in.
 */
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <x86/sysarch.h>
#include <ps5/kernel.h>
#include <x86/segments.h>
#include <sys/sysctl.h>
#include <sys/user.h>

enum { CS32 = 0x33, CS64 = 0x43, LOW_BYTES = 0x10000 };

/* compute, fault, fsread and fault_fs, at the offsets below (llvm-mc, i386). Results go to
 * 0x11111100 + n and every exit is ljmp 0x43:0x22222200; both are patched to the low page. */
static const uint8_t code32[] = {
    0x8c, 0xc9, 0x89, 0x0d, 0x00, 0x11, 0x11, 0x11, 0x8c, 0xd0, 0xa3, 0x04, 0x11, 0x11, 0x11, 0xb8,
    0x06, 0x00, 0x00, 0x00, 0x50, 0xe8, 0x0f, 0x00, 0x00, 0x00, 0x83, 0xc4, 0x04, 0xa3, 0x08, 0x11,
    0x11, 0x11, 0xea, 0x00, 0x22, 0x22, 0x22, 0x43, 0x00, 0x8b, 0x44, 0x24, 0x04, 0x6b, 0xc0, 0x07,
    0xc3, 0xa1, 0x00, 0x00, 0x00, 0x00, 0xea, 0x00, 0x22, 0x22, 0x22, 0x43, 0x00, 0xb8, 0x13, 0x00,
    0x00, 0x00, 0x8e, 0xe0, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0xa3, 0x0c, 0x11, 0x11, 0x11, 0xea,
    0x00, 0x22, 0x22, 0x22, 0x43, 0x00, 0x64, 0xa1, 0x18, 0x00, 0x00, 0x00, 0xa3, 0x10, 0x11, 0x11,
    0x11, 0xa1, 0x00, 0x00, 0x00, 0x00, 0xea, 0x00, 0x22, 0x22, 0x22, 0x43, 0x00,
};
enum { AT_COMPUTE = 0x00, AT_FAULT = 0x31, AT_FSREAD = 0x3d, AT_FAULT_FS = 0x56, AT_RETURN_ONLY = 0x22 };
/* fault_fs without its fault: the same bytes again at AT_TEB_READ, its `mov eax,[0]` made five NOPs. */
enum { AT_TEB_READ = 0x80, FAULT_FS_LOAD_AT = 11 };

/* The low page: 32-bit code at 0x100, the 64-bit return stub at 0x40, results at 0x200, the far
 * pointer at 0x20, the 32-bit stack's top at 0x8000, a fake TEB at 0x9000. */
enum { LOW_FARPTR = 0x20, LOW_BACK64 = 0x40, LOW_CODE = 0x100, LOW_RESULTS = 0x200, LOW_STACK = 0x8000, LOW_TEB = 0x9000 };

static uint8_t *low;
uint64_t m0_saved_rsp;      /* the 64-bit stack, while 32-bit code runs */

/* m0_enter32(farptr, stack): save the callee-saved registers and RSP, switch to the low stack and jump far
 * through farptr (m16:32). m0_back64 is where the low page's 64-bit stub jumps back to. */
void m0_enter32(void *farptr, uint32_t stack);
void m0_back64(void);
__asm__(".text\n"
        ".globl m0_enter32\n"
        "m0_enter32:\n"
        "  pushq %rbx\n  pushq %rbp\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
        "  movq %rsp, m0_saved_rsp(%rip)\n"
        "  movl %esi, %esp\n"
        "  ljmpl *(%rdi)\n"
        ".globl m0_back64\n"
        "m0_back64:\n"
        "  movq m0_saved_rsp(%rip), %rsp\n"
        "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbp\n  popq %rbx\n"
        "  ret\n");

static void line(const char *text)
{
    fputs(text, stdout);
    fflush(stdout);
}

static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static uint32_t low32(const void *p)
{
    return (uint32_t)(uintptr_t)p;
}

/* A page below 4 GiB, read-write-execute through the payload SDK's mmap. */
static int low_page(void)
{
    for (uintptr_t at = 0x10000000; at < 0xf0000000u; at += 0x1000000) {
        void *p = mmap((void *)at, LOW_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) continue;
        if ((uintptr_t)p + LOW_BYTES <= 0x100000000ull) { low = p; return 0; }
        munmap(p, LOW_BYTES);
    }
    return -1;
}

/* Copies the 32-bit code and the 64-bit stub into the low page and patches their addresses. */
static void low_install(void)
{
    memset(low, 0xcc, LOW_BYTES);
    memcpy(low + LOW_CODE, code32, sizeof(code32));
    for (size_t i = 0; i + 4 <= sizeof(code32); i++) {
        uint32_t v;
        memcpy(&v, low + LOW_CODE + i, 4);
        if (v >= 0x11111100u && v < 0x11111120u) v = low32(low + LOW_RESULTS) + (v - 0x11111100u);
        else if (v == 0x22222200u) v = low32(low + LOW_BACK64);
        else continue;
        memcpy(low + LOW_CODE + i, &v, 4);
    }
    memcpy(low + LOW_CODE + AT_TEB_READ, low + LOW_CODE + AT_FAULT_FS, sizeof(code32) - AT_FAULT_FS);
    memset(low + LOW_CODE + AT_TEB_READ + FAULT_FS_LOAD_AT, 0x90, 5);
    /* back64: movabs rax, m0_back64; jmp rax */
    uint8_t *b = low + LOW_BACK64;
    const uint64_t target = (uint64_t)(uintptr_t)m0_back64;
    b[0] = 0x48; b[1] = 0xb8; memcpy(b + 2, &target, 8); b[10] = 0xff; b[11] = 0xe0;
}

static void enter_at(unsigned offset)
{
    const uint32_t entry = low32(low + LOW_CODE + offset);
    memcpy(low + LOW_FARPTR, &entry, 4);
    const uint16_t cs = CS32;
    memcpy(low + LOW_FARPTR + 4, &cs, 2);
    m0_enter32(low + LOW_FARPTR, low32(low + LOW_STACK));
}

static uint32_t result(unsigned n)
{
    uint32_t v;
    memcpy(&v, low + LOW_RESULTS + 4 * n, 4);
    return v;
}

static void mode_ident(void)
{
    uint16_t cs, ss, ds, es, fs, gs;
    __asm__ volatile("movw %%cs,%0\n movw %%ss,%1\n movw %%ds,%2\n movw %%es,%3\n movw %%fs,%4\n movw %%gs,%5"
                     : "=r"(cs), "=r"(ss), "=r"(ds), "=r"(es), "=r"(fs), "=r"(gs));
    uint64_t fsbase = 0, gsbase = 0;
    const int f = sysarch(AMD64_GET_FSBASE, &fsbase), g = sysarch(AMD64_GET_GSBASE, &gsbase);
    printf("M0 ident pid=%d cs=%#x ss=%#x ds=%#x es=%#x fs=%#x gs=%#x fsbase=%#llx(%d) gsbase=%#llx(%d) text=%p\n",
           getpid(), cs, ss, ds, es, fs, gs, (unsigned long long)fsbase, f, (unsigned long long)gsbase, g,
           (void *)m0_back64);
    fflush(stdout);
}

static void mode_low4g(void)
{
    unsigned free_slots = 0, first = 0, last = 0;
    for (uintptr_t at = 0x1000000; at < 0x100000000ull; at += 0x1000000) {
        void *p = mmap((void *)at, 0x1000000, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) continue;
        if ((uintptr_t)p == at) {
            if (!free_slots) first = (unsigned)(at >> 20);
            last = (unsigned)(at >> 20);
            free_slots++;
        }
        munmap(p, 0x1000000);
    }
    printf("M0 low4g free_16mib_slots=%u of 255 first_mib=%u last_mib=%u\n", free_slots, first, last);
    fflush(stdout);
}

static void mode_switch(void)
{
    line("M0 switch entering\n");
    enter_at(AT_COMPUTE);
    printf("M0 switch returned cs32=%#x ss32=%#x computed=%u (want 42)\n", result(0), result(1), result(2));
    fflush(stdout);
    enum { ROUNDS = 100000 };
    const double t0 = now_ns();
    for (unsigned i = 0; i < ROUNDS; i++) enter_at(AT_RETURN_ONLY);
    const double t1 = now_ns();
    printf("M0 switch round_trip_ns=%.1f rounds=%u\n", (t1 - t0) / ROUNDS, ROUNDS);
    fflush(stdout);
}

static void mode_fs(void)
{
    uint64_t host = 0, check = 0;
    sysarch(AMD64_GET_FSBASE, &host);
    uint64_t guest = (uint64_t)(uintptr_t)(low + LOW_TEB);
    enum { ROUNDS = 20000 };
    const double t0 = now_ns();
    for (unsigned i = 0; i < ROUNDS; i++) sysarch(AMD64_GET_FSBASE, &check);
    const double t1 = now_ns();
    int failures = 0;
    for (unsigned i = 0; i < ROUNDS; i++) {
        failures += sysarch(AMD64_SET_FSBASE, &guest) != 0;
        failures += sysarch(AMD64_SET_FSBASE, &host) != 0;
    }
    const double t2 = now_ns();
    sysarch(AMD64_GET_FSBASE, &check);
    printf("M0 fs get_ns=%.1f set_pair_ns=%.1f failures=%d restored=%d\n", (t1 - t0) / ROUNDS, (t2 - t1) / ROUNDS,
           failures, check == host);
    fflush(stdout);
}

static sigjmp_buf recover;
static volatile sig_atomic_t caught_signal;
static volatile uintptr_t caught_address;
static uint64_t host_fsbase;

/* The handler restores the host FS base first (a guest FS breaks libc's TLS), then leaves. */
static void on_fault(int sig, siginfo_t *info, void *context)
{
    (void)context;
    sysarch(AMD64_SET_FSBASE, &host_fsbase);
    caught_signal = sig;
    caught_address = (uintptr_t)info->si_addr;
    siglongjmp(recover, 1);
}

static int raw_registration;

static void install_handler(void)
{
    static uint8_t alternate[65536];
    stack_t ss = { .ss_sp = alternate, .ss_size = sizeof(alternate) };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    int (*registrar)(int, const struct sigaction *, struct sigaction *) = sigaction;
    if (raw_registration) {
        uint32_t handle = 0;
        if (kernel_dynlib_handle(getpid(), "libkernel_sys.sprx", &handle) == 0)
            registrar = (void *)kernel_dynlib_dlsym(getpid(), handle, "_sigaction");
        printf("M0 registration through _sigaction=%s\n", registrar ? "resolved" : "absent");
        fflush(stdout);
        if (!registrar) return;
    }
    registrar(SIGSEGV, &sa, NULL);
    registrar(SIGBUS, &sa, NULL);
    registrar(SIGILL, &sa, NULL);
}

static void mode_signal32(int with_guest_fs)
{
    sysarch(AMD64_GET_FSBASE, &host_fsbase);
    install_handler();
    memset(low + LOW_TEB, 0, 0x1000);
    const uint32_t self = low32(low + LOW_TEB);
    memcpy(low + LOW_TEB + 0x18, &self, 4);     /* the TEB's Self, as Windows has it */
    printf("M0 signal32%s armed\n", with_guest_fs ? "fs" : "");
    fflush(stdout);
    if (sigsetjmp(recover, 1) == 0) {
        if (with_guest_fs) {
            uint64_t guest = (uint64_t)self;
            if (sysarch(AMD64_SET_FSBASE, &guest)) { line("M0 signal32fs set_fsbase_failed\n"); return; }
        }
        enter_at(with_guest_fs ? AT_FAULT_FS : AT_FAULT);
        sysarch(AMD64_SET_FSBASE, &host_fsbase);
        line("M0 signal32 no_fault (unexpected)\n");
        return;
    }
    printf("M0 signal32%s recovered signal=%d address=%#lx teb_self_read=%#x\n", with_guest_fs ? "fs" : "",
           (int)caught_signal, (unsigned long)caught_address, with_guest_fs ? result(4) : 0u);
    fflush(stdout);
}

static void mode_fsguest(void)
{
    uint64_t host = 0, check = 0;
    sysarch(AMD64_GET_FSBASE, &host);
    memset(low + LOW_TEB, 0, 0x1000);
    const uint32_t self = low32(low + LOW_TEB);
    memcpy(low + LOW_TEB + 0x18, &self, 4);
    uint64_t guest = self;
    const int set = sysarch(AMD64_SET_FSBASE, &guest);
    enter_at(AT_TEB_READ);
    const int back = sysarch(AMD64_SET_FSBASE, &host);
    sysarch(AMD64_GET_FSBASE, &check);
    printf("M0 fsguest set=%d teb_self_read=%#x want=%#x restored=%d host_ok=%d\n", set, result(4), self, back,
           check == host);
    fflush(stdout);
}

static void mode_sigpath(void)
{
    static const char *const libraries[] = { "libkernel.sprx", "libkernel_sys.sprx", "libkernel_web.sprx" };
    static const char *const names[] = { "sigaction", "_sigaction", "__sys_sigaction", "sigreturn", "__sys_sigreturn",
                                         "_sigreturn", "sysarch", "__sys_sysarch", "syscall", "__syscall",
                                         "sigaltstack", "__sys_sigaltstack", "_thr_sighandler", "thr_sighandler" };
    for (size_t l = 0; l < sizeof(libraries) / sizeof(libraries[0]); l++) {
        uint32_t handle = 0;
        const int found = kernel_dynlib_handle(getpid(), libraries[l], &handle);
        printf("M0 sigpath library=%s found=%d handle=%u\n", libraries[l], found, handle);
        if (found) continue;
        for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
            const intptr_t at = kernel_dynlib_dlsym(getpid(), handle, names[n]);
            printf("M0 sigpath %s %s=%s\n", libraries[l], names[n], at ? "exported" : "absent");
        }
    }
    fflush(stdout);
}

/* An LDT data descriptor for the fake TEB; returns its selector, or 0. */
static uint16_t ldt_teb_selector(void)
{
    const uint32_t base = low32(low + LOW_TEB);
    struct user_segment_descriptor d;
    memset(&d, 0, sizeof(d));
    d.sd_lolimit = 0xfff;
    d.sd_lobase = base & 0xffffff;
    d.sd_hibase = base >> 24;
    d.sd_type = SDT_MEMRWA;
    d.sd_dpl = SEL_UPL;
    d.sd_p = 1;
    d.sd_def32 = 1;
    struct i386_ldt_args args = { LDT_AUTO_ALLOC, &d, 1 };
    int index = sysarch(I386_SET_LDT, &args);
    printf("M0 ldt set_ldt=%d errno=%d\n", index, index < 0 ? errno : 0);
    if (index < 0) {
        struct i386_ldt_args fixed = { 1, &d, 1 };
        index = sysarch(I386_SET_LDT, &fixed);
        printf("M0 ldt set_ldt_index1=%d errno=%d\n", index, index < 0 ? errno : 0);
        struct user_segment_descriptor current[2];
        struct i386_ldt_args get = { 0, current, 2 };
        const int got = sysarch(I386_GET_LDT, &get);
        printf("M0 ldt get_ldt=%d errno=%d\n", got, got < 0 ? errno : 0);
    }
    fflush(stdout);
    return index < 0 ? 0 : (uint16_t)LSEL((unsigned)index, SEL_UPL);
}

/* 32-bit code at `at`: mov eax,sel; mov fs,eax; mov eax,fs:[0x18]; mov [R+20],eax; [mov eax,[0]]; ljmp 0x43:B */
static void emit_ldt_code(unsigned at, uint16_t selector, int fault)
{
    uint8_t *p = low + LOW_CODE + at;
    const uint32_t sel = selector, result5 = low32(low + LOW_RESULTS + 20), back = low32(low + LOW_BACK64);
    *p++ = 0xb8; memcpy(p, &sel, 4); p += 4;
    *p++ = 0x8e; *p++ = 0xe0;
    *p++ = 0x64; *p++ = 0xa1; *p++ = 0x18; *p++ = 0; *p++ = 0; *p++ = 0;
    *p++ = 0xa3; memcpy(p, &result5, 4); p += 4;
    if (fault) { *p++ = 0xa1; *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 0; }
    *p++ = 0xea; memcpy(p, &back, 4); p += 4; *p++ = CS64; *p++ = 0;
}

static void mode_ldt(int fault)
{
    sysarch(AMD64_GET_FSBASE, &host_fsbase);
    memset(low + LOW_TEB, 0, 0x1000);
    const uint32_t self = low32(low + LOW_TEB);
    memcpy(low + LOW_TEB + 0x18, &self, 4);
    const uint16_t selector = ldt_teb_selector();
    if (!selector) return;
    emit_ldt_code(0xc0, selector, fault);
    if (fault) install_handler();
    printf("M0 ldt%s selector=%#x entering\n", fault ? "signal" : "", selector);
    fflush(stdout);
    if (sigsetjmp(recover, 1) == 0) {
        enter_at(0xc0);
        sysarch(AMD64_SET_FSBASE, &host_fsbase);      /* FS still holds the LDT selector's base here */
        printf("M0 ldt%s teb_self_read=%#x want=%#x%s\n", fault ? "signal" : "", result(5), self,
               fault ? " no_fault (unexpected)" : "");
    } else {
        printf("M0 ldtsignal recovered signal=%d address=%#lx teb_self_read=%#x want=%#x\n", (int)caught_signal,
               (unsigned long)caught_address, result(5), self);
    }
    fflush(stdout);
}

/* The raw entry: the kernel's signal trampoline calls it with (sig, info, ucontext) on the alternate
 * stack, FS as the interrupted code left it. It restores the host FS through libkernel_sys's sysarch
 * (which works with a guest FS loaded), runs the C handler, puts the guest FS back, and returns to the
 * trampoline, which calls sigreturn. */
uint64_t m0_guest_fsbase;
void m0_raw_entry(int sig, siginfo_t *info, void *uc);
void m0_raw_handler(int sig, siginfo_t *info, void *uc);
__asm__(".text\n"
        ".globl m0_raw_entry\n"
        "m0_raw_entry:\n"
        "  pushq %rbx\n  pushq %r12\n  pushq %r13\n"     /* 3 pushes + return address: 16-byte aligned */
        "  movl %edi, %ebx\n  movq %rsi, %r12\n  movq %rdx, %r13\n"
        "  movl $129, %edi\n  leaq host_fsbase(%rip), %rsi\n  call sysarch\n"
        "  movl %ebx, %edi\n  movq %r12, %rsi\n  movq %r13, %rdx\n  call m0_raw_handler\n"
        "  movl $129, %edi\n  leaq m0_guest_fsbase(%rip), %rsi\n  call sysarch\n"
        "  popq %r13\n  popq %r12\n  popq %rbx\n"
        "  ret\n");

static volatile int raw_calls, raw_rip_offset = -1, raw_fsbase_offset = -1, raw_cs_offset = -1;
static uint64_t raw_fault_rip;

void m0_raw_handler(int sig, siginfo_t *info, void *uc)
{
    raw_calls++;
    caught_signal = sig;
    uint8_t *u = uc;
    if (raw_calls == 1) {     /* the console's ucontext, word by word, to read its layout from */
        printf("M0 rawsig handler signal=%d address=%#lx\n", sig, (unsigned long)info->si_addr);
        for (int off = 0; off < 512; off += 64) {
            printf("M0 rawsig uc+%03d", off);
            for (int k = 0; k < 64; k += 8) { uint64_t v; memcpy(&v, u + off + k, 8); printf(" %llx", (unsigned long long)v); }
            printf("\n");
        }
        fflush(stdout);
    }
    if (raw_calls > 3) { line("M0 rawsig looping: stopping\n"); _exit(7); }
    for (int off = 0; off + 8 <= 1024; off += 8) {
        uint64_t v;
        memcpy(&v, u + off, 8);
        uint64_t next = 0;
        if (off + 16 <= 1024) memcpy(&next, u + off + 8, 8);
        /* The saved RIP is the copy of the faulting address followed by the saved CS. */
        if (v == raw_fault_rip && next == CS32 && raw_rip_offset < 0) raw_rip_offset = off;
        if (v == m0_guest_fsbase && raw_fsbase_offset < 0) raw_fsbase_offset = off;
        if (v == CS32 && raw_cs_offset < 0 && raw_rip_offset >= 0 && off > raw_rip_offset) raw_cs_offset = off;
    }
    if (raw_rip_offset < 0) { line("M0 rawsig no_rip_in_ucontext: stopping\n"); _exit(8); }
    if (raw_rip_offset >= 0) {          /* resume after the 5-byte faulting load */
        uint64_t rip;
        memcpy(&rip, u + raw_rip_offset, 8);
        rip += 5;
        memcpy(u + raw_rip_offset, &rip, 8);
    }
}

static void mode_rawsig(void)
{
    sysarch(AMD64_GET_FSBASE, &host_fsbase);
    static uint8_t alternate[65536];
    stack_t ss = { .ss_sp = alternate, .ss_size = sizeof(alternate) };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = m0_raw_entry;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    line("M0 rawsig registering through system call 416\n");
    const long registered = syscall(416, SIGSEGV, &sa, NULL);
    printf("M0 rawsig registered=%ld errno=%d\n", registered, registered ? errno : 0);
    fflush(stdout);
    if (registered) return;
    memset(low + LOW_TEB, 0, 0x1000);
    const uint32_t self = low32(low + LOW_TEB);
    memcpy(low + LOW_TEB + 0x18, &self, 4);
    m0_guest_fsbase = self;
    raw_fault_rip = low32(low + LOW_CODE + AT_FAULT_FS + FAULT_FS_LOAD_AT);
    line("M0 rawsig entering with the guest FS\n");
    if (sysarch(AMD64_SET_FSBASE, &m0_guest_fsbase)) { line("M0 rawsig set_fsbase_failed\n"); return; }
    enter_at(AT_FAULT_FS);
    sysarch(AMD64_SET_FSBASE, &host_fsbase);
    printf("M0 rawsig resumed calls=%d signal=%d teb_self_read=%#x want=%#x rip_offset=%d fsbase_offset=%d cs_offset=%d\n",
           raw_calls, (int)caught_signal, result(4), self, raw_rip_offset, raw_fsbase_offset, raw_cs_offset);
    fflush(stdout);
}

static void mode_stop(pid_t pid)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
    /* The console's kinfo_proc is larger than the SDK's: room for it, the SDK's fields read from its start. */
    static union { struct kinfo_proc info; char room[8192]; } record;
    size_t bytes = sizeof(record);
    memset(&record, 0, sizeof(record));
    const int found = sysctl(mib, 4, &record, &bytes, NULL, 0) == 0 && bytes > 0;
    const int ours = found && !strcmp(record.info.ki_comm, "m0") && pid != getpid();
    const int killed = ours ? kill(pid, SIGKILL) : -1;
    printf("M0 stop pid=%d found=%d name=%s killed=%d\n", (int)pid, found, found ? record.info.ki_comm : "-", killed);
    fflush(stdout);
}

static void mode_fsselector(void)
{
    sysarch(AMD64_GET_FSBASE, &host_fsbase);
    install_handler();
    line("M0 fsselector loading fs=0x13 in 32-bit code\n");
    if (sigsetjmp(recover, 1) == 0) {
        enter_at(AT_FSREAD);
        uint16_t fs;
        __asm__ volatile("movw %%fs,%0" : "=r"(fs));
        uint64_t base = 0;
        sysarch(AMD64_GET_FSBASE, &base);
        sysarch(AMD64_SET_FSBASE, &host_fsbase);
        printf("M0 fsselector read=%#x fs_after=%#x fsbase_after=%#llx host=%#llx\n", result(3), fs,
               (unsigned long long)base, (unsigned long long)host_fsbase);
    } else {
        printf("M0 fsselector faulted signal=%d address=%#lx\n", (int)caught_signal, (unsigned long)caught_address);
    }
    fflush(stdout);
}

enum { MARK_GUEST = 0x6e57f500, MARK_STALE = 0x57a1e000 };

static const char *fs_marker(uint32_t value)
{
    return value == MARK_GUEST ? "guest" : value == MARK_STALE ? "stale_host" : "other";
}

/* One fsreload case: set the guest base through `op`, optionally spin, run fsread (mov fs,0x13; mov eax,fs:[0])
 * in 32-bit code, then put the host base back before anything reads %fs. */
static void fsreload_case(const char *name, int op, uint64_t guest, uint64_t host, int spin_ms)
{
    uint32_t guest32 = (uint32_t)guest;
    memset(low + LOW_RESULTS, 0, 16);
    const int set = op == I386_SET_FSBASE ? sysarch(I386_SET_FSBASE, &guest32) : sysarch(AMD64_SET_FSBASE, &guest);
    if (spin_ms) {
        const double until = now_ns() + spin_ms * 1e6;
        while (now_ns() < until) {}
    }
    uint64_t before = 0;
    if (!set) enter_at(AT_FSREAD);
    sysarch(AMD64_GET_FSBASE, &before);
    const int back = sysarch(AMD64_SET_FSBASE, &host);
    const uint32_t read = result(3);
    printf("M0 fsreload %s set=%d read=%#x (%s) fsbase_after=%#llx restored=%d\n", name, set, read,
           set ? "not_run" : fs_marker(read), (unsigned long long)before, back);
    fflush(stdout);
}

static void mode_fsreload(void)
{
    uint64_t host = 0;
    sysarch(AMD64_GET_FSBASE, &host);
    const uintptr_t stale = (uint32_t)host, page = stale & ~(uintptr_t)0x3fff;
    void *mapped = mmap((void *)page, 0x8000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    printf("M0 fsreload host=%#llx stale=%#lx page=%p mapped=%p\n", (unsigned long long)host, (unsigned long)stale,
           (void *)page, mapped);
    fflush(stdout);
    if (mapped != (void *)page) { line("M0 fsreload stale_page_unavailable\n"); return; }
    const uint32_t mark_stale = MARK_STALE, mark_guest = MARK_GUEST;
    memcpy((void *)stale, &mark_stale, 4);
    memset(low + LOW_TEB, 0, 0x1000);
    memcpy(low + LOW_TEB, &mark_guest, 4);
    const uint64_t guest = low32(low + LOW_TEB);
    fsreload_case("amd64", AMD64_SET_FSBASE, guest, host, 0);
    fsreload_case("amd64_spin", AMD64_SET_FSBASE, guest, host, 20);
    fsreload_case("i386", I386_SET_FSBASE, guest, host, 0);
    fsreload_case("i386_spin", I386_SET_FSBASE, guest, host, 20);
    fsreload_case("amd64_after_i386", AMD64_SET_FSBASE, guest, host, 0);
}

enum { MARK_ALIAS0 = 0xa11a5000, MARK_ALIAS_SELF = 0xa11a5018 };

static uint64_t fs_self(void)
{
    uint64_t self;
    __asm__ volatile("movq %%fs:0,%0" : "=r"(self));
    return self;
}

static void mode_fsalias(void)
{
    uint64_t host = 0, after = 0;
    sysarch(AMD64_GET_FSBASE, &host);
    const uintptr_t alias = (uint32_t)host, page = alias & ~(uintptr_t)0x3fff;
    void *mapped = mmap((void *)page, 0x8000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    printf("M0 fsalias host=%#llx alias=%#lx mapped=%p fs_self=%#llx\n", (unsigned long long)host, (unsigned long)alias,
           mapped, (unsigned long long)fs_self());
    fflush(stdout);
    if (mapped != (void *)page) { line("M0 fsalias alias_page_unavailable\n"); return; }
    const uint32_t mark0 = MARK_ALIAS0, mark_self = MARK_ALIAS_SELF;
    memcpy((void *)alias, &mark0, 4);
    memcpy((void *)(alias + 0x18), &mark_self, 4);
    memset(low + LOW_RESULTS, 0, 32);
    enter_at(AT_TEB_READ);                      /* mov eax,fs:[0x18] with the 64-bit FS base as it is */
    const uint32_t no_reload = result(4);
    printf("M0 fsalias no_reload=%#x (%s)\n", no_reload, no_reload == MARK_ALIAS_SELF ? "alias" :
           no_reload == (uint32_t)(host + 0x18) ? "host_tcb" : "other");
    fflush(stdout);
    enum { ROUNDS = 100000 };
    const double t0 = now_ns();
    for (unsigned i = 0; i < ROUNDS; i++) enter_at(AT_TEB_READ);
    const double t1 = now_ns();
    printf("M0 fsalias round_trip_ns=%.1f rounds=%u read=%#x\n", (t1 - t0) / ROUNDS, ROUNDS, result(4));
    fflush(stdout);
    /* Reloading 0x13 in 32-bit code gives FS the descriptor's 32-bit base, which stays in 64-bit mode: %fs:0 then
     * is not the thread pointer, which is how an entry can tell, and one sysarch puts the host base back. Nothing
     * between the reload and that sysarch may read thread-local storage. */
    enter_at(AT_FSREAD);                        /* mov fs,0x13; mov eax,fs:[0] */
    const uint64_t self_reloaded = fs_self();
    const int fixed = sysarch(AMD64_SET_FSBASE, &host);
    const uint64_t self_fixed = fs_self();
    const uint32_t reload = result(3);
    sysarch(AMD64_GET_FSBASE, &after);
    printf("M0 fsalias reload=%#x (%s) fs_self_after_reload=%#llx fixed=%d fs_self_after_fix=%#llx fsbase_kept=%d\n",
           reload, reload == MARK_ALIAS0 ? "alias" : "other", (unsigned long long)self_reloaded, fixed,
           (unsigned long long)self_fixed, after == host);
    fflush(stdout);
}

enum { TCB_THREADS = 16 };
static uint64_t tcb_base[TCB_THREADS];
static volatile int tcb_ready, tcb_release;

static void *tcb_thread(void *arg)
{
    const unsigned i = (unsigned)(uintptr_t)arg;
    sysarch(AMD64_GET_FSBASE, &tcb_base[i]);
    __atomic_add_fetch(&tcb_ready, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&tcb_release, __ATOMIC_SEQ_CST)) usleep(1000);
    return NULL;
}

static int compare_u64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void mode_tcbs(void)
{
    pthread_t threads[TCB_THREADS];
    uint64_t main_base = 0, sorted[TCB_THREADS];
    unsigned started = 0;
    sysarch(AMD64_GET_FSBASE, &main_base);
    for (unsigned i = 0; i < TCB_THREADS; i++)
        if (!pthread_create(&threads[i], NULL, tcb_thread, (void *)(uintptr_t)i)) started++;
    while (__atomic_load_n(&tcb_ready, __ATOMIC_SEQ_CST) < (int)started) usleep(1000);
    memcpy(sorted, tcb_base, sizeof(sorted));
    qsort(sorted, started, sizeof(sorted[0]), compare_u64);
    uint64_t stride_min = ~0ull, stride_max = 0;
    for (unsigned i = 1; i < started; i++) {
        const uint64_t d = sorted[i] - sorted[i - 1];
        if (d < stride_min) stride_min = d;
        if (d > stride_max) stride_max = d;
    }
    printf("M0 tcbs main=%#llx threads=%u lowest=%#llx highest=%#llx stride_min=%#llx stride_max=%#llx\n",
           (unsigned long long)main_base, started, (unsigned long long)sorted[0],
           (unsigned long long)sorted[started ? started - 1 : 0], (unsigned long long)stride_min,
           (unsigned long long)stride_max);
    for (unsigned i = 0; i < started; i++) printf("M0 tcbs thread=%u fsbase=%#llx\n", i, (unsigned long long)tcb_base[i]);
    fflush(stdout);
    __atomic_store_n(&tcb_release, 1, __ATOMIC_SEQ_CST);
    for (unsigned i = 0; i < started; i++) pthread_join(threads[i], NULL);
    line("M0 tcbs joined\n");
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[argc - 1] : "ident";
    for (int i = 0; i < argc; i++) if (!strncmp(argv[i], "mode=", 5)) mode = argv[i] + 5;
    printf("M0 start mode=%s\n", mode);
    fflush(stdout);
    if (!strcmp(mode, "ident")) { mode_ident(); return 0; }
    if (!strcmp(mode, "low4g")) { mode_low4g(); return 0; }
    if (!strcmp(mode, "stop")) {
        for (int i = 0; i < argc; i++) if (!strncmp(argv[i], "pid=", 4)) mode_stop((pid_t)atoi(argv[i] + 4));
        return 0;
    }
    if (low_page()) { line("M0 no_low_page\n"); return 1; }
    printf("M0 low_page at=%p\n", (void *)low);
    fflush(stdout);
    low_install();
    if (!strcmp(mode, "switch")) mode_switch();
    else if (!strcmp(mode, "fs")) mode_fs();
    else if (!strcmp(mode, "signal32")) mode_signal32(0);
    else if (!strcmp(mode, "signal32fs")) mode_signal32(1);
    else if (!strcmp(mode, "fsselector")) mode_fsselector();
    else if (!strcmp(mode, "fsguest")) mode_fsguest();
    else if (!strcmp(mode, "sigpath")) mode_sigpath();
    else if (!strcmp(mode, "signal32fsraw")) { raw_registration = 1; mode_signal32(1); }
    else if (!strcmp(mode, "ldt")) mode_ldt(0);
    else if (!strcmp(mode, "ldtsignal")) mode_ldt(1);
    else if (!strcmp(mode, "rawsig")) mode_rawsig();
    else if (!strcmp(mode, "fsreload")) mode_fsreload();
    else if (!strcmp(mode, "fsalias")) mode_fsalias();
    else if (!strcmp(mode, "tcbs")) mode_tcbs();
    else line("M0 unknown_mode\n");
    line("M0 end\n");
    return 0;
}
