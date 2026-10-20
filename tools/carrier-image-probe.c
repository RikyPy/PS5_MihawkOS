/* SPDX-License-Identifier: GPL-3.0-or-later
 * Can a payload helper give back the address range its carrier's executable holds? (docs/IA32_DESIGN.md, the
 * fixed-base PE32 gap.) websrv's elfldr starts every payload inside a fresh /system/vsh/app/NPXS40112/eboot.bin,
 * which the kernel places at 0x400000 (a carrier crash dump's module list: xotext 0x400000:0x454000), and maps our
 * position-independent ELF at 0x200000000. The carrier's own code never runs: elfldr takes the process over at exec.
 * A 32-bit installer linked /fixed at 0x400000 needs exactly that range.
 *
 *   mode=list     read only: every mapping below 4 GiB (sceKernelVirtualQuery), every module libkernel knows
 *                 (sceKernelGetModuleList/Info) and which one holds 0x400000; then the after-checks. The control.
 *   mode=stop pid=N  ends a probe left running (a load that never returned): only a process named "carrier", never
 *                 itself (as tools/m0-native32-probe.c's stop).
 *   mode=xo-control, xo-text, xo-rest, xo-forget  why 32-bit programs at 0x400000 were stopped (fixed-base32-2026-10-19-01,
 *                 SYSTEM_XO_VIOLATION): after the release, three first touches under a SIGSEGV/SIGBUS handler, each on a
 *                 page it maps: a read of fresh anonymous memory, a read of a file mapping, and a JIT page made as Wine
 *                 makes executable memory (contents written, read-only, JIT object, mapped over, read+execute), run
 *                 then read. control at 0x590000 (outside the old image), text in its old execute-only segment
 *                 (0x400000-0x454000), rest in its other segments (0x454000-0x584000); forget asks libkernel to unload
 *                 the stale eboot.bin record (handle 0) and reads 0x400000 only if the record went. An ordinary fault
 *                 prints "fault"; the kernel's debug stop ends the process after the line naming the step.
 *   mode=release  the same, but first unmaps that module's segments; after the after-checks lists what lies below
 *                 4 GiB (a module load must not land in the freed range: Wine has not reserved it yet), then maps
 *                 the span at 0x400000 read-write without replacing anything and reads it back.
 *
 * The after-checks are what a helper does once Wine runs: a thread that ends in pthread_exit (libkernel's TLS setup
 * and exit walk every module), a signal, the helper's own first load (ntdll.prx, or module=PATH; libSceSystemService.sprx
 * never returned from its load in a payload, carrier-image-2026-10-19-01), the module list again. tools/run-carrier-image-probe.py
 * compares them between the two modes. Every step prints its "CARRIER" line before it acts, so a step that ends the
 * process is the last line. No fork, no /dev/dmem, no protection change, no kernel write: munmap and mmap of this
 * process's own map only, never MAP_FIXED, and nothing above 4 GiB is unmapped. Ends with _exit(0), as helpers do.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>
#include "pw_vm_census.h"
#include "pw_wine_prx.h"

int sceKernelVirtualQuery(const void *address, int flags, void *info, size_t size);
int sceKernelJitCreateSharedMemory(const char *name, size_t bytes, int max_protection, int *fd);
int sceKernelMmap(void *address, size_t bytes, int protection, int flags, int fd, off_t offset, void **out);
int sceKernelMprotect(const void *address, size_t bytes, int protection);
int sceKernelQueryMemoryProtection(void *address, void **start, void **end, uint32_t *protection);
int sceKernelStopUnloadModule(int32_t handle, size_t argc, const void *argv, uint32_t flags, const void *option,
                              int *result);
int sceKernelGetModuleList(int32_t *handles, size_t count, size_t *actual);
int sceKernelGetModuleInfo(int32_t handle, void *info);
int32_t sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags, const void *option,
                                 int *result);

/* Linked with -Wl,--wrap for both (tools/run-carrier-image-probe.py checks the disassembly): the SDK's grant of
 * execute writes the kernel's vm_map entry without its lock; this probe asks for no execute, and any call is refused. */
