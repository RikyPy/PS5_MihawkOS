/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WINE_MMU4K_H
#define PW_WINE_MMU4K_H
#include <stddef.h>
#include <stdint.h>

/* The 4 KiB MMU (docs/MMU4K.md, sections 3 and 5-7): a program's stores beside read-only 4 KiB pages, run without a
 * signal each and faulting exactly where a CPU with 4 KiB pages would.
 *
 * Why: the console's host page is 16 KiB. Patch 1122 holds a host page read-only when its four Windows pages disagree
 * on write (a "mixed" page) and steps each store a 4 KiB page allows: two signals, 86 us a store
 * (vmp-tracker-2026-10-14-03), a Unity game in the earlier emulator experiment (removed) at 2.0-2.4 fps (2026-10-15; record removed from the tree; in git history). Reporting
 * the read-only neighbours early gave 56.1 fps but uploaded stale bytes over GPU images (2026-10-15):
 * Such an emulator needs the access violation at the store, before its bytes land (docs/MMU4K.md section 1).
 *
 * How: when such a store faults, Wine starts a run on the faulting thread (outside the signal handler, on a run stack
 * of this module's). The run holds Wine's virtual mutex (ops.lock) and executes the program's instructions one at a
 * time from its RIP: the CPU runs each one itself in a 32-byte slot, with only its memory operand rewritten to
 * [scratch], scratch holding the program's address (reads, plain read-write host pages) or the address of the same
 * memory in a writable alias of the host page (pw_wine_alias.h: stores to plain 4 KiB pages of a mixed host page).
 * Control flow, the stack (PUSH, POP, CALL, RET, LEAVE) and MOVS/STOS are emulated in C, the stores among them
 * classified the same way. A store touching a protected 4 KiB page is never run: the run ends before it and the
 * instruction runs again natively, where the kernel faults at it (I2), except an exactly-sized store that begins in a
 * plain page and reaches a protected one, which ends with a write fault at the first byte of the first protected page
 * (the straddle rule, C4). Everything the engine cannot run ends the run the same way: native execution decides.
 *
 * Exits, each counted (PwWineMmu4kStats.exits): idle (PW_WINE_MMU4K_IDLE instructions without an alias store: the
 * thread left the mixed pages; C2: staying pays while those instructions cost less than one re-entry, 45 us / ~40 ns),
 * budget, form (an instruction it does not run), protected, straddle, fault (a slot or a guarded read faulted: the
 * native retry raises it through Wine's path), fetch (the code is unreadable or in a page that may not be executed),
 * noalias, yield (another thread waits on the virtual mutex: always after progress, so RESUME). A run that changed
 * nothing ends in 1122's step (STEP, counted stepfall), never in a plain native retry, so a run always makes progress
 * (P3).
 *
 * Signals (docs/MMU4K.md P1-P4): the glue (patch 1134's signal_x86_64.c; the host test's twin) calls
 * pw_wine_mmu4k_signal first in its SIGSEGV, SIGBUS, SIGILL and SIGFPE handlers on a thread in a run. A fault in a
 * slot or a guarded routine is RECOVER (the run records it and goes on from its recovery point); the run's end is a
 * ud2 at pw_wine_mmu4k_exit_trap, which gives the program's registers back (RESUME, STEP or FAULT). While a run lasts
 * the glue blocks Wine's asynchronous signals (P4); the program's vector registers stay live in the CPU throughout,
 * so this translation unit is built -mgeneral-regs-only -ffreestanding -fno-builtin and the console build checks its
 * object for any vector or x87 operand and any undefined symbol outside pw_wine_mmu4k_slot.S, pw_ia32_decode_mode and
 * pw_ia32_access. Everything a run calls outside it (ops, the decoder) goes through pw_wine_mmu4k_call (P2), which
 * saves and restores the x87/SSE/AVX state (xsave64/xrstor64, mask XCR0 & 7) in the run's own area.
 *
 * Concurrency: runs are serialized by the virtual mutex, which also guards the slots and the decode cache (2048 entries
 * keyed by RIP, each compared with the instruction's bytes before use, so self-modifying code is safe). A run holds the
 * mutex from its start to its exit, so its table of classified host pages needs no emptying: a window goes back only
 * under the mutex (a release of a view's memory, docs/MMU4K.md section 8) or in the run's own classify (an eviction),
 * which page_of sees by the generation. Every PW_WINE_MMU4K_YIELD instructions the run reads how many threads wait on
 * the mutex (ops.waiters) and ends (yield) when one does: a run ends within PW_WINE_MMU4K_YIELD instructions of a
 * counted waiter, and one nobody waits for lasts up to PW_WINE_MMU4K_BUDGET (~0.3 ms by the estimate); a longer hold
 * is one step blocked in an op or a slot. Who takes the mutex next is not the run's to decide: the mutex is not fair,
 * and the thread's next store beside a read-only page comes back through the exit trap and a native fault (~80 us by
 * the estimates below) to virtual_handle_fault's trylock, which can beat the woken waiter, the more so on one core.
 * Measured on the host (tests/test_pw_wine_perf.c, 2026-10-16, Linux, 12 cores, 5 runs): a blocked thread took the
 * mutex first 0 times in 50 when its holder relocked at once, as the run did before at every 256 instructions (that
 * also emptied the table each time; in a 2026-10-15 run the census waited more than 160 s on this mutex), and
 * 49-50 times in 50 when it relocked 80 us later. Not measured: the console's mutex, and one core under its FIFO
 * scheduling (payload-vm-contention, docs/MMU4K.md section 12). Run stacks are taken and given atomically.
 * Stats are relaxed atomics, read lock-free by the census.
 *
 * Slots must be writable and executable: on the console one 64 KiB JIT object (exec-kernel-2026-10-14: in a payload
 * only JIT shared memory executes; pw_wine_mmu4k_ps5.c), on the host an RWX mapping.
 *
 * Costs (estimates, nothing measured on the console): a signal ~40 us, a run's entry ~45 us plus its exit trap ~40 us,
 * ~40 ns an instruction (tests/test_pw_wine_mmu4k.c prints the host's). Unproven on the console: a fault taken on a
 * run stack outside the handler, RWX slots, the cost per instruction (docs/MMU4K.md section 14). */

