# MMU4K: stores beside read-only 4 KiB pages, through an exact software MMU

Status 2026-10-15: steps 1 (the long-mode decoder, section 4.1), 2 (aliases and their sources, sections 4.3 and 8),
3 (the engine, section 4.2) and 4 (patch 1134, which enters it, and the console probes, sections 4.4-4.6 and 12) are
built and host-tested; the probes pass on the PC under the system Wine (the oracle). On the console the engine as
patch 1134 left it ran in `gate-hang-a-2026-10-15-01`: 1,250 runs in 542 census lines (19,254 instructions; exits
protected 1,152, straddle 89, idle 9, all in the large-shared-section helper) with no fault in the engine, and
section 12's gate cases vm-truth32 and vm-truth64 passed there; the on-request probes (`--speed`, `--contention`,
`--rearm`) have not run.
Patch 1136 (2026-10-16, host-tested only, not run on the console): a run ends at a waiting thread (exit `yield`)
instead of unlocking and relocking the virtual mutex, no run starts while the process exits and a run already going
keeps a real hold of the mutex, and a fault in the engine dooms the helper. This file is the contract between the build
steps of this work (section 13); a step that changes an interface changes this file in the same step.

Sources: the judge's verdict and the three designs it judged (scratchpad `mmu4k/judge.md`, `design-{1,2,3}.json`),
and the Windows program they were measured with (the earlier emulator experiment, removed). The verdict numbers the designs differently from their files: its design 1 (shadow windows,
rejected) is `design-2.json`; its design 2 (alias stores and an interpreter, "correct but too slow") is
`design-1.json`; its design 3 (mmu4k, the core) is `design-3.json`. That program's facts below are the verdict's.

## 1. The problem and the invariant

The console's host page is 16 KiB and holds four Windows pages. Patch 1122 maps a host page read-only when its
committed pages disagree on write (`get_host_page_unix_prot`) and single-steps each store a 4 KiB page allows: two
signals, 86 us a store (`vmp-tracker-2026-10-14-03`); the test workload under that program ran at 2.0-2.4 fps
(2026-10-15; record removed from the tree; in git history). Patch 1123 reported the read-only neighbours as written before the store:
56.1 fps (2026-10-15; record removed from the tree; in git history), but the false reports made the program upload stale guest bytes over GPU-written
images (white glyph blocks). The program needs the access violation at the store itself: `ForEachUploadRange` re-protects
before it copies, the hand-off to the GPU thread makes no system call, a no-access page is downloaded before it is
marked dirty, and a late handler run can EXIT it (`TrackingSpinLock`).

A 4 KiB page is **plain** when it is committed, readable and writable by its own protection, and not guard,
write-watch or write-copy. Any other committed or uncommitted page is **protected** for a store. A host page is
**mixed** when the kernel holds it read-only (1122's rule) and at least one of its pages is plain.

The invariant, each line a test (sections 11, 12):

- **I1** No write is ever reported that did not happen. The only access violation Wine makes up is the straddle rule
  (C4), for an instruction that does write the reported byte.
- **I2** A store into a protected 4 KiB page faults at the store, on the writing thread, before its bytes land: the
  handler sees the old bytes, RIP at the store and the registers as they were before it.
- **I3** Stores to plain 4 KiB pages in mixed host pages run without a signal each: inside a run, through an alias.
- **I4** No alias maps direct memory after it is released: every release forgets the aliases first.

## 2. Decisions

Build design 3 (mmu4k): the CPU runs every instruction itself in a slot, with only its memory operand rewritten. Take
from design-1.json the alias module and its forget hooks, the straddle rule and the alias, contention and re-arm
probes. Delete patch 1123 entirely. The verdict's four corrections:

- **C1 Every written host page is classified.** A run keeps a table of the last 8 host pages it wrote:
  kernel-writable, store at the real address; mixed with the target pages plain, store through the alias; anything
  else, end the run. (The program's vertex ring flips adjacent 4 KiB pages between read-only and writable.)
- **C2 Idle exit at 1,024 instructions** without an alias store (was 32): staying is worth it while the instructions
  between alias stores cost less than one re-entry (45 us / ~40 ns).
- **C3 The engine runs outside the signal handler.** `segv_handler` copies the registers out, blocks the
  asynchronous signals through `uc_sigmask`, points RIP at an engine stub on a run stack and returns. A fault in a
  slot is an ordinary first-level signal. The program's vector registers stay live; the engine's translation unit is
  built `-mgeneral-regs-only` and checked with `llvm-objdump`/`llvm-nm`.
- **C4 Stop before a protected store.** A store that touches a protected page ends the run; the instruction runs
  again natively and takes a real kernel fault. The one exception is the straddle rule: an exactly-sized store whose
  first byte lies in a plain page and which reaches a protected page gets a write access violation at the first byte
  of the first protected page, the address a 4 KiB MMU reports (this also fixes 1122's straddle hole wherever the
  engine runs).

Precisions this spec adds to make C3 implementable (each named, so a later step can revisit it):

- **P1 Exit by trap.** A run ends with `ud2` at `pw_wine_mmu4k_exit_trap`. The SIGILL handler writes the program's
  registers into the ucontext and restores its signal mask; `sigreturn` restores mask and registers in one step.
  Unblocking with `sigprocmask` and jumping back leaves a window in which SIGUSR1 (suspend) and SIGQUIT see the
  engine's context, and an unblock by a raw `syscall` instruction in our own code is refused by the build (the PRX
  audit fails a module with `raw_syscalls`). Cost: one more signal (about 40 us) per run.
- **P2 Calls out of the engine save the vector state.** Everything outside `pw_wine_mmu4k.c` that a run calls
  (classify, the virtual mutex, the decoder on a cache miss, alias maps) goes through `pw_wine_mmu4k_call`: `xsave64`
  of x87/SSE/AVX (the mask is XCR0 & 7) into the run's own 64-byte-aligned area, `fninit`, `ldmxcsr 0x1f80` (Wine's
  Unix-side MXCSR), the call, `xrstor64`. Our own buffer, so no dependence on the ucontext layout. A cache hit on a
  known host page makes no call.
- **P3 Zero progress steps.** A run that changed nothing (no instruction retired, no REP element done) ends in
  1122's step (or the straddle fault), never in a plain native retry. This is the progress guarantee: no livelock.
- **P4 What a run blocks.** `server_block_set` (SIGALRM SIGIO SIGINT SIGHUP SIGUSR1 SIGUSR2 SIGCHLD) plus SIGQUIT:
  `quit_handler` ends a thread at once when RSP is outside the syscall stack, which would leave `virtual_mutex`
  held (`pw_wine_pages` blocks SIGQUIT for the same reason). SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGTRAP stay open.