int __wrap_kernel_mprotect(int pid, unsigned long address, unsigned long bytes, int protection)
{ (void)pid; (void)address; (void)bytes; (void)protection; errno = EPERM; return -1; }
int __wrap_kernel_set_vmem_protection(int pid, unsigned long address, unsigned long bytes, int protection)
{ (void)pid; (void)address; (void)bytes; (void)protection; errno = EPERM; return -1; }

enum { MODULES = 256, PAGE = 0x4000 };
#define LOW4G UINT64_C(0x100000000)
#define CARRIER_BASE UINT64_C(0x400000)

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...)
{
    va_list list;
    va_start(list, format);
    vprintf(format, list);
    va_end(list);
    fflush(stdout);
}

/* Every mapping below 4 GiB, and the first one above. */
static void list_mappings(const char *phase)
{
    uint8_t raw[PW_VM_RECORD];
    PwVmRecord record;
    unsigned count = 0;
    for (uintptr_t at = 0; count < 512; count++) {
        memset(raw, 0, sizeof(raw));
        const int status = sceKernelVirtualQuery((const void *)at, 1, raw, sizeof(raw));
        if (status) { say("CARRIER map %s end status=%#x after=%u\n", phase, (unsigned)status, count); return; }
        pw_vm_record_decode(raw, &record);
        say("CARRIER map %s %s start=%#lx end=%#lx prot=%#x type=%d flags=%#x name=%s\n", phase,
            record.start >= LOW4G ? "above" : "low", (unsigned long)record.start, (unsigned long)record.end,
            (unsigned)record.protection, record.type, record.flags, record.name);
        if (record.start >= LOW4G || record.end <= at) return;
        at = record.end;
    }
}

/* Every module libkernel knows; the one with a segment at CARRIER_BASE into carrier. */
static int list_modules(const char *phase, PwPrxSegment carrier[PW_PRX_MAX_SEGMENTS], uint32_t *carrier_count)
{
    int32_t handles[MODULES];
    size_t actual = 0;
    const int status = sceKernelGetModuleList(handles, MODULES, &actual);
    say("CARRIER modules %s status=%#x count=%zu\n", phase, (unsigned)status, actual);
    if (status) return -1;
    int found = -1;
    for (size_t i = 0; i < actual && i < MODULES; i++) {
        union { uint64_t size; unsigned char bytes[PW_PRX_MODULE_INFO_BYTES]; } info = {0};
        char name[PW_PRX_MAX_NAME] = "";
        PwPrxSegment segments[PW_PRX_MAX_SEGMENTS];
        uint32_t count = 0;
        info.size = sizeof(info.bytes);
        const int got = sceKernelGetModuleInfo(handles[i], info.bytes);
        if (got < 0 || pw_prx_parse_module_info(info.bytes, name, segments, &count)) {
            say("CARRIER module %s handle=%d info=%#x\n", phase, handles[i], (unsigned)got);
            continue;
        }
        char line[512];
        size_t at = (size_t)snprintf(line, sizeof(line), "CARRIER module %s handle=%d name=%s", phase, handles[i], name);
        for (uint32_t s = 0; s < count && at < sizeof(line); s++) {
            const uintptr_t start = (uintptr_t)segments[s].address;
            at += (size_t)snprintf(line + at, sizeof(line) - at, " seg=%#lx+%#x/%#x", (unsigned long)start,
                                   segments[s].size, segments[s].protection);
            if (start <= CARRIER_BASE && CARRIER_BASE < start + segments[s].size && found < 0) {
                found = (int)i;
                memcpy(carrier, segments, sizeof(segments));
                *carrier_count = count;
            }
        }
        say("%s\n", line);
    }
    say("CARRIER carrier %s module=%d\n", phase, found);
    return found;
}

static uintptr_t low = UINTPTR_MAX, high;   /* the released span */