enum {
    PW_WINE_MMU4K_HOST_PAGE = 16384, PW_WINE_MMU4K_PAGE = 4096,
    PW_WINE_MMU4K_YIELD = 256,     /* instructions between looks at the virtual mutex's waiters */
    PW_WINE_MMU4K_IDLE = 1024,     /* instructions without an alias store that end a run (C2) */
    PW_WINE_MMU4K_BUDGET = 8192,   /* instructions a run at most: how long P4's signals wait (~0.3 ms) */
    PW_WINE_MMU4K_PAGES = 8,       /* written host pages a run remembers (C1) */
    PW_WINE_MMU4K_SLOT = 32, PW_WINE_MMU4K_SLOT_BYTES = 65536,   /* 2048 slots */
    PW_WINE_MMU4K_STACKS = 16, PW_WINE_MMU4K_STACK = 16384,      /* concurrent runs, and each one's stack */
};
/* shortcut: every number above is a first guess; tune it from the census (docs/MMU4K.md section 10). */

/* The program's registers, in the encoding's order: rax rcx rdx rbx rsp rbp rsi rdi r8-r15. */
typedef struct PwWineMmu4kRegs { uint64_t gpr[16], rip, rflags; } PwWineMmu4kRegs;
typedef struct PwWineMmu4kPage {
    uintptr_t host_page;
    uint8_t kernel_write;    /* the kernel lets the whole host page be written (Wine's host protection) */
    uint8_t plain;           /* bit i: 4 KiB page i is plain: committed, read-write, not guard, write-watch, write-copy */
    uint8_t exec;            /* bit i: 4 KiB page i may be executed: get_unix_prot(vprot) & PROT_EXEC (committed, not guard) */
    intptr_t alias_delta;    /* alias - host_page for a mixed page with an alias, else 0 */
    uint64_t generation;     /* pw_wine_alias_generation() when classified */
} PwWineMmu4kPage;
/* Called inside pw_wine_mmu4k_call on the run stack, right after a sigreturn: on the console each op first repairs a
 * null %fs as patch 0590's dispatchers do (docs/MMU4K.md P5). classify reads pw_wine_alias_generation() after its
 * pw_wine_alias_get, which may give a window back. */