- **P5 The console's ops repair a null FS first** (step 3 found it; step 4 builds it). A run executes right after a
  `sigreturn` (its start, and every recovered slot fault), and patch 0590 measured that a thread can come back from
  a signal handler with a null `%fs` selector, so libkernel's `%fs:0x10` thread pointer reads address 0x10. The
  engine never touches FS (no TLS, `-fno-stack-protector`), but `ps5_mmu4k_lock`, `_unlock` and `_classify`
  (`virtual_mutex`, `pw_wine_alias_get`'s kernel calls) do: each begins with 0590's check (`movw %fs`, and when it
  is null `sysarch(AMD64_SET_FSBASE, &pthread_teb)`). PE code keeps FS as the libc base (patch 0500).
- **P6 An overlapping forward MOVS reads its source through the same alias.** The CPU's fast strings judge overlap
  by linear address: on the host, 689 of 3,239 `rep movsb` copies onto their own source (destination 1-79 bytes
  above it) came out wrong with only the destination through an alias (i5-10600K, 2026-10-15). When the
  destination goes through the alias and lies above the source by less than the chunk, the source is read through
  the alias too if it is in the same host page; otherwise the run ends `form`.

## 3. One run, end to end

1. The program stores into a plain page of a mixed host page H. The kernel faults (H is read-only).
2. `segv_handler` sets `ps5_fault_own` and calls `virtual_handle_fault`. In 1122's branch, under `virtual_mutex`,
   `ps5_mmu4k_wanted` (section 4.5) takes an alias of H and a run stack, sets `ps5_mmu4k_page = H` and returns
   `STATUS_SUCCESS` without opening H. Any condition missing: 1122's step, unchanged.
3. Back in `segv_handler` (`ps5_fault_resolved`): `ps5_mmu4k_enter` copies the GPRs, RIP and RFLAGS into the run (top of the run stack),
   saves `uc_sigmask` in `ps5_mmu4k_mask`, adds P4's set to `uc_sigmask`, sets RIP = `pw_wine_mmu4k_start`,
   RSP = the run stack, RDI = the run, RFLAGS with TF, DF and AC clear, and returns. `sigreturn` restores the
   program's vector state unchanged.
4. `pw_wine_mmu4k_start` calls `pw_wine_mmu4k_run`, which takes `virtual_mutex` (P2 call) and runs instructions
   (section 6) until an exit, holding the mutex throughout. Every 256 instructions it reads how many threads wait on
   the mutex (`ops.waiters`, patch 1136: `pw_wine_perf_vlock_waiters`) and ends `yield` when one does, so the run
   ends within 256 instructions of a blocked fault or protection change; with no waiter a run lasts up to 8,192
   (~0.3 ms by the estimate). Who takes the mutex next is the mutex's and the scheduler's choice: the thread's next
   such store comes back through the exit trap and a native fault (~80 us) to `virtual_handle_fault`'s trylock,
   which can beat the woken waiter. On the host a blocked thread took the mutex first 49-50 times in 50 when its
   holder relocked 80 us later, 0 in 50 when it relocked at once (`tests/test_pw_wine_perf.c`, 2026-10-16); the
   console's rate is not measured. Until patch 1136 the run unlocked and relocked the mutex there: no hand-over, and
   the host-page table emptied each time; in a 2026-10-15 run (record removed from the tree; in git history) the census waited more than 160 s on the mutex.
   A run starts only while the process is not exiting (`ps5_mmu4k_wanted`: after `process_exiting`, `mutex_lock`
   does nothing); a run that asked just before takes and gives the mutex with `pthread_mutex_lock` and
   `pthread_mutex_unlock` from then on (`ps5_mmu4k_lock`, `ps5_mmu4k_unlock`), so runs still exclude each other.
5. At the exit it releases the mutex and returns to the stub, which executes `ud2`.
6. `segv_handler`'s first check sees SIGILL at the exit trap on a thread with `ps5_mmu4k_run` set:
   `pw_wine_mmu4k_signal` gives back the stack and returns the program's registers and the action. The ucontext
   takes the registers and `ps5_mmu4k_mask`, and: RESUME returns to the program; STEP and FAULT both run
   `virtual_handle_fault` on a write record at the engine's address with `ps5_fault_own` FALSE, so no run starts:
   for STEP (the run's first store) that is 1122's step exactly as without the engine (the open, then the trap flag
   in `ps5_fault_resolved`), for FAULT (the straddle address) the access violation, guard page or write-watch answer
   Wine gives that page; what it does not resolve is raised (`setup_raise_exception`).

## 4. Files and functions

### 4.1 Decoder: `src/pw_ia32_decode.{h,c}`, `src/pw_ia32_isa.{def,h,c}` (the one x86 parser gains a long mode)

Built in step 1 (host-tested; no console run; evidence `x64-decode-oracle-2026-10-15-01`).

```c
enum { PW_IA32_MODE_32 = 32, PW_IA32_MODE_64 = 64 };
/* pw_ia32_decode(b, n, i) is pw_ia32_decode_mode(b, n, PW_IA32_MODE_32, i): 32-bit results do not change. */
int pw_ia32_decode_mode(const uint8_t *bytes, size_t available, unsigned mode, PwIa32Inst *inst);

enum { PW_IA32_ACCESS_NONE, PW_IA32_ACCESS_ADDRESS, PW_IA32_ACCESS_READ, PW_IA32_ACCESS_WRITE,
       PW_IA32_ACCESS_RW };
typedef struct PwIa32Access { uint8_t kind, exact; uint16_t bytes; } PwIa32Access;
/* The ModRM memory operand of an instruction with a form: how it is accessed and its size (exact, or an upper
 * bound with exact 0). PW_OK; PW_ERR_UNSUPPORTED when the form touches memory ModRM does not name (STACK, STR,
 * IMPL, moffs) or the operand has no size (M); PW_ERR_PRECONDITION for a null argument or no form. */
int pw_ia32_access(const PwIa32Inst *inst, PwIa32Access *access);
/* src/pw_ia32_print.h: the ModRM memory operand alone, as LLVM spells it, with the size word for bits. */
int pw_ia32_print_memory(const PwIa32Inst *inst, unsigned bits, char *out, size_t capacity);
```

- `PwIa32Inst` gains: `mode`; `rex` (the REX byte that took effect, or 0); `opcode_at` (offset of the opcode byte
  itself: after 0F/0F 38/0F 3A, or after the VEX payload), `modrm_at` (0xff when absent); `rip_relative`;
  `vex_l`, `vex_w`, `vex_vvvv` (un-inverted, 0-15), `vex_pp` (`PW_IA32_PP_*`). `prefixes` counts REX too; with
  VEX it is the offset of the C4/C5 byte. In mode 64, `operand_size` is 16/32/64 (VEX: 64 with W),
  `address_size` 32/64, and `reg`, `rm`, `base`, `index` are 0-15 (REX.R/B/X, or VEX's); rows select on the low
  three bits. `imm` is `uint64_t` (imm64, moffs64), `imm2` stays `uint32_t`.
- Long mode: REX as the last prefix (a REX before another prefix is ignored, as the CPU does), RIP-relative
  (`mod 00 rm 101`; a SIB with base 101 is not), `D64` rows default to 64-bit operands (stack and near branches),
  MOVSXD, imm64 (B8-BF with REX.W, the one 8-byte immediate) and moffs64 lengths, `NO64` rows are MALFORMED, 40-4F
  are REX, C4/C5 always VEX and 62 always EVEX. Two exceptions in the decoder: 90 with REX.B is XCHG r8 (never
  NOP or PAUSE), and a near branch with 66 has no form (AMD makes it 16-bit, Intel ignores the 66; the length
  decoded is AMD's).
- `.def` attributes (the field widens to `uint32_t`): `MRO` (operand 0 in memory is only read: CMP, TEST, BT,
  MUL/DIV, PUSH Ev, CALL/JMP Ev, x87 loads, LDMXCSR, CLFLUSH, VERR/VERW), `ADDR` (address only: LEA, NOP Ev,
  PREFETCHx), `STACK` (implicit [RSP] and RSP: PUSH, POP, CALL, RET, ENTER, LEAVE, PUSHF/POPF, IRET, far
  CALL/RET), `STR` (implicit [RSI]/[RDI]; it does not say which way: MOVS/STOS/INS write, LODS/SCAS/CMPS/OUTS
  read), `IMPL` (other memory ModRM does not size: XLAT, MASKMOVQ/DQU, the XSAVE/FXSAVE family and the x87
  environment forms FNSAVE/FRSTOR/FNSTENV/FLDENV), `MASK` (masked store: never exact); modes and encodings:
  `NO64`, `X64` (long mode only), `D64`, `VEX` (the legacy row is also the VEX form, VEX.pp its prefix), `VEXONLY`,
  `L0`/`L1`, `W0`/`W1` (as VEX, only with that L or W), `VLH` (as VEX with L = 1 the memory operand doubles:
  VPMOVSX/ZX, VCVTPS2PD, VCVTDQ2PD, F16C), `NOV` (as VEX, vvvv must be unused: anything else is #UD). Size
  selectors gain `O64` and `A64`; operands gain `Iv` (imm64), `By` (a general register in VEX.vvvv, BMI), `Wb`,
  `Wdq` (128 bits whatever VEX.L: VEXTRACTF128, VINSERTF128, VBROADCASTF128, the shift counts of
  VPSLLW/D/Q-VPSRAW/D-VPSRLW/D/Q).
- Access kind: no ModRM memory, NONE; `ADDR`, ADDRESS; memory at operand 1 or 2, READ; at operand 0, READ with
  `MRO`, RW with `LOCK` or `O1` (lockable and shift forms are read-modify-write), else WRITE. Sizes: b/w/d/q,
  `v` by operand size, `x` 16 or 32 by VEX.L, `Mt` 10, `Mp` 2 + operand size, `VLH` doubling; exact unless `MASK`.
- VEX: a VEX instruction matches the legacy rows marked `VEX` (SSE to SSE4.2, AES, PCLMULQDQ, with VEX.pp as the
  mandatory prefix; not MMX, SHA, CRC32, ADX, POPCNT/LZCNT/TZCNT, MOVNTI, the fences, XSAVE), and `VEXONLY` rows:
  BMI1/2, FMA (all 60), F16C, VZEROUPPER/ALL, VBROADCAST*/VPBROADCAST*, VEXTRACT/VINSERT F128/I128, VPERM2F128/
  I128, VPERMD/PS/Q/PD, VPERMILPS/PD, VTESTPS/PD, VPBLENDD, VPSLLV/VPSRLV/VPSRAV, VBLENDV*, and VMOVSS/VMOVSD
  (split: from or to memory they leave vvvv unused, between registers they use it). A VEX row leaves out its
  vector vvvv register; the engine finds a general register in vvvv only in a `By` operand. VSIB (gathers),
  VMASKMOV/VPMASKMOV, EVEX and XOP have no form, so nothing runs them but the CPU. In 32-bit mode VEX still has
  no form (32-bit results do not change).
- General registers an instruction names (the engine's scratch choice): ModRM.reg under a G operand, ModRM.rm
  with mod 3 under an E operand, `base` and `index`, VEX.vvvv under `By`, the opcode's register under Zb/Zv
  (with REX.B), and implicit ones (rAX, rCX, rDX, rSI, rDI, rSP: never r8-r15).
- `pw_ia32_print` prints both modes for legacy encodings and returns `PW_ERR_UNSUPPORTED` for a VEX form;
  `tools/ia32-dump.sh --64` dumps long-mode code (a VEX form as its row and memory operand).
- Oracle: `tools/ia32-decode-oracle.sh --64` (libLLVM `x86_64`, 58,645,650 encodings: 25 prefix sets with REX,
  and VEX over every map × pp × L × W × R/X/B, with vvvv unused and in use) compares length, validity, text
  (legacy), and every form's memory operand from `pw_ia32_access` (kind and exact size) with LLVM's; and
  `tools/x86_llvm_semantics.cpp` checks the attributes against LLVM's MC description: STORE_MISSED (LLVM may store,
  the row says no store), STORE_EXTRA (the row writes, LLVM never stores: a missing MRO), FLOW (BRANCH), STACK.
  Disagreements kept on purpose, each with what the CPU does, are in `tests/x64-llvm-differences.txt`.

### 4.2 Engine: `wine/ps5/pw_wine_mmu4k.{h,c}`, `pw_wine_mmu4k_slot.S`, `pw_wine_mmu4k_ps5.c` (all new)

Built in step 3 (host-tested; no console run). Linked into ntdll in both runtimes; since step 4 `virtual_init`
calls `__wine_ps5_mmu4k_init` (patch 1134), which also calls `pw_wine_alias_init`.

`pw_wine_mmu4k.h` (portable; copied into `dlls/ntdll/unix/` by `build-wine-ps5.sh`, with `pw_wine_alias.h`, so it
includes only `<stddef.h>`/`<stdint.h>`). The header carries this contract and its measurements (signal ~40 us, step
86 us, the 2026-10-15 font runs, JIT `exec-kernel-2026-10-14`).

```c
enum {
    PW_WINE_MMU4K_HOST_PAGE = 16384, PW_WINE_MMU4K_PAGE = 4096,
    PW_WINE_MMU4K_YIELD = 256,     /* instructions between looks at the virtual mutex's waiters */
    PW_WINE_MMU4K_IDLE = 1024,     /* instructions without an alias store that end a run (C2) */
    PW_WINE_MMU4K_BUDGET = 8192,   /* instructions a run at most: how long P4's signals wait (~0.3 ms) */
    PW_WINE_MMU4K_PAGES = 8,       /* written host pages a run remembers (C1) */
    PW_WINE_MMU4K_SLOT = 32, PW_WINE_MMU4K_SLOT_BYTES = 65536,   /* 2048 slots */
    PW_WINE_MMU4K_STACKS = 16, PW_WINE_MMU4K_STACK = 16384,      /* concurrent runs, and each one's stack */
};
/* shortcut: every number above is a first guess; tune it from the census (section 10). */
typedef struct PwWineMmu4kRegs { uint64_t gpr[16], rip, rflags; } PwWineMmu4kRegs; /* rax rcx rdx rbx rsp rbp rsi rdi r8-r15 */
typedef struct PwWineMmu4kPage {
    uintptr_t host_page;
    uint8_t kernel_write;    /* the kernel lets the whole host page be written (Wine's host protection) */
    uint8_t plain;           /* bit i: 4 KiB page i is plain (section 1) */
    uint8_t exec;            /* bit i: 4 KiB page i may be executed: get_unix_prot(vprot) & PROT_EXEC */
    intptr_t alias_delta;    /* alias - host_page for a mixed page with an alias, else 0 */
    uint64_t generation;     /* pw_wine_alias_generation() when classified */
} PwWineMmu4kPage;
typedef struct PwWineMmu4kOps {
    void *context;
    int (*classify)(void *context, uintptr_t host_page, PwWineMmu4kPage *page); /* under the mutex: 0, -1 not Wine's */
    void (*lock)(void *context);     /* virtual_mutex, held for real even once the process exits */
    void (*unlock)(void *context);
    const uint64_t *waiters;         /* threads blocked on virtual_mutex, read relaxed; NULL: never hand over */
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

/* slots: slot_bytes of memory both writable and executable (a JIT object on the console, mmap RWX on the host);
 * xsave_mask: XCR0 & 7. PW_OK, PW_ERR_PRECONDITION (bad argument), PW_ERR_STATE (already initialized). */
int pw_wine_mmu4k_init(const PwWineMmu4kOps *ops, void *slots, size_t slot_bytes, uint64_t xsave_mask);
int pw_wine_mmu4k_ready(void);            /* 1 after a successful init */
int pw_wine_mmu4k_stack_take(void);       /* a free run stack (atomic), or -1 (counted stack_full) */
void pw_wine_mmu4k_stack_give(int stack);
/* Start a run for the write at address in host_page: the program's regs are copied into the run, and *regs is
 * rewritten to the engine's start (rip pw_wine_mmu4k_start, rsp the stack, rdi the run, rflags 0x202). */
PwWineMmu4kRun *pw_wine_mmu4k_enter(int stack, PwWineMmu4kRegs *regs, uintptr_t host_page, uintptr_t address);
/* A signal on a thread in a run; *regs holds the interrupted GPRs, RIP and RFLAGS.
 * RECOVER: a fault in a slot or a guarded routine, recorded in the run; *regs is the recovery point (rip, rsp,
 *          rdi): write those back and return from the handler.
 * RESUME, STEP, FAULT: the exit trap. *regs is the program's state, the stack is given back and the run is over.
 *          STEP: 1122's step for *address. FAULT: a write access violation at *address (C4's straddle rule).
 * NOT_OURS: anything else; *regs is untouched. */
int pw_wine_mmu4k_signal(PwWineMmu4kRun *run, int signal, uintptr_t fault_address, PwWineMmu4kRegs *regs,
                         uintptr_t *address);
void pw_wine_mmu4k_run(PwWineMmu4kRun *run);   /* the start stub calls it; nothing else does */
void pw_wine_mmu4k_stats(PwWineMmu4kStats *stats);
void pw_wine_mmu4k_reset(void);                /* tests: not initialized, empty caches, zero counters */
void pw_wine_mmu4k_start(void);                /* pw_wine_mmu4k_slot.S: a run's first RIP */
extern const char pw_wine_mmu4k_exit_trap[];   /* pw_wine_mmu4k_slot.S: its ud2 */
```

- `pw_wine_mmu4k.c`: the run loop (section 6), the decode/slot cache (2048 entries keyed by RIP, each checked
  against the instruction's bytes before use), slot encoding (section 7), classification (section 5), the stack
  pool (16 x 16 KiB in `.bss`; the lowest 64 bytes of each hold a pattern checked at the exit trap, counted
  `stack_damage`). Built `-mgeneral-regs-only -ffreestanding -fno-builtin -fno-stack-protector`; copies are
  `rep movs`/`rep stos` in inline assembly and large structs are never assigned, so the compiler emits no
  `memcpy`/`memset`. Its object may reference no `%xmm`/`%ymm`/`%zmm`/`%st`/`%mm`/`%k` operand, and no undefined
  symbol outside an allow-list: the `.S` routines, `pw_ia32_decode_mode`/`pw_ia32_access` (reached only inside
  `pw_wine_mmu4k_call`) and `_GLOBAL_OFFSET_TABLE_`. It reads program memory only through the guarded routines or
  slots. A run (`PwWineMmu4kRun`, 1,600 bytes, 64-byte aligned at the top of its stack) begins with the program's
  `PwWineMmu4kRegs` (gpr at 0, rflags at 136) and the exec routine's saved RSP at 144, offsets the `.S` uses and
  the `.c` checks with `_Static_assert`; its xsave area (1,024 bytes) is zeroed at every enter, header included.
  Plans: verbatim, memory, and the emulated JMP/Jcc, JMP/CALL r/m, CALL rel32, RET (imm16), LOOP/LOOPE/LOOPNE (67:
  `form`, ECX's upper half is unsaid), JRCXZ/JECXZ, PUSH r/imm/r/m, POP r, LEAVE, MOVS/STOS (F2 or F3 repeat).
- `pw_wine_mmu4k_slot.S` (each routine states its register contract; the host test is its C oracle; no `syscall`
  instruction, which the PRX audit would count):
  - `pw_wine_mmu4k_start`: rdi = run; calls `pw_wine_mmu4k_run`, then `pw_wine_mmu4k_exit_trap: ud2`.
  - `int pw_wine_mmu4k_exec(PwWineMmu4kRun *run, const void *slot)`: saves callee-saved registers and its RSP in
    the run, loads RFLAGS (`popfq`) and every GPR but RSP from the run's slot registers, calls the slot, stores
    them back, `cld`, clears AC, returns 0. `pw_wine_mmu4k_recover` (RSP from the run) returns 1 with the run's
    registers untouched.
  - Guarded routines between `pw_wine_mmu4k_guarded_begin` and `_end`, each 0 or 1 (fault, recorded):
    `pw_wine_mmu4k_read(dst, src, bytes)` (code fetch, POP/RET loads), `pw_wine_mmu4k_write(dst, src, bytes)`
    (PUSH/CALL stores), `pw_wine_mmu4k_copy(dst, src, count, element, stos)` (`rep movs`/`rep stos` of 1/2/4/8;
    at a fault the recovery records RCX/RSI/RDI from the ucontext).
  - `void pw_wine_mmu4k_call(void (*fn)(void *), void *arg, void *xsave_area, uint64_t mask)`: P2.
- `pw_wine_mmu4k_ps5.c` (console): `int __wine_ps5_mmu4k_init(const PwWineMmu4kOps *ops)` (called by
  `virtual_init`; 0 on, -1 off, one log line either way: the switch, no shared-page range, the JIT object refused
  (a title has none: that is the "no payload" case), the slot mapping or its RWX protection refused, the alias or
  engine init), the console's `const PwWineAliasOps __wine_ps5_alias_ops` (section 8; also what
  `tests/test_crash_replays.c` drives), and `unsigned int __wine_ps5_mmu4k_stats(uint64_t *out, unsigned int count)`
  (section 10; in ntdll's descriptor since step 4). The xsave mask is `xgetbv(0) & 7`.

### 4.3 Aliases and their sources

Built in step 2 (host-tested; no console run; nothing calls `pw_wine_alias_init` yet, so `get` answers 0 and
`forget` finds nothing until step 3's `__wine_ps5_mmu4k_init`).

| File | Change |
| --- | --- |
| `wine/ps5/pw_wine_alias.{h,c}` (new) | `PwWineAliasOps { context; int (*backing)(ctx, uintptr_t address, int64_t *dmem, uintptr_t *low, uintptr_t *high); uintptr_t (*take)(ctx, size_t); void (*give)(ctx, uintptr_t, size_t); int (*map)(ctx, uintptr_t address, size_t bytes, int64_t dmem); }` (`backing`: 1 found, 0 none; `map`: 0, or nonzero); `enum { PW_WINE_ALIAS_HOST_PAGE = 16384, PW_WINE_ALIAS_WINDOW = 2u << 20, PW_WINE_ALIAS_WINDOWS = 16 }`; `int pw_wine_alias_init(const PwWineAliasOps *)` (`PW_OK`, `PW_ERR_PRECONDITION` for a missing op, `PW_ERR_STATE` the second time); `uintptr_t pw_wine_alias_get(uintptr_t host_page)` (the alias of H, or 0: not initialized, misaligned, or counted); `void pw_wine_alias_forget(int64_t dmem, uint64_t bytes)`; `uint64_t pw_wine_alias_generation(void)` (bumped only when a window is given back: an eviction, or a forget that found one); `void pw_wine_alias_stats(PwWineAliasStats *)` (`maps, map_failures, forgets, evictions, noalias`; lock-free relaxed loads; `noalias` counts every 0 but a refused map: no backing, an answer that does not hold the 16 KiB page, no room after one eviction); `void pw_wine_alias_reset(void)` (tests). The header includes only `<stddef.h>`/`<stdint.h>`, so it may be copied into `dlls/ntdll/unix/`. |
| `wine/ps5/pw_wine_pages.{h,c}` | `uintptr_t pw_wine_pages_take(size_t bytes)`: a free run of the range (first-fit, 16 KiB units) left reserved for the caller's own fixed mapping, the kernel not asked, or 0 (no range, or none free). `void pw_wine_pages_give(uintptr_t address, size_t bytes)`: lay the reservation back (a range that cannot be laid back stays taken, lost, as `pw_wine_pages_unmap` does; an address outside the range is ignored). |
| `wine/ps5/pw_wine_dmem.{h,c}` | `int pw_wine_dmem_run_at(PwWineDmem *, uintptr_t address, PwWineDmemRun *run)`: 1 with the direct-memory run holding address (not `PW_WINE_DMEM_CALLER`), else 0. Takes the engine lock. |
| `wine/ps5/pw_wine_dmap.{h,c}` | `int pw_wine_dmap_lookup(int fd, uint64_t offset, int64_t *dmem, uint64_t *chunk_offset, uint64_t *chunk_bytes)`: the chunk holding offset of a direct section (remembered, else asked of the title as a fault does). 0, or -1 with errno (`EINVAL` not such a section, the title's, `EIO` an answer that does not hold offset). `pw_wine_dmap_map` uses the same lookup. |
| `wine/ps5/pw_wine_section.{h,c}` | `int pw_wine_section_backing(PwWineSection *, uintptr_t address, int *fd, uint64_t *offset, uintptr_t *low, uintptr_t *high)`: for a direct view whose chunk holding address is mapped (`chunks` bit): its fd, address's section offset, and the chunk's extent clipped to the view. 1, or 0 (a cache view, an unmapped chunk, no view). |
| `wine/ps5/pw_wine_dmem_ps5.c` | `release()` calls `pw_wine_alias_forget(offset, bytes)` before `sceKernelReleaseDirectMemory`; `int __wine_ps5_dmem_backing(uintptr_t, int64_t *offset, uintptr_t *low, uintptr_t *high)` over the payload's direct engine (`dmem` with `payload_allocate_direct` and the self-check passed; never `flex`): 1 with the run's extent, or 0 (always 0 once an adopt failed: `backing_stale`, section 8). |
| `wine/ps5/pw_wine_section_ps5.{h,c}` | `int __wine_ps5_section_backing(uintptr_t, int64_t *dmem, uintptr_t *low, uintptr_t *high)`: `pw_wine_section_backing` then `pw_wine_dmap_lookup`; dmem = chunk + (offset - chunk_offset), `[low, high)` also clipped to the title's chunk extent. 1, or 0. Declared in `pw_wine_section_ps5.h`. |
| `tests/fake_ps5_kernel.{h,c}` | Hazard `FK_MAPPED`: `sceKernelReleaseDirectMemory` of memory still mapped anywhere (the release unmaps it, as before; the hazard is the alias that outlived it). Every earlier replay holds with it. |

### 4.4 `wine/ps5/pw_wine_vm.{h,c}` and the patch series

Built in step 4 (host-tested and compiled into both runtimes; no console run).

- `pw_wine_vm.{h,c}`: `PwWineVmReport`, `pw_wine_vm_report_pick`, `pw_wine_vm_report_opened`,
  `reports_off`/`WINE_PS5_NEIGHBOUR_REPORTS` are gone, with their cases in `tests/test_pw_wine_vm.c`.
  `pw_wine_vm_step_slot` stays; its comment names 1122 and 1134.
- `wine/patches/1123-*.patch` is deleted (the PS5NOTIF continue in `exception.c`, `ps5_report`/`ps5_report_ok`,
  `VM_STAT_HOST_PAGE_REPORTS`, the `NtProtectVirtualMemory` hook). 1125 (the `beside` lines of 1122's branch), 1127
  (`unix_private.h` context) and 1128 (the `pw_wine_vm_report_pick` declaration context) were regenerated from the
  series rebased without 1123 (only hunk positions and context changed); `tools/check-patches.sh`: 206 clean.
- `wine/patches/1134-ntdll-ps5-stores-beside-read-only-4k-pages-run-through-a-4k-mmu.patch`, at the end:

`unix_private.h`, `struct ntdll_thread_data` (PROSPERO), after `ps5_step_tf`:

```c
void      *ps5_mmu4k_run;   /* the PwWineMmu4kRun this thread is in, or NULL */
void      *ps5_mmu4k_page;  /* virtual_handle_fault: segv_handler starts a run for this host page, */
void      *ps5_mmu4k_addr;  /* the store at this address, */
int        ps5_mmu4k_stack; /* on this run stack */
BOOL       ps5_fault_own;   /* the program's own fault, which may start a run (segv_handler) */
sigset_t   ps5_mmu4k_mask;  /* the program's signal mask, given back when its run ends */
```

and `extern void signal_ps5_fs_repair(void);`.

`virtual.c` (includes `pw_wine_alias.h`, `pw_wine_mmu4k.h`):

- 1122's branch gains one line before its step: `if (ps5_mmu4k_wanted( data, page, addr )) ret = STATUS_SUCCESS;
  else` (1122's open). No `ps5_step_open`/`virtual_ps5_step_open`: the exit trap's STEP goes through
  `virtual_handle_fault` itself (section 3, step 6), so 1122's code is untouched.
- `static BOOL ps5_mmu4k_wanted( struct ntdll_thread_data *, char *page, void *addr )` (section 4.5).
- `static int ps5_mmu4k_classify( void *, uintptr_t host_page, PwWineMmu4kPage * )`: -1 when no view holds
  `host_page`; `kernel_write` from `get_host_page_unix_prot( page, 0, 0 ) & PROT_WRITE`; bit i of `plain` when
  `get_unix_prot( get_page_vprot( page i ) )` allows write (committed, not guard, not write-watch) and, for a host page
  the kernel holds read-only, the page is not write-copy. So in a kernel-writable host page (an image's, or one with an
  executable page, which 1122 leaves to the union rule) the run stores at the program's address exactly where a native
  store would go through, and only a held host page's plain read-write pages go through the alias, never a private
  copy. (Changed from the spec, which made every page of a `SEC_IMAGE` view protected: that ended a run at every store
  into an image's writable data for no gain in correctness.) `alias_delta` from `pw_wine_alias_get` for a held host
  page with a plain page; `generation` read after it. Bit i of `exec` when `get_unix_prot( get_page_vprot( page i ) )`
  allows execute (committed, not guard): a run fetches code only there (section 6, step 1), whatever the host page's
  union. `ps5_mmu4k_lock/unlock` are `pw_wine_perf_vlock_lock/unlock( &virtual_mutex )`, as `mutex_lock/unlock`
  are, and plain `pthread_mutex_lock/unlock` once `process_exiting` is set (patch 1136), where `mutex_lock` does
  nothing: a run that asked just before the flag still excludes the others, and its unlock balances its lock.
  Each op starts with `signal_ps5_fs_repair()` (P5).
- `virtual_init` (PROSPERO, after `__wine_ps5_section_init`): `__wine_ps5_mmu4k_init( &ps5_mmu4k_ops )`.
- `get_host_page_unix_prot` keeps its exact text: `tests/extract_crash_replays.py` extracts it.

`signal_x86_64.c` (PROSPERO; `#include "pw_wine_mmu4k.h"`), before `segv_handler`:

- `void signal_ps5_fs_repair(void)`: `movw %fs`; when null, the TEB through `%gs:0x30` (never `NtCurrentTeb`, which is
  `pthread_getspecific` through `%fs`) and `sysarch( AMD64_SET_FSBASE, &pthread_teb )`, as `PW_RESTORE_FS` does.
- `ps5_regs_get`/`ps5_regs_set` over `R*_sig`, `RIP_sig`, `EFL_sig`.
- `static BOOL ps5_fault_is_own( ucontext_t * )`: 64-bit CS, no TF, RSP outside the syscall stack and the signal
  stack, no DR7 enable bit, no `WowTebOffset`, and runs not turned off (`ps5_mmu4k_off`).
- `static void ps5_mmu4k_enter( ucontext_t * )`: section 3 step 3; `uc_sigmask` gains every signal of
  `server_block_set` and SIGQUIT (P4). An `enter` that refuses (it cannot with a stack the fault took) gives the stack
  back and turns runs off, so the store steps rather than looping.
- `static void ps5_fault_resolved( ucontext_t * )`: the success tail of a write fault: `ps5_mmu4k_page` set, enter;
  else 1122's trap-flag setting when `ps5_step_tf == 2`; then `leave_handler`.
- `static BOOL ps5_mmu4k_signal( int signal, siginfo_t *, ucontext_t * )`, first thing in `segv_handler` (SIGSEGV,
  SIGBUS, SIGILL) and `fpe_handler`: one load when no run is active. NOT_OURS is logged once ("mmu4k: fault in the
  engine"), stamps `pw_ps5_child_doomed(PW_CHILD_DOOM_ENGINE)` (patch 1136: the run's lock of `virtual_mutex` is
  never released, so the helper's loader ends the process 5 s later) and is handled as any fault. RECOVER: registers back, `leave_handler`. Exit: registers and
  `uc_sigmask = ps5_mmu4k_mask`, `ps5_mmu4k_run = NULL`; RESUME, `leave_handler`; STEP and FAULT, section 3 step 6.
- The mask check: the console's ucontext is not the SDK header's (`UCONTEXT_MC`: the machine context at +64, not
  +16), and `uc_sigmask`'s place is not measured. Every exit trap until it fails asks the thread's mask
  (`pthread_sigmask`); SIGQUIT missing means the run did not run with the mask written at entry, and
  `ps5_mmu4k_off` stops all runs ("mmu4k: off: a run did not have its signal mask"). The likely layout is FreeBSD's
  (`uc_sigmask` at 0, a 64-byte aligned machine context), which the check confirms on the first run.
- `segv_handler`'s page-fault case sets `ps5_fault_own = ps5_fault_is_own( ucontext )` before `virtual_handle_fault`
  (the only caller; the exit trap sets it FALSE before its own call) and ends a resolved fault in `ps5_fault_resolved`.

### 4.5 When `virtual_handle_fault` starts a run (`ps5_mmu4k_wanted`, under `virtual_mutex`)

All of: `!process_exiting` (patch 1136: `mutex_lock` does nothing once it is set, so the exiting thread changes views
without the mutex, and a thread ended while it held the mutex never gives it back); `pw_wine_mmu4k_ready()` (so the switch is on and this is a payload); `data->ps5_fault_own`;
`!data->ps5_step_page[0]` (no step pending on this thread); `!data->ps5_mmu4k_run`; `pw_wine_alias_get( page )`
non-zero (else counted `noalias`); `pw_wine_mmu4k_stack_take() >= 0`. The section `RETRY` case keeps 1122's path
(no chunk mapped, so no alias anyway).

### 4.6 Elsewhere

- `native/proton_wine_child.c` (step 4): `reported=` is gone; `__wine_ps5_mmu4k_stats` is looked up and printed as a
  line of its own after the server line (section 10): appended to it, the 384-byte line overflowed and was dropped.
- `tools/build-wine-ps5.sh` (step 3, done): copies `pw_wine_alias.h` and `pw_wine_mmu4k.h` with the other ntdll
  headers; builds `pw_wine_mmu4k` with its flags, then fails the build when `llvm-objdump -d` (the host's
  `llvm-objdump-18` or `llvm-objdump`: the SDK has none) shows a vector or x87 register or `llvm-nm
  --undefined-only` a symbol outside the allow-list; assembles `pw_wine_mmu4k_slot.S`; builds `pw_wine_mmu4k_ps5`,
  `src/pw_ia32_isa`, `src/pw_ia32_decode`; all into `$dmem`, the group `dlls/ntdll/ntdll.so` and `link_prx ntdll`
  take. Step 4 added `__wine_ps5_mmu4k_stats` to ntdll's descriptor. `tools/build-payload-runtime.py` (not in the
  production build, not run) lists the same objects.
- `tools/host-tests.sh`: `test_pw_wine_alias` (step 2), `test_pw_wine_mmu4k` (step 3; flags `-mgeneral-regs-only
  -fno-sanitize=address,undefined`: ASan's stack handling and UBSan's runtime calls would break the engine's
  contract); `pw_wine_alias.c` joins `test_pw_wine_dmem_ps5` and `test_crash_replays` (step 2);
  `test_crash_replays` also links `pw_wine_mmu4k_ps5.c`, the engine and the decoder (step 3) for
  `__wine_ps5_alias_ops`, with a `__wine_ps5_section_backing` that answers nothing (no direct section there).
- `tools/run-battery.py` (step 4): `vm-truth32`, `vm-truth64` (both in `GATE_CASES`), `vm-speed64` and `vm-rearm64`
  (on request, beside `payload-vm-contention`), all in a helper (`PROTON_LAYOUT=launcher`).

## 5. Classification of one access

- Reads (READ, ADDRESS, and POP/RET loads): the program's address. The kernel allows reads of a mixed host page
  (1122 keeps reads at host-page granularity); a read that faults in a slot ends the run.
- A write of `[a, a + n)` (WRITE, RW, PUSH/CALL stores, a MOVS/STOS chunk), per host page through the run's table
  (a miss calls `classify`; -1 ends the run `protected`; a result whose `generation` differs from the table's
  empties the table first, so no stale alias is ever used: the run holds `virtual_mutex` throughout, as every
  release of a view's memory does (section 8), so only this run's own `classify` can evict an alias it holds):
  - every touched 4 KiB page plain and every touched host page `kernel_write`: the program's address;
  - every touched 4 KiB page plain, the host pages mixed, each with an alias and the aliases contiguous: the alias
    (`a + alias_delta`); otherwise end `noalias` (also a store across a kernel-writable and a mixed host page:
    a kernel-writable page carries no alias; on the host's random streams this is most of `noalias` and of the
    zero-progress steps it leads to);
  - a touched page protected, at `a`'s own page: end `protected` (the native retry faults at `a`);
  - a touched page protected after a plain first byte: exact size and not `MASK`, end `straddle` with the first
    byte of the first protected page; otherwise end `protected`.
- Code (section 6, step 1): the 4 KiB page of RIP, and of the instruction's last byte when that lies in the next
  page, through the same table: `exec` bit clear, or `classify` -1, ends the run `fetch`, and the native retry takes
  the execute fault a CPU with 4 KiB pages would (the host page's kernel protection is a union and lets more run).
- The engine never lets a store into a page that is not plain go through the alias, and never sends to the
  program's address a store the kernel would refuse: native execution decides everything else.
- A table entry that comes back with a newer generation empties the table, and a store classified while that
  happened is classified again, so no stale alias survives a window given back by the run's own classify (M6).

## 6. The run loop (`pw_wine_mmu4k_run`)

Holding `virtual_mutex`, per instruction at `regs.rip`:

1. Fetch up to 15 bytes through `pw_wine_mmu4k_read`; on a fault, only to the end of the 4 KiB page; nothing
   readable, or too short to decode: end `fetch`. Code made JIT memory by patch 1127 is mapped RX and readable.
   Before the read, the 4 KiB page of RIP must be `exec` in the table (section 5), and after the decode the page of
   the last byte when it differs; else end `fetch`.
2. Cache lookup by RIP with a byte compare; a miss decodes (`pw_ia32_decode_mode(64)`, `pw_ia32_access`) and
   encodes a slot, through `pw_wine_mmu4k_call`. `BRANCH` and `STACK` rows are emulated or end the run, never run
   in a slot (a slot runs on the run stack).
3. Plans:
   - **verbatim** (no ModRM memory operand, no RSP operand, not emulated, not ending): the slot as it is. This
     covers all SSE/AVX/x87 arithmetic, LODS/SCAS/CMPS and XLAT (reads), CPUID, RDTSC, fences, PAUSE.
   - **memory** (one ModRM operand, RIP-relative allowed): EA from the registers (RIP-relative from the next
     instruction; with 67, truncated to 32 bits); the target by section 5; the slot with the operand rewritten.
     FS/GS overrides end `form`.
   - **emulated** in C, by the table's mnemonic: JMP/Jcc rel8/rel32, JMP/CALL r/m64 (the target loaded as a read),
     CALL rel32, RET, RET imm16, LOOP/LOOPE/LOOPNE/JRCXZ, PUSH r64/imm/r/m64, POP r64, LEAVE, MOVS and STOS with
     or without REP (DF 0 only; 67 ends `form`), in chunks that end at both operands' next 4 KiB boundary. A chunk
     whose destination's first element is refused ends the run with RCX/RSI/RDI at that element; a fault inside a
     chunk sets them from the fault's RCX (elements left). Since protections are per 4 KiB page, a chunk faults
     only at its first element; the partial count is kept for safety but no host test reaches it. An overlapping
     forward MOVS: P6.
   - **ends** `form`: no form, EVEX, XOP, VSIB, `IMPL` writes, SYSCALL/INT/INT3/INTO/ICEBP/UD2/HLT, privileged,
     far control flow, POPF, POP r/m, ENTER, IRET, an RSP-implicit form not listed, moffs, a byte-register store
     that would need a REX it cannot take (AH/CH/DH/BH), a rewritten length over 15, BTS/BTR/BTC with a memory
     operand and a register bit offset (0F AB/B3/BB: the CPU adds the offset to the address, past `[EA, +size)`;
     a shortcut, section 14), and JMP/CALL r/m or RET to a non-canonical target (the CPU's #GP is at the branch,
     before its push or pop: the run ends before it, the native retry raises it). FS/GS overrides decide by the
     decoder's `segment`, in which ES/CS/SS/DS after FS or GS are null prefixes (long mode, AMD APM vol. 3, 1.2.4).
4. After it: RIP advances (emulated branches set it), `retired`++, the idle count resets on an alias store. Exits:
   `idle` at 1,024 instructions since the last alias store, `budget` at 8,192, `yield` at a multiple of 256 when
   `ops.waiters` reads nonzero (another thread waits on the mutex; a count left stale by a waiter that was ended only
   makes runs hand over more often). The run holds the mutex throughout, so its host-page table is never emptied
   mid-run (patch 1136; before, an unlock and relock every 256 instructions emptied it and handed the mutex over 0
   times in 50 on the host).
5. A slot or guarded routine that faults: RECOVER, then end `fault` at that instruction with the registers as before
   it (x86 faults are precise); the native retry raises the fault through Wine's own path.

At the exit: `straddle`, FAULT; otherwise progress made, RESUME at `regs.rip`; none (P3), STEP for the run's first
store (counted `stepfall`, its opcode into `top`).

## 7. Slots and JIT memory

- In a payload the kernel keeps execute only on JIT shared memory (`exec-kernel-2026-10-14`, K1-K8). At init,
  `pw_wine_mmu4k_ps5.c` makes one 64 KiB object (`sceKernelJitCreateSharedMemory("pw-mmu4k", 65536, RWX, &fd)`),
  maps it read-write in the helper's shared-page range (`pw_wine_pages_map`, never where the kernel chooses),
  makes it RWX with `sceKernelMprotect` (0x7, measured) and closes fd (the mapping keeps the object, K6). It is
  charged to flexible memory. Any refusal: the engine stays off and every such store steps.
- A title has no JIT memory (0x80020001): the engine is off there; titles keep 1122's step.
- The engine never executes program code in place: patch 1127 converts the program's code to JIT objects of its
  own; the engine only reads those bytes (step 1 above) and copies the instruction into a slot.
- Slots are 32 bytes, direct-mapped by RIP, written and executed only under `virtual_mutex`, so one global cache
  needs no other lock. Encoding of a memory plan:
  - legacy prefixes kept, except 67 (the scratch holds the final address) and REX (rebuilt);
  - the operand becomes `[scratch]`, `mod 00` with no SIB or displacement; scratch is the first of r8, r9, r10,
    r11, r14, r15 the instruction does not name as a general register (r12 needs a SIB, r13 means RIP-relative);
  - REX.B set (REX added after the legacy prefixes when there was none); VEX2 widened to VEX3, VEX3's B set;
  - an explicit RSP register operand (ModRM.reg, ModRM.rm with mod 3, VEX.vvvv) becomes a second scratch loaded
    with the program's RSP and copied back after;
  - then `ret` (C3). The slot runs on the run stack with every other program register live.

## 8. Aliases: made, used, forgotten

- **Made** (`pw_wine_alias_get`, under `virtual_mutex`, before the alias lock): `backing(H)` gives H's direct
  memory P and the virtual range `[low, high)` around it backed contiguously (a payload dmem run:
  `__wine_ps5_dmem_backing`; a direct section chunk mapped in its view: `__wine_ps5_section_backing`; anything
  else, 0 and `noalias`). The window is `[max(low, H rounded down to 2 MiB), min(high, that + 2 MiB))`. A table
  entry whose direct memory holds `[P, P + 16 KiB)` is reused; else `take` a range (`pw_wine_pages_take`; full:
  evict the least recently used entry, `give` it, retry once), `map` the window's direct memory there CPU
  read-write (`sceKernelMapDirectMemory`, 0x03, fixed: the protection and memory
  type of the view's own mapping), and record it. A refusal gives the range back and counts `map_failures`. The
  range is the helper's own reservation, laid back at every give, as window pages and 1127's scratch are.
- **Keyed by direct-memory offset, not by address**, so a range of the program's that changes backing can never
  reach an old alias. Every eviction, and every forget that gave a window back, bumps the generation, which empties
  every run's host-page table (section 5).
- **Used** only by a run holding `virtual_mutex`. Every release of the memory behind a Windows view holds that
  mutex too, so an alias is never forgotten while a run stores through it.
- **Forgotten before release** (I4): `pw_wine_alias_forget(dmem, bytes)` lays the reservation back over every
  overlapping alias before it returns, and runs inside the dmem engine's `release` op, before
  `sceKernelReleaseDirectMemory`. The release paths:

| Path | How it reaches the forget | Held by (step 2) |
| --- | --- | --- |
| `pw_wine_dmem_unmap` (a free) | `remove_locked` → `forget()` → `ops.release` | replay "alias outlives its memory: free" |
| `pw_wine_dmem_replace` (a decommit, a fresh commit over it) | `clear_runs` → `forget()` → `ops.release` | replays "...: decommit", "...: commit over it" |
| `pw_wine_dmem_protect` (a failed back) | `back()` → `ops.release` of memory it just allocated, which no alias can map yet | by construction |
| exec conversion (`jit_over`, patch 1127) | `__wine_ps5_exec_convert` adopts the range: `ops.release` | replay "...: execute conversion" |
| a demand-paged view taking a range (`__wine_ps5_dmem_file_region`) | replace with 0, then adopt: `ops.release` | replay "...: demand-paged view" |
| a placed GPU mapping (`__wine_ps5_placed_range_bare`, patch 1130) | `__wine_ps5_dmem_file_region`/`_released`: `ops.release` | replay "...: placed GPU range" |
| `__wine_ps5_dmem_lend`, `_keep` | release nothing themselves; the release before them is one of the above | by construction |
| the heap's area (`heap_unmap`) | `__wine_ps5_mmap` with protection 0: `ops.release`; no Windows view, so no alias is ever made there | by construction |
| an alias evicted for room | `give` lays the reservation back; its memory stays live | replay "alias evicted"; `test_pw_wine_alias` |
| a direct section's chunk (`pw_wine_dsection_collect`) | runs in the title, which makes no alias (no JIT memory), and frees a chunk only after every admitted process ended: a helper's alias dies with it | `test_pw_wine_section_dmem` (`admitted_when_mapped`) |
| a direct view's removal (`pw_wine_section_remove`) | unmaps the view's chunks, releases nothing; lookups go by offset; `pw_wine_section_backing` answers 0 after | `test_pw_wine_section_dmem` (`backing_for_alias`) |
| pager eviction | cache pages are file memory, which has no alias: `pw_wine_section_backing` answers 0 for a cache view | `test_pw_wine_section_dmem` (`backing_for_alias`) |
| flexible memory (`flex` engine) | no direct memory, no alias: `__wine_ps5_dmem_backing` answers 0 without `payload_allocate_direct` | by construction |
| an adopt that fails after the caller's own mapping replaced runs (a shared file over runs; an exec conversion racing the heap area for the last table entries) | the runs still name direct memory no longer mapped there: `backing_stale` is set and `__wine_ps5_dmem_backing` answers 0 from then on (stores step; a shortcut, section 14) | `test_pw_wine_dmem_ps5` ("stale backing") |

The replays (`tests/test_crash_replays.c`) run the real engine, glue, pages and alias module on the shared fake
kernel with the console's alias ops as section 8 describes them (backing `__wine_ps5_dmem_backing`, take/give
`pw_wine_pages_take`/`_give`, map `sceKernelMapDirectMemory` 0x03 fixed); each fails with `FK_MAPPED` when the forget
in `release()` is taken out of a scratch copy (checked 2026-10-15, all seven). Step 3 built them in
`pw_wine_mmu4k_ps5.c` as `__wine_ps5_alias_ops` (adding `__wine_ps5_section_backing` as the second source), and the
replays now call those.

- Lock order: `virtual_mutex`, then the dmem engine's lock, then the alias lock, then the pages lock. `forget` may
  run on any thread (inside the dmem lock); `get` calls `backing` before it takes the alias lock.
- Room: 16 windows of 2 MiB (32 MiB) in the helper's 128 MiB shared-page range, beside window pages (up to 64 MiB),
  the frame page (8 MiB), the rings, the slots (64 KiB) and 1127's scratch.

## 9. `WINE_PS5_MMU4K=0`

Read once by `__wine_ps5_mmu4k_init`: a value starting with `0` leaves the engine off (logged once), and every store
beside a read-only page takes 1122's step, for A/B runs. Unset or anything else: on. It travels to helpers as the
other `WINE_PS5_*` diagnostics do.

## 10. Census

The helper's 10 s census (`proton_wine_child.c`), counters as differences per window, on a line of its own after the
server line, which keeps `opened=` (1122's step and system-call openings, `VM_STAT_HOST_PAGE_OPENS`):
`wine-child: mmu4k runs=N insns=N exits=idle:N,budget:N,form:N,protected:N,straddle:N,fault:N,fetch:N,noalias:N,yield:N
stepfall=N stack_full=N stack_damage=N top=MMOO:N,... alias=maps/failures/forgets evictions=N noalias=N` (`top` counts
since the start, a space-saving count). `reported=` is gone. `__wine_ps5_mmu4k_stats` fills, in this order: runs, insns, the
nine exits (yield last, patch 1136), stepfall, stack_full, stack_damage, top[4] (`opcode << 32 | count`), alias maps, failures, forgets,
evictions, noalias; it returns how many it has. As the helper ends (its control socket closed), the loader prints the same
fields once more as `wine-child: mmu4k total runs=...`: the counts since the start, so a probe that ends inside its first
10 s window still leaves its runs and hand-overs on record (l4-payload-vm-contention-2026-10-16-01 left none).

## 11. Host tests

- `tests/test_pw_wine_mmu4k.c` (step 3; `-mgeneral-regs-only`, no sanitizers). The oracle is the Linux host's real
  4 KiB pages. A memfd of 128 KiB (8 host pages) is the direct memory: the program view P maps it, and the real
  `pw_wine_alias` module maps windows of it through the test's ops. The same generated code runs (a) natively, with
  per-4 KiB `mprotect` and a tracker handler that records each access violation (address, RIP, registers, the 32
  bytes around the address at that moment) and opens that page, and (b) with each 16 KiB of P held as 1122 holds it
  and the engine entered by a test signal glue (the Linux twin of section 4.4's, with 1122's step on TF). The
  access violations, the program's #DE/#UD, final registers, RFLAGS, ymm0-15, MXCSR and memory must match; the
  ops clobber ymm0/1/7/8/15, push x87 and load MXCSR 0x7f80 on every call to prove P2. Built 2026-10-15, all
  passing; checked by breaking the engine on purpose, one change at a time (each caught: overlapping MOVS
  without P6, no straddle rule, a store into a protected page let through, no `xrstor64`, no hand-over when
  `ops.waiters` reads nonzero (M5, 2026-10-16), no generation check, zero progress resumed, RSP not copied back, Jcc L and P conditions, RET imm16,
  LOOPE/LOOPNE, JECXZ reading RCX, a 16-bit POP writing 64 bits, an AH store given a REX, CALL r/m not pushing,
  r14/r15 encoded as r10/r11, a Jcc displacement not sign-extended, a 67 address not truncated). Cases:
  - M1 random streams near page edges (stores of 1-32 bytes, SSE/AVX stores, ALU RMW, LOCK forms, Jcc loops,
    PUSH/POP/CALL/RET with the stack in a mixed page, RIP-relative operands, `sub rsp, imm`, `mov rax, rsp`; all 16
    Jcc conditions, LOOP/LOOPE/LOOPNE, JRCXZ/JECXZ, RET imm16, CALL/JMP through a register and memory, PUSH
    imm/memory, 16-bit PUSH/POP, frames with LEAVE, REP MOVSB/STOSQ, JMP/CALL/RET to a non-canonical target, whose
    #GP the handler skips like #DE/#UD): 240 streams of 60 elements (40 of them with #DE/#UD, M4), about 2,800 runs
    and 68,000 instructions;
  - M2 straddle: 8-byte and 16-byte stores across a plain|read-only edge give one fault at the read-only page's
    first byte, before either page changes;
  - M3 partial REP: `rep movsb`/`rep stosq` across a plain|read-only edge and with a source fault mid-chunk:
    RCX/RSI/RDI and memory as native at the fault;
  - M4 #DE and #UD in a slot: the same exception at the same RIP and registers; ymm0-15 patterns intact;
  - M5 a re-protect at a hand-over (a waiter is counted; the test's unlock op at the run's `yield` end makes a
    written page read-only): the next store to it faults natively with its old bytes in place;
  - M6 an alias forgotten before its memory is released (the test's release lays it back first; the next run maps
    a new one; no store lands in the old range);
  - M7 zero progress: an undecodable first instruction, and an alias that faults, end in STEP, never a loop;
  - M8 budgets: the idle exit at 1,024, the budget at 8,192, one lock per run and none mid-run; M8-yield: with a
    waiter counted throughout, every run of the 20,001-instruction loop but the last hands over at exactly 256
    instructions (78 runs hand over, and a 79th ends at the program's exit), results equal to native;
  - M9 encoding: REX insertion, VEX2 to VEX3, an AH store ending `form`, a named r8 skipped, RSP as a second scratch,
    a 67 address; ns per instruction printed (host i5-10600K: 32-40 ns an instruction, entries and exits included,
    over 20,001 instructions in 3 runs);
  - M0 the API: init's checks, 16 run stacks then -1 (counted), enter and signal refusing what is not a run's;
  - M3 also: rep movsq whose source runs into a no-access host page, and copies onto their own source (P6);
  - M10 (review findings, 2026-10-15), each inside a run and faulting as natively: `bts`/`btr`/`btc qword [m], rcx`
    with the bit offset reaching the read-only neighbour (+0x1000), another host page (+0x4000) and back (-0x1000);
    `mov gs:[rbx+0x10], rax` with a DS after the GS (GS base 0x1000 by `arch_prctl`); `call rdx` into a read-write
    data page holding `c3` (one execute fault at the target, the return address pushed; the test's classify makes
    the generated code `exec` 0xf and the region 0). Each of the four fixes, taken out of a scratch copy, fails it
    (M10-bit, M10-gs, M10-exec; the non-canonical check fails M1), checked 2026-10-15.
- `tests/test_pw_wine_alias.c` (step 2; fake ops with a call log over a memfd standing in for direct memory, so
  every alias store is read back at the program's address): before init nothing asked; init's checks; window
  clipping (a run shorter than 2 MiB, a run across a 2 MiB boundary at an offset off it); reuse by offset (the same
  page, its neighbour, a second view of the same memory); the least recently used evicted, given back, generation
  bumped; forget gives back only overlapping windows, generation bumped; a refused map gives the range back; a full
  range evicts once then 0; no backing or a bad answer, 0 and `noalias`.
- `tests/test_crash_replays.c` (step 2): "alias outlives its memory" for a free, a decommit, a fresh commit over it,
  an execute conversion, a demand-paged view and a placed GPU range, and "alias evicted": on the shared fake kernel
  with `FK_MAPPED` (section 8); each fails when the guard is taken out. Since step 3 they drive the console's
  `__wine_ps5_alias_ops` (checked again 2026-10-15: without the forget in `release()`, `FK_MAPPED` on the free).
- Extended in step 2: `test_pw_wine_pages` (take/give, lay-back failure), `test_pw_wine_dmem_regions` (`run_at`),
  `test_pw_wine_section_dmem` (`section_backing`, `dmap_lookup`, a second mapping of a chunk coherent with the view,
  nothing for a cache view or after removal). `test_pw_wine_dmem_ps5` only links `pw_wine_alias.c`: the replays
  hold the forget before the kernel release on the shared fake kernel instead of its own fake.
- Extended in step 1: `test_pw_ia32_decode` (64-bit forms, REX, RIP-relative, VEX sizes, `pw_ia32_access`; since
  2026-10-15 FS/GS followed by DS or ES in long mode, and FS then DS in 32-bit mode, which is DS),
  `tools/ia32-decode-oracle.sh --64` (libLLVM `x86_64`).
- `tests/test_pw_wine_vm.c` (step 4): the report tests are gone; the step-slot and query tests stay.
- `tests/test_crash_replays.c` (step 4), "4 KiB MMU classify": patch 1134's `ps5_mmu4k_classify`, extracted from the
  applied tree (`tests/extract_crash_replays.py`), on the shared fake kernel: a held host page (read-write | read-only |
  write-copy | write-watched) is `plain` 0x1 with an alias of its direct memory (a store through it read at the
  program's address); a kernel-writable one with a write-copy page is `plain` 0xf with no alias; a host page outside
  every view is -1; each call repairs FS first; `exec` is 0 for read-write pages and names an R-X page (0x4). Each
  of three mutations (write-copy through the alias, an alias for a kernel-writable page, no view check) fails it
  (checked 2026-10-15).
- `tests/test_pw_wine_dmem_ps5.c`, "stale backing": a shared file mapped over the middle of a three-page run when the
  table is full; the adopt fails, and `__wine_ps5_dmem_backing` answers 0 there and for every other run after
  (without `backing_stale` it answered 1 for the shared page, checked 2026-10-15).

## 12. Console probes and runs (queued in `docs/CONSOLE_QUEUE.md`; vm-truth32 and vm-truth64 passed in gate-hang-a-2026-10-15-01, the rest not run)

Built in step 4. Each runs on the PC under the system Wine (wine-11.19, a throwaway prefix) first: the PC's 4 KiB pages
need no MMU, so every case must hold there, and does (2026-10-15).

`tests/vm-protect-probe.c`: its default run loses 1123's marks (`tracker_check` no longer allows an early report; the
mixed-watches case, page 0 writable beside a no-access and two read-only pages, now requires no report at all), and
gains modes, each with its own verdict line (`fail` prints the mode's marker):

- `--truth` (marker `PS5_PROTON_VM_PROTECT_TRUTH`; battery `vm-truth32`, `vm-truth64` in `GATE_CASES`):
  - V1 (64-bit) signals: a store into a writable page beside read-only ones, then `lock add` (#UD); another store,
    then `div rcx` with RCX 0 (#DE). The handler sees each at its own instruction with RBX, R8-R15, RAX, RCX, RDX as
    they were, skips it, and ymm0-15, RBX and R8-R15 come back intact.
  - V2 truth: 64 KiB read-only; stores of 1, 2, 4, 8 (64-bit), 16 and 32 bytes into 8 pages, then, re-armed, into 6
    others. The handler requires: a write, at the store's own address, RIP at the store (published by the store's
    asm), RDI and RAX the store's, and all 64 KiB still as before the store; it opens the page. Each written page
    faults exactly once, an unwritten one never, and the memory is what was stored. Then page 0 writable beside a
    no-access page and two read-only ones takes a store of each size and 1,024 single bytes with no fault, and nothing
    beside it changes.
  - V3 (64-bit) straddle: stores of 2-32 bytes split 1, size/2 and size-1 bytes before a read-only page, inside a host
    page (pages 0|1) and across two (3|4): each faults at the read-only page's first byte with all 64 KiB unchanged,
    then lands (30 stores). The 32-bit run (WoW64 keeps 1122's step, whose straddle hole is section 14's) checks V2 only.
- `--speed` (V4, `vm-speed64`, on request; marker `PS5_PROTON_VM_PROTECT_SPEED`): best of three, page 0 writable
  beside three read-only ones: 1,024 4-byte stores (bound 600 us, not applied when `WINE_PS5_MMU4K` starts with 0),
  256 SSE stores, 128 AVX stores, `rep movsb` and `rep stosq` of 4 KiB. PC: 0 us each.
- `--contention` (V5, `payload-vm-contention`, on request): two threads store 100,000 times each into their own
  writable page of one host page while a third makes a read-only page of it read-only every 100 us; an access
  violation fails the probe, a lost store too. It should also record the re-protect latency (how long the third
  thread's protection change waits on `virtual_mutex`): the run ends within 256 instructions once it blocks (the
  `yield` exit), and this latency says whether the waiter or the run's thread takes the mutex next on the console;
  the census's `yield:` count says how often runs ended for a waiter.
- `--rearm` (V6, `vm-rearm64`, on request; marker `PS5_PROTON_VM_PROTECT_REARM`): a thread stores rising numbers into
  page 0; the main thread waits until no handler is mid-flight, makes it read-only, reads it, waits 20 us, reads it
  again: a change with no access violation since the protection returned is a violation. 100,000 cycles; PC: 0
  violations, the page changed after a fault in 99,815 cycles (`mmu4k-probes-pc-oracle-2026-10-15-01`).

`tests/section-view-probe.c`: `tracked_writes` keeps its shape without the PS5NOTIF counter (its rounds are now
`track_rounds`, which resets its counters per case), and `tracked_private` runs the same stores into 1 MiB of
`VirtualAlloc` memory (a payload dmem run on the console), each byte read back as written; the verdict becomes
`PS5_PROTON_LARGE_SECTION PASS cases=3`. PC: 957 tracked faults a case, every byte read back. Whether the alias was
used is the census's `alias=` (above 0), which the probe cannot see.

Runs, each passing before the next: (1) the probe suite; (2) the gate battery on exactly this build
(`build-production.sh`, deploy, `gate.py run`, TLS ports exported); (3) a game workload whose uploads mix 4 KiB
protections in one host page, 60 s: no white blocks, at least 52 fps, `opened` about 0, zero EXITs, census per 10 s;
(4) the same with `WINE_PS5_MMU4K=0`.

## 13. Build steps, in order

Each step ends with `bash tools/host-tests.sh` passing every test and
`PATH=$PWD/.deps/magick-7.1.2-32/bin:$PATH bash tools/build-production.sh` succeeding, both reported with numbers. No
commit, no console.

1. **Long-mode decoder** (4.1): decoder, table attributes, `pw_ia32_access`, 64-bit host tests and oracle. Not
   linked into ntdll yet. Done 2026-10-15 (host only).
2. **Alias sources and the alias module** (4.3): pages take/give, `pw_wine_dmem_run_at`, `pw_wine_dmap_lookup`,
   `pw_wine_section_backing`, `pw_wine_alias`, the forget in `release()`, the two `__wine_ps5_*_backing`, their
   tests; `pw_wine_alias` linked into ntdll. Nothing makes an alias yet (no init, so `forget` finds nothing).
   Done 2026-10-15 (host only).
3. **Engine** (4.2): `pw_wine_mmu4k.{h,c}`, the `.S`, `pw_wine_mmu4k_ps5.c`, `test_pw_wine_mmu4k`, the build's
   compile, link and object checks. Linked, not entered. Done 2026-10-15 (host only).
4. **Patch series** (4.4, 4.6): delete 1123 and its `pw_wine_vm` code and tests, refresh 1125/1127/1128, add 1134
   (with P5's FS repair in the ops), census, descriptor, `WINE_PS5_MMU4K`. `tools/check-patches.sh` clean. Done
   2026-10-15 (host only), with step 5's probes, battery cases and queue (section 12; the probes pass on the PC).
5. **Records**: `docs/PLAN.md`, `wine/patches/README.md` and `docs/CONSOLE_QUEUE.md` updated with step 4;
   `docs/FEATURES.md` waits for a console run (rule 7).

## 14. Estimates and known limits

- Frame rate, an estimate only: about 3,800-5,600 engine stores per frame x about 5 instructions x about 40 ns, plus
  about 20 runs x (entry 45 us + exit trap 40 us, P1) is about 2.5-3 ms per frame: about 50 fps where 1123 gave 56
  and stepping 2. If `insns` dominates, the next step is a basic-block slot cache; if `runs` does, an exit without
  a signal (a restartable exit window checked by the asynchronous handlers).
- Unchanged from 1122: reads of a no-access 4 KiB page beside a readable one do not fault (the GPU-dirty
  readback of the program above); executable and image host pages keep the union rule; WoW64 code, titles, a program's own trap flag or
  hardware breakpoints, and instructions the engine does not run keep 1122's step with its holes (a straddle into
  a read-only page; another thread's write while a host page is open), counted under `stepfall` and `opened`.
- System-call writes into mixed host pages still open the page for the call (`opened`); moving them to the alias
  is a later change.
- A run ends within 256 instructions of a thread that blocks on the mutex (the `yield` exit, patch 1136); with no
  waiter it holds the mutex up to the whole run, 8,192, and P4's signals wait that long too (about 0.3 ms). A longer hold is one step blocked in an op or a slot (an alias map, a section's fstat, a page-in). The
  mutex is not fair, so the run's thread can take it back first through its next fault: a waiter that keeps losing
  waits until the scheduler preempts that thread, which pays a run's exit and entry (~125 us) every 256
  instructions meanwhile. On the host a blocked thread took the mutex first 49-50 times in 50 after an 80 us gap
  (0 in 50 for the old unlock-and-relock); on the console, how often runs end for a waiter (the census's `yield:`)
  and who then takes the mutex are unmeasured (`payload-vm-contention`).
- BTS/BTR/BTC with a memory operand and a register bit offset end the run (`form`, then the step): the engine does
  not offset the classification by the register. Upgrade if the census's stepfall top shows 0F AB/B3/BB.
- After any failed adopt in the direct-memory glue (`backing_stale`), no direct-memory alias is made for the rest
  of the process: its stores step. Upgrade by holding the engine lock across `pw_wine_dmem_can_adopt` and the adopt.
- Unchecked on Zen 2: ES/CS/SS/DS as null prefixes after FS/GS, and the #GP of a branch to a non-canonical target
  reported at the branch (both from AMD's manual and the host's Intel).
- Unproven on the console: where the console's ucontext keeps `uc_sigmask` (the first run's exit checks it and turns
  runs off if the mask did not take, section 4.4); a SIGILL/SIGSEGV taken on a run stack outside the handler (V1 first); a second mapping
  of a payload dmem run (section-view-probe); RWX on the slot object; the per-instruction cost (32-40 ns on the
  host, entries and exits included); the straddle fault address on Zen 2 (the host's Intel reports the read-only
  page's first byte for all 57 straddles of 2-32 bytes it was given, as C4 assumes); whether Zen 2's fast strings
  share the aliased-overlap hazard P6 avoids.