/* Gives back the carrier module's segments. 0 when every one went. */
static int release(const PwPrxSegment *segments, uint32_t count)
{
    for (uint32_t s = 0; s < count; s++) {
        const uintptr_t start = (uintptr_t)segments[s].address & ~(uintptr_t)(PAGE - 1);
        const uintptr_t end = ((uintptr_t)segments[s].address + segments[s].size + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
        if (end > LOW4G || end <= start) { say("CARRIER release refused seg=%#lx-%#lx\n", (unsigned long)start,
                                                (unsigned long)end); return -1; }
        low = start < low ? start : low;
        high = end > high ? end : high;
    }
    if (low != CARRIER_BASE) { say("CARRIER release refused low=%#lx\n", (unsigned long)low); return -1; }
    int failed = 0;
    for (uint32_t s = 0; s < count; s++) {
        const uintptr_t start = (uintptr_t)segments[s].address & ~(uintptr_t)(PAGE - 1);
        const uintptr_t end = ((uintptr_t)segments[s].address + segments[s].size + PAGE - 1) & ~(uintptr_t)(PAGE - 1);
        say("CARRIER unmap seg=%#lx-%#lx\n", (unsigned long)start, (unsigned long)end);
        errno = 0;
        const int status = munmap((void *)start, end - start);
        say("CARRIER unmapped seg=%#lx status=%d errno=%d\n", (unsigned long)start, status, status ? errno : 0);
        failed |= status != 0;
    }
    list_mappings("released");
    return failed ? -1 : 0;
}

/* Takes the released span at CARRIER_BASE read-write, as Wine's reservation of its low area would. 0 when it held. */
static int take_low(void)
{
    const size_t bytes = high - low;
    say("CARRIER map-at %#lx bytes=%#zx\n", (unsigned long)low, bytes);
    errno = 0;
    unsigned char *taken = mmap((void *)low, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    const int error = taken == MAP_FAILED ? errno : 0;
    say("CARRIER mapped-at want=%#lx got=%p errno=%d\n", (unsigned long)low, (void *)taken, error);
    if (taken == MAP_FAILED) return -1;
    if ((uintptr_t)taken != low) { munmap(taken, bytes); return -1; }
    for (size_t i = 0; i < bytes; i += 4096) taken[i] = (unsigned char)(i >> 12);
    size_t wrong = 0;
    for (size_t i = 0; i < bytes; i += 4096) wrong += taken[i] != (unsigned char)(i >> 12);
    say("CARRIER readback bytes=%#zx wrong=%zu\n", bytes, wrong);
    return wrong ? -1 : 0;
}

static void *thread_main(void *argument)
{
    pthread_exit((void *)((uintptr_t)argument + 1));
}

static volatile sig_atomic_t signalled;
static void on_signal(int number) { signalled = number; }

/* The same in both modes; the runner compares their results. */
static void after_checks(const char *module)
{
    say("CARRIER thread start\n");
    pthread_t thread;
    void *value = NULL;
    int status = pthread_create(&thread, NULL, thread_main, (void *)41);
    if (!status) status = pthread_join(thread, &value);
    say("CARRIER thread status=%d value=%lu\n", status, (unsigned long)(uintptr_t)value);

    say("CARRIER signal start\n");
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    status = sigaction(SIGUSR1, &action, NULL);
    if (!status) status = raise(SIGUSR1);
    say("CARRIER signal status=%d delivered=%d\n", status, signalled == SIGUSR1);

    say("CARRIER load start path=%s\n", module);
    int result = 0;
    const int32_t handle = sceKernelLoadStartModule(module, 0, NULL, 0, NULL, &result);
    say("CARRIER load handle_ok=%d status=%#x\n", handle >= 0, handle >= 0 ? 0u : (unsigned)handle);

    PwPrxSegment unused[PW_PRX_MAX_SEGMENTS];
    uint32_t count = 0;
    list_modules("after", unused, &count);
}

static sigjmp_buf fault_jump;
static volatile sig_atomic_t fault_signo, fault_code;
static volatile uintptr_t fault_address;
static void on_fault(int number, siginfo_t *info, void *context)
{
    (void)context;
    fault_signo = number;
    fault_code = info->si_code;
    fault_address = (uintptr_t)info->si_addr;
    siglongjmp(fault_jump, 1);
}

/* One read of a byte at address, or a call of the code there, under a fault handler. */
static void guarded(const char *what, uintptr_t at, int call)
{
    struct sigaction action, old_segv, old_bus;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, &old_segv);
    sigaction(SIGBUS, &action, &old_bus);
    say("CARRIER xo %s at=%#lx\n", what, (unsigned long)at);
    if (!sigsetjmp(fault_jump, 1)) {
        const unsigned value = call ? ((unsigned (*)(void))at)() : *(volatile unsigned char *)at;
        say("CARRIER xo %s ok value=%#x\n", what, value);
    } else {
        say("CARRIER xo %s fault signo=%d code=%d addr=%#lx\n", what, (int)fault_signo, (int)fault_code,
            (unsigned long)fault_address);
    }
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
}

static unsigned char *page_at(uintptr_t at, int protection, int flags, int fd, const char *what)
{
    errno = 0;
    unsigned char *page = mmap((void *)at, PAGE, protection, flags, fd, 0);
    say("CARRIER xo %s map want=%#lx got=%p errno=%d\n", what, (unsigned long)at, (void *)page,
        page == MAP_FAILED ? errno : 0);
    if (page == MAP_FAILED) return NULL;
    if ((uintptr_t)page != at) { munmap(page, PAGE); return NULL; }
    return page;
}

static void xo_anon(uintptr_t at)
{
    if (page_at(at, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, "anon")) guarded("anon-read", at, 0);
}

static void xo_file(uintptr_t at)
{
    const int fd = open("/data/ps5proton/runtime/ge11-7/share/wine/wine.inf", O_RDONLY);
    if (fd < 0) { say("CARRIER xo file open errno=%d\n", errno); return; }
    unsigned char *page = page_at(at, PROT_READ, MAP_PRIVATE, fd, "file");
    close(fd);
    if (page) guarded("file-read", at, 0);
}

/* As __wine_ps5_exec_convert does (wine/ps5/pw_wine_dmem_ps5.c jit_over, then payload_protect and the query). */
static void xo_jit(uintptr_t at)
{
    static const unsigned char code[] = { 0xb8, 0x34, 0x12, 0x00, 0x00, 0xc3 };   /* mov eax, 0x1234; ret */
    unsigned char *page = page_at(at, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, "jit");
    if (!page) return;
    memcpy(page, code, sizeof(code));
    int fd = -1;
    void *over = NULL, *start = NULL, *end = NULL;
    uint32_t protection = 0;
    const int readonly = sceKernelMprotect(page, PAGE, PROT_READ);
    const int created = sceKernelJitCreateSharedMemory("pw-jit", PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, &fd);
    void *scratch = created ? MAP_FAILED : mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (scratch != MAP_FAILED) { memcpy(scratch, page, PAGE); munmap(scratch, PAGE); }
    const int mapped = scratch == MAP_FAILED ? -1 :
        sceKernelMmap(page, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0, &over);
    if (fd >= 0) close(fd);
    const int executable = mapped ? -1 : sceKernelMprotect(page, PAGE, PROT_READ | PROT_EXEC);
    const int queried = sceKernelQueryMemoryProtection(page, &start, &end, &protection);
    say("CARRIER xo jit steps readonly=%#x create=%#x scratch=%d map=%#x protect=%#x query=%#x prot=%#x\n",
        (unsigned)readonly, (unsigned)created, scratch != MAP_FAILED, (unsigned)mapped, (unsigned)executable,
        (unsigned)queried, protection);
    if (executable || queried || (protection & 5) != 5) return;
    guarded("jit-exec", at, 1);   /* Wine's code runs first, and is read later */
    guarded("jit-read", at, 0);
}

static void xo_tests(uintptr_t base)
{
    xo_anon(base);
    xo_file(base + 0x10000);
    xo_jit(base + 0x20000);
}

/* 32-bit execution at the released address: the one step carrier-xo-2026-10-19-01 did not test, and the last thing a
 * fixed-base 32-bit program does before the stop (docs/IA32_DESIGN.md). The code page is made as Wine makes executable
 * memory (xo_jit's steps: anon contents, JIT object mapped over the same address, RX after mprotect, checked by query),
 * not by an SDK execute grant. The far pointer, the results and the 32-bit stack sit in an ordinary read-write page
 * below 4 GiB; the 64-bit return stub is in the code page and jumps to xo32_back64. The 32-bit code is the M0 probe's
 * (tools/m0-native32-probe.c: mov cs/ss to results, 6*7 through a call, ljmp 0x43 back), which ran in a payload on
 * this console. The control runs first at 0x590000; only a low run that returns says 32-bit code runs at 0x400000. */
enum { XO32_DATA_OFF = 0x20, XO32_RESULTS_OFF = 0x200, XO32_STACK_TOP = 0x3000,
       XO32_CODE_OFF = 0x100, XO32_STUB_OFF = 0x40, XO32_CODE_BYTES = 0x31 };
static const unsigned char xo32_code[XO32_CODE_BYTES] = {
    0x8c, 0xc9, 0x89, 0x0d, 0x00, 0x11, 0x11, 0x11, 0x8c, 0xd0, 0xa3, 0x04, 0x11, 0x11, 0x11, 0xb8,
    0x06, 0x00, 0x00, 0x00, 0x50, 0xe8, 0x0f, 0x00, 0x00, 0x00, 0x83, 0xc4, 0x04, 0xa3, 0x08, 0x11,
    0x11, 0x11, 0xea, 0x00, 0x22, 0x22, 0x22, 0x43, 0x00, 0x8b, 0x44, 0x24, 0x04, 0x6b, 0xc0, 0x07,
    0xc3,
};
static uint8_t *xo32_data;
uint64_t xo32_saved_rsp;
void xo32_enter(void *farptr, uint32_t stack);
void xo32_back64(void);
__asm__(".text\n"
        ".globl xo32_enter\n"
        "xo32_enter:\n"
        "  pushq %rbx\n  pushq %rbp\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
        "  movq %rsp, xo32_saved_rsp(%rip)\n"
        "  movl %esi, %esp\n"
        "  ljmpl *(%rdi)\n"
        ".globl xo32_back64\n"
        "xo32_back64:\n"
        "  movq xo32_saved_rsp(%rip), %rsp\n"
        "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbp\n  popq %rbx\n"
        "  ret\n");

static uint32_t result32(unsigned n)
{
    uint32_t value;
    memcpy(&value, xo32_data + XO32_RESULTS_OFF + 4 * n, 4);
    return value;
}

/* The code page at `at`: the 32-bit code and the 64-bit stub, then JIT memory at the same address, RX. */
static unsigned char *xo32_code_page(uintptr_t at, const char *label)
{
    unsigned char *page = page_at(at, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, "xo32");
    if (!page) return NULL;
    memset(page, 0xcc, PAGE);
    memcpy(page + XO32_CODE_OFF, xo32_code, sizeof(xo32_code));
    for (size_t i = 0; i + 4 <= sizeof(xo32_code); i++) {
        uint32_t value;
        memcpy(&value, page + XO32_CODE_OFF + i, 4);
        if (value >= 0x11111100u && value < 0x11111120u)
            value = (uint32_t)(uintptr_t)(xo32_data + XO32_RESULTS_OFF) + (value - 0x11111100u);
        else if (value == 0x22222200u) value = (uint32_t)at + XO32_STUB_OFF;
        else continue;
        memcpy(page + XO32_CODE_OFF + i, &value, 4);
    }
    const uint64_t target = (uint64_t)(uintptr_t)xo32_back64;
    unsigned char *stub = page + XO32_STUB_OFF;   /* movabs rax, target; jmp rax */
    stub[0] = 0x48; stub[1] = 0xb8; memcpy(stub + 2, &target, 8); stub[10] = 0xff; stub[11] = 0xe0;

    int fd = -1;
    void *over = NULL, *start = NULL, *end = NULL;
    uint32_t protection = 0;
    const int readonly = sceKernelMprotect(page, PAGE, PROT_READ);
    const int created = sceKernelJitCreateSharedMemory("pw-xo32", PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, &fd);
    void *scratch = created ? MAP_FAILED : mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (scratch != MAP_FAILED) { memcpy(scratch, page, PAGE); munmap(scratch, PAGE); }
    const int mapped = scratch == MAP_FAILED ? -1 :
        sceKernelMmap(page, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0, &over);
    if (fd >= 0) close(fd);
    const int executable = mapped ? -1 : sceKernelMprotect(page, PAGE, PROT_READ | PROT_EXEC);
    const int queried = sceKernelQueryMemoryProtection(page, &start, &end, &protection);
    say("CARRIER xo32 %s jit readonly=%#x create=%#x scratch=%d map=%#x protect=%#x query=%#x prot=%#x\n", label,
        (unsigned)readonly, (unsigned)created, scratch != MAP_FAILED, (unsigned)mapped, (unsigned)executable,
        (unsigned)queried, protection);
    if (readonly || created || scratch == MAP_FAILED || mapped || executable || queried || (protection & 5) != 5)
        return NULL;
    return page;
}

/* One 32-bit run from the code page at `at`; 0 when it returned CS 0x33, SS 0x3b and 42. */
static int xo32_run(uintptr_t at, const char *label)
{
    unsigned char *code = xo32_code_page(at, label);
    if (!code) { say("CARRIER xo32 %s refused\n", label); return -1; }
    const uint32_t entry = (uint32_t)at + XO32_CODE_OFF, selector = 0x33;
    memcpy(xo32_data + XO32_DATA_OFF, &entry, 4);
    memcpy(xo32_data + XO32_DATA_OFF + 4, &selector, 2);
    memset(xo32_data + XO32_RESULTS_OFF, 0, 12);
    say("CARRIER xo32 %s enter code=%#lx farptr=%#lx stack=%#lx\n", label, (unsigned long)at,
        (unsigned long)(uintptr_t)(xo32_data + XO32_DATA_OFF),
        (unsigned long)(uintptr_t)(xo32_data + XO32_STACK_TOP));
    xo32_enter(xo32_data + XO32_DATA_OFF, (uint32_t)(uintptr_t)(xo32_data + XO32_STACK_TOP));
    const uint32_t cs = result32(0) & 0xffff, ss = result32(1) & 0xffff, value = result32(2);
    say("CARRIER xo32 %s returned cs=%#x ss=%#x value=%u want=42\n", label, cs, ss, value);
    return cs == 0x33 && ss == 0x3b && value == 42 ? 0 : -1;
}

/* An ordinary read-write page below 4 GiB for the far pointer, the results and the 32-bit stack. */
static int xo32_data_page(void)
{
    static const uintptr_t hints[] = { 0x10000000, 0x20000000, 0x30000000, 0x40000000 };
    for (unsigned i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        errno = 0;
        unsigned char *page = mmap((void *)hints[i], PAGE, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (page == MAP_FAILED) continue;
        if ((uintptr_t)page != hints[i]) { munmap(page, PAGE); continue; }
        memset(page, 0, PAGE);
        xo32_data = page;
        return 0;
    }
    return -1;
}

static void xo32_tests(void)
{
    if (xo32_data_page()) { say("CARRIER xo32 data refused errno=%d\n", errno); return; }
    say("CARRIER xo32 data=%#lx\n", (unsigned long)(uintptr_t)xo32_data);
    const int control = xo32_run(0x590000, "control");
    const int low = control == 0 ? xo32_run(0x400000, "low") : -2;
    say("CARRIER xo32 result control=%d low=%d\n", control, low);
}

static void stop(pid_t pid)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
    /* The console's kinfo_proc is larger than the SDK's: room for it, the SDK's fields read from its start. */
    static union { struct kinfo_proc info; char room[8192]; } record;
    size_t bytes = sizeof(record);
    const int found = sysctl(mib, 4, &record, &bytes, NULL, 0) == 0 && bytes > 0;
    const int ours = found && !strcmp(record.info.ki_comm, "carrier") && pid != getpid();
    const int killed = ours ? kill(pid, SIGKILL) : -1;
    say("CARRIER stop pid=%d found=%d name=%s killed=%d\n", (int)pid, found, found ? record.info.ki_comm : "-", killed);
}

int main(int argc, char **argv)
{
    const char *mode = "list", *module = "/data/ps5proton/runtime/ge11-7/lib/wine/x86_64-unix/ntdll.prx";
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "mode=", 5)) mode = argv[i] + 5;
        if (!strncmp(argv[i], "module=", 7)) module = argv[i] + 7;
    }
    if (!strcmp(mode, "stop")) {
        for (int i = 0; i < argc; i++) if (!strncmp(argv[i], "pid=", 4)) stop((pid_t)atoi(argv[i] + 4));
        _exit(0);
    }
    const int xo = !strncmp(mode, "xo-", 3), xo32 = !strcmp(mode, "xo32");
    if (xo && strcmp(mode, "xo-control") && strcmp(mode, "xo-text") && strcmp(mode, "xo-rest") &&
        strcmp(mode, "xo-forget")) { say("CARRIER bad mode=%s\n", mode); _exit(2); }
    const int releasing = xo || xo32 || !strcmp(mode, "release");
    if (!releasing && strcmp(mode, "list")) { say("CARRIER bad mode=%s\n", mode); _exit(2); }
    say("CARRIER start mode=%s pid=%d text=%p\n", mode, getpid(), (void *)main);
    list_mappings("before");
    PwPrxSegment carrier[PW_PRX_MAX_SEGMENTS];
    uint32_t count = 0;
    const int found = list_modules("before", carrier, &count);
    const int unmapped = releasing && found >= 0 && !release(carrier, count);
    if (xo32) {
        say("CARRIER xo32 released=%d\n", unmapped);
        if (!unmapped) _exit(0);
        xo32_tests();
        say("CARRIER end mode=%s\n", mode);
        _exit(0);
    }
    if (xo) {
        say("CARRIER xo released=%d\n", unmapped);
        if (!unmapped) _exit(0);
        if (!strcmp(mode, "xo-control")) xo_tests(0x590000);
        if (!strcmp(mode, "xo-text")) xo_tests(CARRIER_BASE);
        if (!strcmp(mode, "xo-rest")) xo_tests(0x458000);
        if (!strcmp(mode, "xo-forget")) {
            int result = 0;
            say("CARRIER xo forget unload handle=0\n");
            const int status = sceKernelStopUnloadModule(0, 0, NULL, 0, NULL, &result);
            say("CARRIER xo forget status=%#x result=%d\n", (unsigned)status, result);
            PwPrxSegment unused[PW_PRX_MAX_SEGMENTS];
            uint32_t left = 0;
            int32_t handles[MODULES];
            size_t listed = 0;
            /* Only a list that answered and names no module at 0x400000 says the record went. */
            if (!sceKernelGetModuleList(handles, MODULES, &listed) && list_modules("forgot", unused, &left) < 0)
                xo_anon(CARRIER_BASE);
            else say("CARRIER xo forget kept\n");
        }
        say("CARRIER end mode=%s\n", mode);
        _exit(0);
    }
    /* What a helper does next, before Wine maps anything low: then nothing of the system may sit below 4 GiB. */
    after_checks(module);
    list_mappings("after-load");
    if (releasing) say("CARRIER released ok=%d\n", unmapped && !take_low());
    say("CARRIER end mode=%s\n", mode);
    _exit(0);
}