typedef struct PwWineMmu4kOps {
    void *context;
    /* Under the mutex: fill *page for host_page (16 KiB aligned); 0, or -1 when it is not Wine's (the run ends). */
    int (*classify)(void *context, uintptr_t host_page, PwWineMmu4kPage *page);
    void (*lock)(void *context);     /* virtual_mutex, held for real even once the process exits */
    void (*unlock)(void *context);
    /* The threads blocked on virtual_mutex (pw_wine_perf_vlock_waiters), read relaxed every PW_WINE_MMU4K_YIELD
     * instructions; NULL: a run never hands the mutex over. A thread ended while it blocks stays counted, so a count
     * left stale makes runs end at every look, handing over more often, never less. */
    const uint64_t *waiters;
} PwWineMmu4kOps;
typedef struct PwWineMmu4kRun PwWineMmu4kRun;   /* at the top of a run stack; opaque to Wine */
enum { PW_WINE_MMU4K_EXIT_IDLE, PW_WINE_MMU4K_EXIT_BUDGET, PW_WINE_MMU4K_EXIT_FORM, PW_WINE_MMU4K_EXIT_PROTECTED,
       PW_WINE_MMU4K_EXIT_STRADDLE, PW_WINE_MMU4K_EXIT_FAULT, PW_WINE_MMU4K_EXIT_FETCH, PW_WINE_MMU4K_EXIT_NOALIAS,
       PW_WINE_MMU4K_EXIT_YIELD, PW_WINE_MMU4K_EXITS };
enum { PW_WINE_MMU4K_NOT_OURS, PW_WINE_MMU4K_RECOVER, PW_WINE_MMU4K_RESUME, PW_WINE_MMU4K_STEP,
       PW_WINE_MMU4K_FAULT };
typedef struct PwWineMmu4kStats {
    uint64_t runs, insns, exits[PW_WINE_MMU4K_EXITS], stepfall, stack_full, stack_damage;
    uint32_t top[4]; uint64_t top_count[4];   /* stepfall's instructions as map << 8 | opcode, space-saving */
} PwWineMmu4kStats;

/* slots: slot_bytes (at least PW_WINE_MMU4K_SLOT_BYTES) of memory both writable and executable; xsave_mask: XCR0 & 7,
 * x87 and SSE at least. PW_OK, PW_ERR_PRECONDITION (bad argument), PW_ERR_STATE (already initialized). */
int pw_wine_mmu4k_init(const PwWineMmu4kOps *ops, void *slots, size_t slot_bytes, uint64_t xsave_mask);
int pw_wine_mmu4k_ready(void);            /* 1 after a successful init */
int pw_wine_mmu4k_stack_take(void);       /* a free run stack (atomic), or -1 (counted stack_full) */
void pw_wine_mmu4k_stack_give(int stack);
/* Start a run for the write at address in host_page: the program's regs are copied into the run, and *regs is
 * rewritten to the engine's start (rip pw_wine_mmu4k_start, rsp the stack, rdi the run, rflags 0x202). NULL for a
 * bad stack or argument (*regs untouched). */
PwWineMmu4kRun *pw_wine_mmu4k_enter(int stack, PwWineMmu4kRegs *regs, uintptr_t host_page, uintptr_t address);
/* A signal on a thread in a run; *regs holds the interrupted GPRs, RIP and RFLAGS.
 * RECOVER: a fault in a slot or a guarded routine, recorded in the run; *regs is the recovery point (rip, rsp,
 *          rflags): write those back and return from the handler.
 * RESUME, STEP, FAULT: the exit trap. *regs is the program's state, the stack is given back and the run is over.
 *          STEP: 1122's step for *address. FAULT: a write access violation at *address (C4's straddle rule).
 * NOT_OURS: anything else; *regs is untouched. */
int pw_wine_mmu4k_signal(PwWineMmu4kRun *run, int signal, uintptr_t fault_address, PwWineMmu4kRegs *regs,
                         uintptr_t *address);
void pw_wine_mmu4k_run(PwWineMmu4kRun *run);   /* the start stub calls it; nothing else does */
void pw_wine_mmu4k_stats(PwWineMmu4kStats *stats);
/* Back to not initialized, with empty caches and zero counters (tests). */
void pw_wine_mmu4k_reset(void);
void pw_wine_mmu4k_start(void);                /* pw_wine_mmu4k_slot.S: a run's first RIP */
extern const char pw_wine_mmu4k_exit_trap[];   /* pw_wine_mmu4k_slot.S: its ud2 */

#endif
