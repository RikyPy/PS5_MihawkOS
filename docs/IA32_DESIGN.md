# 32-bit (IA-32) Windows programs: design

Local `main`, promoted from `win-32bit` at the person's request. Status (2026-10-07): **M1 done for user-mode code and checked against both LLVM
and the real CPU; M2's integer interpreter matches the CPU; M0 done; M3 done: a 32-bit Windows program
runs natively under Wine's WoW64 in a helper on the console (`pe32-probe.exe` PASS: 32-bit mode, system
calls, SEH, threads and TLS, x87/SSE) and is a gate case. M4 step 1 done: a crossing costs about 0.4–0.7 µs
more than from 64-bit code, down from 4 µs, because a 32-bit TEB now sits where 32-bit code finds it with
the host FS base left alone. M4 step 2 done: a 32-bit program starts 32-bit child processes, has the prefix's
32-bit registry view, plays audio and gets the DualSense through XInput and HID (`wow64-probe`, `hid32-probe`).
M4 step 3 done: a 32-bit program draws with OpenGL (i686 Mesa/Zink) and Direct3D 11 (i386 DXVK) through the GPU
broker, its GPU memory mapped below 4 GiB. M4 step 4 under way: a 32-bit native game reaches active gameplay on the
1 GiB host-visible broker allowance after patch 0955 fixes WoW64's nested hardware-input layout.
A relaunch displayed 4,317 frames, and the person at the console confirmed picture, controls and sound.
The roughly 39-second startup gap remains open. An FNA game's OpenGL build reaches a level; the person confirmed games
with video, input and sound. An earlier DirectX relaunch crashed in Mono and remains to investigate.**

| Part | State | Proof |
| --- | --- | --- |
| Table `src/pw_ia32_isa.def` | structure for all four maps; 943 forms for 32-bit mode: general-purpose integer, x87 (with the aliases the CPU runs), MMX, SSE to SSE4.2, AES, SHA, MOVBE, CRC32, ADX, XSAVE, hint NOPs; since 2026-10-15 also long mode and VEX (1,110 rows: AVX/AVX2, FMA, F16C, BMI1/2), for the 64-bit store engine ([docs/MMU4K.md](MMU4K.md), section 4.1) | 32-bit decodes unchanged: 106,415,808 encodings field-identical before and after; long mode: **LLVM oracle** `--64` PASS over 58,645,650 encodings ([evidence](../evidence/x64-decode-oracle-2026-10-15-01.json)) |
| Decoder, printer | done for those forms; VEX/EVEX/XOP decoded to their length | **LLVM oracle** PASS over 12,486,072 encodings: 2,497,796 identical in length and text, 976,132 VEX/EVEX/XOP lengths identical, 0 unexplained groups. **CPU oracle** (this PC's CPU in 32-bit mode, single-stepped) over 2,576,220 encodings: 0 length disagreements, 0 encodings the CPU runs that we call undefined, 0 forms the CPU rejects, LOCK legality as the table says ([evidence](../evidence/ia32-decode-oracle-2026-10-07-03.json)) |
| CPU runner `tools/ia32-cpu-run.sh`, sweep `tools/ia32-cpu-decode-check.sh` | done | the first M2 tool: any encoding, single-stepped on the real CPU |
| Dump tool `tools/ia32-dump.sh` | done | stays in sync through VEX/EVEX/XOP |
| Left without forms | system, CET and virtualization instructions; GFNI and MOVDIR* (absent on Zen 2) | the oracles' NOFORM groups |
| Interpreter `src/pw_ia32_interp.c` (integer slice) | general-purpose integer instructions; x87/SSE, segment loads, far transfers and CPUID-like instructions return "unsupported" | **CPU oracle** `tools/ia32-exec-check.sh`: 148,144 random-state cases over 374 forms, 0 mismatches in event, EIP, registers, flags or memory ([evidence](../evidence/ia32-exec-check-2026-10-07-01.json)); host test |
| WoW64 on the console (M3) | Wine's own WoW64 with the FS switch, signal entry and image placement the console needs (patches 0945, 0946); i386 modules built, staged and in the probe prefix's `syswow64` | **console** `pe32-probe.exe` PASS in a helper, run `m3-pe32-2026-10-07-12` ([evidence](../evidence/m3-pe32-2026-10-07-12-pe32.json)); gate case `pe32` |
| Crossing cost (M4 step 1) | no FS switch: each WoW64 thread's 32-bit TEB at the low half of its own thread pointer (patch 0947); `tests/cross-probe.c` built 32- and 64-bit times a crossing by kind | **console**: a 32-bit system call 1.9–2.2 µs against 1.2–1.5 µs from 64-bit code (was 5.2 µs); see "M4 step 1 results" |
| The runtime around a 32-bit program (M4 step 2) | 32-bit child processes (0948); the prefix's WoW64 install by wineboot (0949); the PS5-patched PE modules built for i386 too; 32-bit entry points in the audio driver and winebus (0950), each PRX checked to export them; i386 drivers in `syswow64\drivers` | **console**: `wow64-probe` PASS (child, registry, audio, XInput), `hid32-probe` PASS (DualSense over HID); see "M4 step 2 results" |
| 32-bit rendering (M4 step 3) | placed GPU mappings in the broker client (PS5_Vulkan `3e04762`, opted into by a WoW64 process, patch 0951); Mesa WGL/Zink built for i686 and synced into `syswow64`; the i386 DXVK, VKD3D-Proton and d3d8 from the pinned GE archive | **console**: `vk32` (memory at `0xfe0000`, a GPU fill read back), `gl-vertex32` and `d3d11-32` PASS; see "M4 step 3 results" |
| Real games (M4 step 4, part 1) | the launcher starts 32-bit games (in the launcher layout), prefixes finish their setup however a first launch ends (0952), 32-bit allocations stay below 4 GiB (0953), execute faults during a protection change run again (0954), game prefixes get the i386 HID drivers, and a refused direct-memory call is recorded | **console**: a 32-bit native game runs 120 s without a fault, the title stops it; see "M4 step 4 results" |
| Message-processing fix (M4 step 4, part 2) | patch 0955 widens the inner hardware INPUT using the public SendInput converter; no game-specific override | paired input probes pass 60 checks each; PE32 upload completes naturally in 11.4 s with maximum frame 44 ms; the game's gameplay/input/audio confirmed on a clean relaunch |
| Next | qualify repeated DirectX/.NET launches and longer sessions, then reduce the virtual-memory conformance differences | [paired validation](CONFORMANCE.md) |

## Goal

Run 32-bit Windows programs (PE32, i386) in PS5_Proton's production architecture
(one owner title, each Windows program in its own payload helper, OpenGL through
Mesa/Zink, Direct3D through DXVK or VKD3D-Proton), as fast as the console allows,
with every claim backed by a verdict a script can check. 64-bit programs must not
regress: the gate battery passes on every code commit.

## The decision: native execution, crossings engineered, a small exact fallback

| Option | Speed | Correctness risk | Verdict |
| --- | --- | --- | --- |
| **Native** (the CPU in compatibility mode, CS `0x23`) | 100% of i386 native for guest code; each crossing costs two FS switches (~2.8 µs measured by the reference) | the CPU is the semantics: nothing to decode or get wrong | **the engine** |
| Same-ISA translator | 85–93% of native measured by the reference; cheap crossings | a decoder, an emitter and their caches to get right; ~12,000 lines in the reference | built only if M4's numbers prove native's crossings cannot be made cheap enough |
| Interpreter | typically 20–50× slower than native (general figure, not measured here) | small and exact; easy to check against the CPU | **the fallback and the instrument**: where native cannot start, single-step traces, an independent check |

Why native wins here, where an interpreter won in my emulators: those guests were
foreign, slow CPUs (MIPS, PowerPC, ARM), where a JIT must re-create every quirk and
an interpreter has speed to spare. A 32-bit PC program is the host's own instruction
set and expects a full-speed CPU. And in our stack the crossing problem is smaller
than the reference's: OpenGL runs as Mesa/Zink *inside* the process, so GL and
Direct3D both reach the host only through Vulkan calls.

## Architecture

```text
PE32 program + Wine i386 PE modules            guest, IA-32, runs natively (CS 0x23)
  │  system call / Unix call (BOP)
  ▼
pw_wow_cross        the ONE guest↔host crossing: FS switch, state save, counters by kind
  ▼
wow64.dll thunks → ntdll.so, win32u.so, winevulkan.so (+ batch replay) → owner title
                                                     (wineserver, GPU broker, audio, pad)
fallback where native cannot start:  pw_ia32_interp  (same crossing contract)
```

| Part | Where | What it owns |
| --- | --- | --- |
| **WoW64 runtime** | `tools/` build scripts, `wine/patches/` | Wine's i386 PE modules built and staged beside the x86-64 ones; the pinned stack's WoW64 mode turned on; i686 DXVK, VKD3D-Proton and Mesa/Zink DLLs |
| **CPU backend** `pw_wow_native` | upstream Wine `wow64cpu` changed through our patch series; PS5 logic in `wine/ps5/pw_wow_*.c` | entering 32-bit code, BOP service returns, thread init/term, context get/set, the capability check that decides native or fallback |
| **Crossing** `pw_wow_cross` | `wine/ps5/pw_wow_cross.{c,h}` + one small assembly file | the only code that switches FS and saves guest state; per-thread counters by crossing kind (Vulkan, NT call, other Unix call, exception) written into `evidence/<run>.json` |
| **Signal entry** | `wine/ps5/pw_wow_signal.*` | a signal that lands while the guest's FS is loaded restores host FS before any C or TLS access, resolved by a general mechanism that works in a payload helper, never by a fingerprint of one libkernel build |
| **ISA table** | `src/pw_ia32_isa.def` (data) → generated tables | every instruction form: bytes, prefixes allowed, operands and widths, flags read / written / left undefined, memory access, stack effect, control-flow kind, invalid-in-64-bit, privileged |
| **Decoder** | `src/pw_ia32_decode.{c,h}` | bytes → `PwIa32Inst`; the only x86 parser in the repository; reads only the table |
| **Interpreter** | `src/pw_ia32_interp.{c,h}` | one instruction at a time on a `PwIa32State`; semantics as small functions keyed by the table, not one switch |
| **Dump tool** | `tools/ia32-dump.py` (over the decoder, built for the host) | any guest range as decoded instructions, so no agent reads bytes |
| **Vulkan batching** | `wine/ps5/` beside winevulkan's 32-bit thunks | queue Vulkan commands the guest records and send them in a few crossings per frame; serves DXVK, VKD3D and Zink alike |

### Rules the design keeps

- **One parser, one table.** Nothing outside `pw_ia32_decode` matches x86 bytes. The
  interpreter, the dump tool, the test generator and any future translator read the
  same table, so a correction lands everywhere at once.
- **One crossing.** Every guest↔host transition goes through `pw_wow_cross`, so its
  cost is measured in one place and every reduction helps every program.
- **No per-game CPU choice and no option matrix.** The backend is chosen by a
  capability check (native when it can start, the interpreter when it cannot) and
  logged. One diagnostic override, `PW_IA32_CPU=interp`, exists for traces. Nothing
  else changes behaviour.
- **No generated code that rewrites itself**, if a translator is ever built.
- **No `abort()` on the console**: a broken invariant returns a typed `PW_ERR_*` and
  is logged with the guest EIP; host tests assert every invariant.

## Testing: every layer against an oracle

| Layer | Oracle | Check | Where it runs |
| --- | --- | --- | --- |
| Decoder: length and operands | LLVM's disassembler (`llvm-mc --disassemble`, `llvm-objdump`) on the PC | every table row × legal prefixes × ModRM/SIB/displacement forms, generated; plus random byte streams that must never read past the buffer | host test, seconds |
| Decoder: refusals | the table | invalid-in-64, privileged and undefined encodings are classified, never guessed | host test |
| Interpreter semantics | **the real CPU in 32-bit mode** on the PC (a `-m32` harness process) | generated snippets from random register, flag, memory, x87 and SSE states; compare GPRs, EIP, EFLAGS (masked only by the table's undefined bits), the memory window, x87, SSE, MXCSR, and the exception raised and its EIP | host test; seeds recorded; one JSON line per mismatch |
| Crossing contract | host Linux (also has compatibility mode) | a guest that clobbers every register, flag, x87, SSE and YMM state around each crossing kind; the guest sees its state back, the host sees host FS | host test |
| Native backend on the console | the PC run of the same probe | CS `0x23` seen by the guest; TEB reads before and after crossings; SEH with access violation, UD2, int3, divide by zero; x87/SSE/YMM preserved; worker threads; suspend and `GetThreadContext` while in guest code; signals during guest code | console probe `pe32-probe.exe` through `tools/run-console.py` |
| Graphics | the same probes in 64-bit | i686 DXVK and Zink draw the expected frames and read back the expected pixels | console probes, then battery cases |
| Speed | x64 builds of the same sources run natively | 7-Zip, nbench and pi as i386 builds against their x64 builds; a crossing microbenchmark per kind | console, `evidence/` |

Every console failure becomes a host test if it reproduces on the PC, a battery case
if it does not. A probe joins `GATE_CASES` (`tools/run-battery.py`) when its layer lands.

## Milestones

Each ends in a verdict and an evidence record. Console time is spent only on runs
that decide something.

| # | What | Exit criterion |
| --- | --- | --- |
| **M0** | **Measure in our process model** (console probes in a payload helper started through elfldr, no Wine): far jump into CS `0x23` and back; the cost of one `sysarch` FS switch, with and without the full vector save; what happens to a signal delivered while the guest's FS is loaded, and how a host-FS-first entry can be installed with the libkernel our helpers have; free address space below 4 GiB in a helper | an `evidence/` record answering each; **if native cannot start in a helper, the design changes before M3** |
| M1 | ISA table, decoder, dump tool | the decoder agrees with LLVM's disassembler on every generated form; fuzzing finds no out-of-bounds read |
| M2 | Interpreter | zero unexplained mismatches against the real CPU over the generated corpus |
| M3 | WoW64 runtime and `pw_wow_native`, with `pw_wow_cross` | `pe32-probe.exe` passes on the console and becomes a battery case; the 64-bit gate still passes |
| M4 | Crossing counters, Vulkan batching, i686 DXVK and Zink | crossings per frame by kind recorded for a real 32-bit program; d3d and gl probes pass; **the translator decision is made here, from these numbers** |
| M5 | Real 32-bit programs on the default path | the 32-bit installations `PLAN.md` names start, draw, take input, play sound, close and relaunch, each with an evidence record |

## Implementation plan: M1 and M2 (host only)

Order of work, each step ending in a check that runs:

1. **The table** (`src/pw_ia32_isa.def`). Two parts, one file, readable by C (X-macros)
   and by Python (one row per line):
   - *structure*: a 16×16 grid per opcode map (primary, `0F`, `0F 38`, `0F 3A`), one
     code per opcode saying what follows it (ModRM, immediate kind, prefix, escape,
     undefined), laid out like the Intel SDM's opcode maps, so every opcode has a
     length rule before it has semantics;
   - *forms*: one row per instruction form: name, mnemonic, map, opcode (or opcode
     plus register), ModRM extension or exact ModRM byte, mandatory prefix,
     operand-size constraint, operands in the SDM's notation (`Eb`, `Gv`, `Iz`, `Jb`,
     `Ob`...), flags read, written and left undefined, and attributes (lockable,
     repeatable, control transfer).
2. **The decoder** (`src/pw_ia32_decode.{c,h}`): prefixes, maps, ModRM, SIB, 16- and
   32-bit addressing, displacement and immediates, all from the table. Returns
   `PW_OK` (a known form), `PW_ERR_UNSUPPORTED` (well formed, length known, no form
   yet), `PW_ERR_TRUNCATED` or `PW_ERR_MALFORMED`. Refuses VEX, EVEX and XOP for now
   (the fallback's CPUID will not offer AVX).
3. **The printer** (`src/pw_ia32_print.{c,h}`): Intel syntax spelled exactly as LLVM
   spells it, so the oracle can compare text, and so the dump tool's output is what
   every other x86 tool shows.
4. **Host test** (`tests/test_pw_ia32_decode.c`, in `tools/host-tests.sh`): hand-picked
   encodings with expected results, plus random byte streams under AddressSanitizer
   (no read past the buffer, whatever the bytes).
5. **The decoder oracle** (`tools/ia32-decode-oracle.sh`, `tools/ia32_decode_oracle.c`):
   our decoder against LLVM's C disassembler API (`libLLVM`, version recorded in the
   result) over a generated corpus (every opcode of every map × prefix combinations ×
   ModRM, SIB and displacement forms). Verdict and counts as one JSON object;
   mismatches as JSON lines. Known differences between LLVM and the real CPU live in
   `tests/ia32-llvm-differences.txt`, each with its reason: LLVM returns `lock` as a
   separate one-byte instruction, and rejects encodings real CPUs accept (`F1`, the
   `/6` row of the shift groups, `F6/F7 /1`). Where they disagree the CPU decides (M2).
6. **The dump tool** (`tools/ia32-dump`): any byte range as decoded instructions.
7. **M2, the interpreter** (`src/pw_ia32_interp.{c,h}`): one small function per form,
   named after it; and the semantics oracle (`tools/ia32-exec-oracle`): generated
   snippets run in a `-m32` process on the real CPU and through the interpreter from
   the same random states.

### M2 in detail: the interpreter and its oracle

- **State** `PwIa32State`: eight GPRs, EIP, EFLAGS, six selectors with their bases (FS for the
  TEB), and the x87/MMX/SSE state kept as a 512-byte FXSAVE image, the layout Wine's 32-bit
  context already carries, so a context moves between Wine and the interpreter without
  conversion.
- **Memory** through one interface: `translate(address, size, write)` returns a host pointer or a
  fault. Every memory operand of an instruction is translated **before** anything is written,
  so an exception leaves the state exactly as it was (precise exceptions without undo logs).
  Under WoW64 it is a bounds check on the identity-mapped low 4 GiB; in the oracle, a window.
- **One step** `pw_ia32_step(state, memory, &event)`: decode with `pw_ia32_decode`, translate,
  execute, commit; or an event (vector, error code, fault address) with nothing changed.
- **Semantics**: integer operations and their flags in portable C, one small function per
  operation, so the CPU oracle checks an independent implementation. x87 and SSE run on the
  host's own x87/SSE instructions over the loaded FXSAVE image: re-implementing 80-bit x87 and
  IEEE rounding in C would be large and never more exact than the hardware both hosts have.
- **The oracle** `tools/ia32-exec-oracle`: one `-m32` process holding both sides. For every form
  it generates encodings whose memory operands point into a window, runs each from random
  register, flag, memory, x87 and SSE states natively (a trampoline loads the state, runs the
  instruction, and a stub or signal handler stores the result) and through the interpreter, and
  compares GPRs, EIP, EFLAGS (masked only by the table's undefined bits), the window, the
  FXSAVE image and the exception. One JSON line per mismatch; seeds recorded.
- **Which CPU decides.** This PC's CPU is Intel; the console's is AMD Zen 2. Architectural
  behaviour is the same on both, so the PC answers it. Vendor-specific behaviour (SSE4a's
  EXTRQ/INSERTQ, undefined flags, the undocumented x87 aliases) is answered on the console,
  once M0 has native 32-bit code running there; until then those rows stay as they are.

**Committing code.** The commit gate covers every file outside `docs/`, `evidence/` and
`*.md`, so this host-only code is committed only after a console gate run on a build
that contains it (AGENTS.md). Until then it is host-tested and left uncommitted.

## M0 results (2026-10-07, console, payload processes through websrv/elfldr)

Measured by `tools/run-m0-probe.py` (`tools/m0-native32-probe.c`), one mode per payload process, no title
running; evidence `evidence/m0-2026-10-07-01.json` to `-11.json` (`-09`: the first raw-signal run looped on a
wrong ucontext slot and was ended with the probe's `stop` mode, which kills only a process named `m0`).

| Question | Answer |
| --- | --- |
| Selectors in a payload | CS `0x43`, SS/DS/ES `0x3b`, FS `0x13`, GS `0x1b` (FreeBSD amd64's GDT); FS base `0x8ff800080` (above 4 GiB), GS base 0 |
| Free address space below 4 GiB | all 255 slots of 16 MiB from 16 MiB to 4 GiB |
| Does 32-bit code run? | **yes**: a far jump to CS `0x33` ran push/call/imul/ret on a low stack (SS `0x3b`) and returned 42 |
| Cost of a 64→32→64 round trip | **36 ns** (100,000 round trips) |
| Cost of the FS base | `sysarch` GET 949 ns; SET to guest and back **2,723 ns**: the whole of a crossing's cost is these system calls, not the mode switch |
| Does a guest FS base work? | yes: set through `sysarch`, 32-bit code read `fs:[0x18]` = the fake TEB's Self; libkernel_sys's `sysarch` can be called with it loaded |
| FS selector `0x13` in 32-bit code | its base is the low 32 bits of the FS base the thread had at its last context switch (`0xff800080` here), or the base `I386_SET_FSBASE` last set: `AMD64_SET_FSBASE` leaves the descriptor alone. Measured by the `fsreload` mode ([evidence](../evidence/m0-fsreload-2026-10-07-01.json)): after `AMD64_SET_FSBASE` to a guest base, a reload at once read the **stale host** base, after 20 ms of spinning the guest's; after `I386_SET_FSBASE` the guest's at once |
| A fault in 32-bit code, host FS | delivered (SIGSEGV, address 0) to an ordinary `sigaction` handler; recovered |
| A fault in 32-bit code, **guest FS** | **the process dies before the handler runs**, registered through `sigaction` or libkernel_sys's `_sigaction` alike: the signal path touches thread-local storage through FS first |
| **The host FS base seen from 32-bit code** | in compatibility mode the CPU ignores its upper half: with the host base `0x8ff800080` left alone, 32-bit code reading `fs:[0x18]` read `0xff800098`, the low half plus `0x18`; a 64→32→64 round trip with that read took **55 ns** (`fsalias`, [evidence](../evidence/m0-fsalias-2026-10-07-02.json)). After 32-bit code loads `0x13` into %fs, 64-bit code has the low half as its base too; one `sysarch` restores it. `%fs:0` is 0 on the console (no TLS self-pointer there); libkernel's thread pointer is at `%fs:0x10` |
| libkernel's thread pointers | 16 threads alive at once: `0x8ff804080` to `0x8ff840080`, exactly **16 KiB** apart, the main thread's at `0x8ff800080` (`tcbs`, [evidence](../evidence/m0-fsalias-2026-10-07-01.json)) |
| User LDT (`I386_SET_LDT`/`GET_LDT`) | **refused**, EINVAL, for automatic and explicit indexes: not available |
| libkernel in a payload | `libkernel_sys.sprx` (prospero-win's native signal path refuses exactly this identity); by name it exports `sigaction`, `_sigaction`, `sigreturn`, `sysarch`, `sigaltstack`, no `__sys_*` |
| **A host-FS-first signal entry** | **works**: registered with the kernel's own `sigaction` (system call 416, issued by the payload SDK's `syscall()`), the entry ran with the guest FS loaded, restored the host FS through libkernel_sys's `sysarch`, ran a C handler (`printf` included), skipped the faulting instruction, restored the guest FS and returned through the kernel's trampoline and `sigreturn`; the 32-bit code resumed and finished. No libkernel fingerprint is involved |
| The console's `ucontext` | the machine context starts at `ucontext+64` (the SDK header says +16): RIP at +224, CS +232, RFLAGS +240, RSP +248, SS +256, FS/GS selectors at +196/+198; `mc_flags` is 5 (no `_MC_HASBASES`), so `sigreturn` does not restore an FS base: the entry restores it. Found at run time by searching for planted values (the faulting address followed by CS `0x33`), which the probe also uses |

What this decides:

- **Native execution in our helpers is possible**, and the mode switch is nearly free; the design's engine stands.
- **Every crossing costs about 2.7 µs of FS system calls.** Reducing crossings (Vulkan batching first) is the lever;
  making each crossing cheaper needs an FS mechanism other than `sysarch`, and the two candidates measured so far
  (a user LDT; the GDT's FS selector) are closed: no LDT, and the GDT selector carries the host's base.
  *Superseded by M4 step 1:* no switch is needed at all when the 32-bit TEB sits at the low half of the host
  base, which is all 32-bit code sees of it.
- **Signals with the guest FS loaded are handled** by a host-FS-first entry registered through the kernel's own
  `sigaction` system call, with the `ucontext` layout above. In Wine this entry belongs in `pw_wow_signal`: the
  per-thread host FS base must come from somewhere that needs no TLS (the probe used a global), and the
  entry must chain to Wine's own handlers once the host FS is back.
- **M0 is done.** Next: M3, the WoW64 runtime and `pw_wow_native` with `pw_wow_cross` and `pw_wow_signal`.

## M3 plan: Wine's WoW64 on the console

Upstream Wine runs 32-bit code under WoW64 by giving %fs, in 32-bit mode, a selector whose base is the 32-bit TEB:
Linux allocates a GDT TLS entry (cs32 `0x23`), FreeBSD would allocate an LDT entry, and `signal_init_process`
sets `cs32_sel` for Linux and macOS only, so FreeBSD (and the console) has WoW64 off. M0 measured that the console
has no user LDT, that CS32 is `0x33` and SS32 `0x3b`, and that the FS base changes only through `sysarch`. The
port therefore keeps Wine's structure and replaces its FS mechanism. In
`dlls/ntdll/unix/signal_x86_64.c` (our patch series, execution core 0500-0899; PS5 logic in `wine/ps5/pw_wow_*.c`):

| Step | What | Where Linux does it |
| --- | --- | --- |
| M3.1 | build Wine's i386 PE modules (`--enable-archs=i386,x86_64`, host clang) and stage `lib/wine/i386-windows` | `tools/build-wine-runtime.sh`, `tools/stage-wine.py` |
| M3.2a | `cs32_sel = 0x33` for `__PROSPERO__` in `signal_init_process`; mark WoW64 threads (`pthread_teb` = the host FS base, kept by patch 0500 already) | the `__linux__` branch sets `0x23` |
| M3.2b | dispatcher **entry** (syscall and Unix call): for a WoW64 thread, `sysarch(SET_FSBASE, pthread_teb)` before any Unix code | `wrfsbase`/`arch_prctl` where `PW_RESTORE_FS` now repairs a null %fs |
| M3.2c | dispatcher **exit**: for a WoW64 thread, `sysarch(SET_FSBASE, the 32-bit TEB)`; 64-bit PE code never reads %fs, so the guest FS can be set here, before `wow64cpu` jumps to 32-bit code | `movw amd64_thread_data()->fs,%fs` |
| M3.2d | signals in a WoW64 process: register the handlers through the kernel's own `sigaction` (system call 416) with an entry that restores the host FS from `%gs:0x320` (`pthread_teb`; %gs is the 64-bit TEB, which 32-bit code never touches), calls Wine's handler, and sets the FS for the context it returns to (guest when its CS at `ucontext+232` is `0x33`) | M0's `rawsig` |
| M3.3 | `wow64cpu`: its selectors come from `RtlWow64GetThreadContext`, so `0x33`/`0x3b` follow from M3.2a; check its far jumps on the console | `dlls/wow64cpu/cpu.c` |
| M3.4 | `tests/pe32-probe.c` built as i386 PE: exceptions (AV, UD2, divide, int3) handled by SEH, threads and TLS, x87/SSE state across system calls, `fs:[0x18]` | a battery case `pe32` beside `pe64` |

Exit criterion: `pe32-probe.exe` passes on the console through `tools/run-console.py` and joins `GATE_CASES`, and
the 64-bit gate still passes. The crossing counters (`pw_wow_cross`) and Vulkan batching are M4.

## M3 results (2026-10-07, console, the production launcher layout)

**The exit criterion is met.** `pe32-probe.exe`, an i386 PE with no C runtime (`tests/pe32-probe.c`), passes in a
helper process through `tools/run-console.py` (run `m3-pe32-2026-10-07-12`), and it is the gate case `pe32`;
the gate passed with it, all ten cases, on build `11e4dc73` (`gate-ia32-m3-2026-10-07-02`, [evidence](../evidence/gate-ia32-m3-2026-10-07-02.json); there `Sleep(0)` took 34,226 ns). What the probe printed on the console:

| Check | Result |
| --- | --- |
| mode | CS `0x33`; `fs:[0x18]` = `NtCurrentTeb()` = `0x7ffc2000` |
| system calls | 2,000 `Sleep(0)` from 32-bit code return; **33,109 ns each** |
| SEH | an access violation at address 8, an integer divide by zero and an `int3`, each caught by a vectored handler and resumed after the instruction |
| threads | 4 of 4 threads see their own TEB through `%fs` and their own TLS value |
| FPU | x87 and SSE values kept across system calls |

Wine's own WoW64 runs unchanged above the console's FS mechanism. Each step below was found on the console,
in the run named, and fixed at its cause:

| Run | What the console showed | Cause | Fix |
| --- | --- | --- | --- |
| `-03` | `cannot run "C:\pe32-probe.exe" in this process: c000007b` | `WINEARCH=wow64` (which makes Wine run an i386 image in this process) never reached a helper: a helper inherits a fixed list of variables | `WINEARCH` on that list (`wine/ps5/pw_wine_process.c`); the title sets `wow64` |
| `-04` | SIGSEGV in libkernel's `pthread_getspecific`, reading `0x1c8` | Wine's signal handler found its TEB through `NtCurrentTeb()`, which is `pthread_getspecific()`, which reads `%fs:0x10`: the 32-bit TEB | `init_handler` reads the TEB at `%gs:0x30` (0945) |
| `-06` (`+heap,+virtual`) | the process heap's commit asked for `0x8005446b0` bytes with protection `0x4ad4a0` | at the system call return `%rsp` points into the syscall frame; the FS switch's `call sysarch` there overwrote the saved registers it then restored | the switch runs on the user stack, below `frame->rsp` (0945) |
| `-07` | a 32-bit read of `0xff80c098` | wow64cpu reloads `%fs` (`0x13`) when it enters 32-bit code with a full context, and the selector's descriptor still held the host base: `0xff80c080 + 0x18` | measured with M0's `fsreload` mode (table above); the guest base is set with `I386_SET_FSBASE`, which updates the descriptor too (0945) |
| `-08` | 32-bit ntdll: `RtlImageNtHeader(NULL)`, then an unhandled read of address 4 | the exe was mapped at `0x1000000000`, the console's reserved area above 4 GiB, so the 32-bit PEB's `ImageBaseAddress` was its low half, 0 | a 32-bit image is held below the WoW64 limit (0946) |
| `-09`, `-10` | `could not load kernel32.dll, status c0000135` | the 32-bit loader takes a system DLL only from the prefix, and the probe prefix (made before the i386 build) had an empty `syswow64` | `tools/stage-wine.py` installs the i386 modules there as wineboot would; `tools/deploy-wine.py` keeps them current |
| `-11` | the probe exited 0, no verdict | in a helper, standard output does not reach the run's log | the probe also writes through `OutputDebugString`, as `wine-probe.c` does |

Also from this milestone: `tools/run-console.py` asks a run that has not returned 25 s before its watchdog to go
back to the launcher through the title's own stop, instead of leaving it holding the console (`-05` did).

**Open after M3, with what is known:**

- **The crossing costs 33 µs**, against about 1 µs for the same probe under Wine on Linux, where the FS switch
  is a selector load. The two `sysarch` calls account for about 2.7 µs (M0); the rest is not yet broken down,
  and the run had Wine's probe trace channels on. M4 measures it per crossing kind before anything is batched.
- **32-bit child processes are refused**: the console's `NtCreateUserProcess` starts only AMD64 images in a new
  helper. A 32-bit program cannot start another process yet, and wineboot cannot run its 32-bit `rundll32`, which
  is what installs a prefix's WoW64 side on Linux.
- **The prefix's 32-bit registry view** (`Wow6432Node`, from that install) does not exist yet. The probe prefix
  has the `syswow64` files only; a game prefix created from it gets them, an existing one does not.
- `tests/test_pw_memory_service.c` fails on this host at line 81, as it does when built from a clean `HEAD`
  (`7b65534`): it predates M3 and is not caused by it.

## M4 step 1 results (2026-10-07, console): what a crossing costs, and the FS switch removed

`tests/cross-probe.c`, built as `cross32-probe.exe` and `cross64-probe.exe`, times 4,000 calls of each kind; run
in a helper with `WINEDEBUG=-all,+debugstr` and under the pinned Wine on this PC (the oracle). Nanoseconds per
call:

| Kind | Linux 32-bit / 64-bit | console 64-bit | console 32-bit, FS switched (0945) | console 32-bit, TEB aliased (0947) |
| --- | --- | --- | --- | --- |
| `NtQueryPerformanceCounter`, a system call | 355–383 / 238–857 | 1,187 / 1,458 | **5,213** | **2,204 / 1,856** |
| `NtClose` of no handle, a server request | 11,779–17,169 / 11,450–20,859 | 30,691 / 32,613 | 35,097 | 31,227 / 31,877 |
| `NtYieldExecution` | ~1,050 / 1,050–3,362 | 34,428 / 43,863 | 40,818 | 48,138 / 45,029 |
| `Sleep(0)` | 1,002–3,623 / 899–1,041 | 34,017 / 34,779 | 33,332 | 34,702 / 34,469 |
| `NtGetCurrentProcessorNumber` (a server request on the console) | 319–357 / 196–930 | 27,174 / 35,667 | 40,443 | 29,486 / 33,414 |

What this says:

- **The 33 µs `Sleep(0)` of M3 was not WoW64.** A 64-bit `Sleep(0)` costs 34 µs on the console too: yielding
  is that slow there, and a server request costs about 31 µs (in-process server) against 12–17 µs on Linux.
  Both are costs every program pays, recorded here for the 64-bit side.
- **The FS switch was the WoW64 cost: about 4 µs per crossing.** Patch 0947 removes it. In compatibility mode
  the CPU ignores the upper half of the FS base (M0 `fsalias`), so each WoW64 thread's 32-bit TEB is placed at
  the low 32 bits of its own libkernel thread pointer, and 32-bit code finds it through %fs while the host base
  stays in place. Measured after: pe32-probe's TEB at `0xff80c080` for the thread pointer `0x8ff80c080`, and
  a 32-bit system call 0.4–0.7 µs over the 64-bit one. In the gate run (`gate-ia32-m4s1-2026-10-07-01`, all ten
  cases passed) every one of the process's 7 threads was placed, none fell back: `0xff80c080`, then
  `0xff810080` to `0xff824080`, 16 KiB apart, the probe's four workers among them.
- How it is built (0947): the process owns a window at `0xff800000` for the aliases (thread pointers are 16 KiB
  apart, M0 `tcbs`), and TEB blocks for WoW64 threads go below it so `WowTebOffset` reaches; each thread moves
  its 32-bit TEB there as it starts and the creating thread waits for it; the entry checks libkernel's thread
  pointer at `%fs:0x10` and calls `sysarch` only when 32-bit code had loaded %fs again (wow64cpu does for a full
  context); a thread whose alias cannot be placed keeps switching FS. wow64's `ThreadBasicInformation` and
  `RtlWow64GetThreadSelectorEntry` read the TEB's place from the thread's own TEB (for another process's
  thread they still assume the usual offset).
- **A hardware detail found on the way:** without the `sysarch`, the kernel returns from system calls with
  SYSRET, which on AMD keeps the stack segment's cached descriptor from kernel mode; the next 32-bit `ret`
  read `[sp]` with 16 bits (address `0xfb50` for esp `0x0022fb50`, run `m4-alias-2026-10-07-01`). The exit now
  loads SS from its selector again.
- Left for later: the 0.4–0.7 µs still above 64-bit code (wow64's thunks); the 34 µs yield and 31 µs server
  request, which 64-bit programs pay too.

## M4 step 2 results (2026-10-07, console): the runtime around a 32-bit program

What a 32-bit program needs beyond its own process, checked from 32-bit code by `tests/wow64-probe.c` (gate case
`wow64`) and `tests/hid-probe.c` built for i386 (battery case `hid32`). Each failure was found on the console and
fixed where it arose:

| Need | What was missing | Fix | Console result |
| --- | --- | --- | --- |
| Child processes | the console's `NtCreateUserProcess` started a helper for an AMD64 image only | 0948: an i386 image too when `WINEARCH=wow64` | a 32-bit child of a 32-bit program returned 32 |
| The prefix's 32-bit side | wineboot updates a prefix only when `wine.inf`'s time changes, so a prefix made before the i386 build never got its `Wow64Install` | 0949: the prefix's timestamp records the WoW64 install (`<time> wow64`); an older one is updated once more | wineboot ran `syswow64\rundll32.exe ... Wow64Install` (which itself started a 32-bit `iexplore.exe`); the 32-bit view reads `CurrentVersion`. The one update took about 100 s; later launches 9 s |
| PS5 patches on the 32-bit side | the patched PE modules were built for x86_64 only, so 32-bit programs loaded unpatched ntdll, kernelbase, xinput, the audio driver | the patched tree is configured `i386,x86_64` and its 24 patched modules are built for both and staged into the runtime and `syswow64` | XInput: pad 0 connected, pad 1 absent |
| Unix libraries' 32-bit entry points | `wineps5` (audio, 37 entries), `winebus` (20) and `win32u` (1) had none; win32u's is its init, which only the 64-bit side calls (upstream's design) | the audio driver's from Wine's OSS driver; 0950 for winebus (4 entries carry pointers) | — |
| …exported | a PRX's exports are listed by hand: the audio driver's 32-bit table was defined but not exported (c0000139) | listed; the build now refuses a PRX whose Unix call table is not in its descriptor | — |
| Audio buffers below 4 GiB | the driver never set its WoW64 `zero_bits`, so stream buffers landed above 4 GiB and winmm's `memmove` wrote to a truncated pointer | `process_attach` sets it, as the OSS driver's does | 1 device, 200 ms played |
| 32-bit HID drivers | the in-process HID drivers load from `system32\drivers`, which a 32-bit process sees as `syswow64\drivers`, empty | the i386 drivers staged there | DualSense 054c:0ce6, a 64-byte report read |

Runs: `m4s2-2026-10-07-01` to `-04`. Known limits: a thread of another process still has its 32-bit TEB reported at
the usual offset (step 1); a 32-bit program's prefix update happens on its first launch after this runtime.

## M4 step 3 results (2026-10-07, console): 32-bit rendering

A 32-bit program reaches the GPU the way a 64-bit one does, through winevulkan and the title's GPU broker, with one
difference: every CPU mapping of GPU memory must lie below 4 GiB. Measured first with `tests/vk-probe.c` built 32-
and 64-bit (a device, host-visible memory mapped, a GPU fill read back through the mapping):

| Run | 64-bit | 32-bit |
| --- | --- | --- |
| before (`m4s3-vk-2026-10-07-01`, `-02`) | PASS, memory at `0x3000000000` | `vkMapMemory` failed, `VK_ERROR_MEMORY_MAP_FAILED` |
| after (`m4s3-render-2026-10-07-03`) | PASS | PASS, memory at `0xfe0000` |

Wine already did its part: the broker's device advertises `VK_EXT_map_memory_placed` (alignment 16 KiB), so win32u
reserves an address below 4 GiB and asks the driver to map there. The driver's platform layer in the broker client
answered no: `radv_ps5_memory_map_at` was a stub, and the client closed each allocation's shared-memory descriptor
once mapped. PS5_Vulkan `3e04762` keeps the descriptor and maps it again at the placed address (the GPU keeps the
first address), but only in a process that asks (`ps5_gpu_client_keep_sources`): a payload process has a few hundred
descriptors (636 measured), which a 64-bit program with many allocations would otherwise spend. A WoW64 process asks
(patch 0951 sets `WINE_PS5_GPU_PLACED`; `native/proton_gpu_client.c` passes it on); 64-bit processes run as before.

On top of that:

- **OpenGL:** Mesa's WGL over Zink built for i686 from the same pinned Mesa revision and compiler archive
  (`tools/build-mesa-windows-zink-i686.sh`), staged in `lib/wine/mesa/i386-windows` and synced by the title into a
  prefix's `syswow64`, where the existing `opengl32=n,b` override chooses it. `gl-vertex32-probe` PASS (7.5 s).
- **Direct3D:** the i386 DXVK (`d3d11`, `dxgi`, `d3d10core`, `d3d9`, `d3d8`) and VKD3D-Proton from the pinned
  GE-Proton archive, staged into `syswow64`. `d3d11-32-probe` PASS, frames and a readback (129 s).
- Before the broker change both reached their first `vkMapMemory` and stopped there (`m4s3-render-2026-10-07-01`):
  nothing else stood in the way.

Gate cases `vk32` and `gl32` keep the mapping and Zink checked on every commit; `d3d11-32` is a battery case.

## M4 step 4 results so far (2026-10-07, console): a 32-bit native game

The library holds a 32-bit native game (entry 2: a native i386 program, large-address-aware, relocatable, loaded at `0x590000`
because a helper's carrier executable sits at `0x400000`, see below) and an FNA game in two builds (entries 3, 4: .NET, IL-only and
32-bit-required). Every fix below was found by a run of the native game and is general:

| Run | What happened | Cause | Fix |
| --- | --- | --- | --- |
| 2026-10-07 run 1 (records removed from the tree; in git history) | the launcher refused the game | a guard from before WoW64: "64-bit games only" | a 32-bit game starts when the runtime ships its i386 modules, always in the launcher layout (WoW64 rests on the payload loader and helper address space) |
| `-02` | the game never started | its first launch was making the prefix (about 90 s) when the 60 s budget ended it; wineboot had already written the prefix's timestamp, so the half-made prefix would never be finished | 0952: the timestamp is written after the update |
| `-03`, `-04` | a write fault at `0x43000000` (then `0x42c00000`) in the game | (`-06`, `+virtual`) the game's aligned allocations came through wow64 with no highest address, and the console's allocator put them above 4 GiB (`0x1043400000`): the game kept the low half | 0953: no highest address is the 32-bit process's own limit |
| `-03` | winebus could not load in the game | wineboot installs no drivers in `syswow64\drivers`; the probe prefix had them from staging, a game's did not | the title syncs the i386 HID drivers there at each launch |
| `-08` | an execute fault in win32u, 2 s in | the game hooks code (5-byte patches made `PAGE_EXECUTE_READWRITE`, then restored); executable protection takes two calls on the console, and between them the page cannot be executed by a thread running it | 0954: such a fault waits for the change and runs again |
| `-07`, `-09` | no fault in 120 s and 90 s; one frame, then 37 s without | Zink cannot allocate host-visible memory: the game's textures total 344 MB (a 4096×4096 one is 64 MB) and each upload stages through host-visible memory, of which the broker grants a process 128 MiB | open: the broker's limit (PS5_Vulkan `PW_GPU_LIMIT`) |

Also from this step: a refused direct-memory call is recorded (which call, where) and printed in the helper's census,
where a count alone had said nothing; the first crash's single refusal turned out unrelated.

Subsequent paired validation on the working tree's 1 GiB broker host-memory allowance:
six 4096×4096 textures (384 MiB) uploaded and verified in both PE32 and PE64, without an allocation refusal.
PE64 completed in 9.9 seconds. PE32 reached a 35.8-second whole-frame stall while processing messages,
logged repeated hardware-message-type-zero errors, and completed only after watchdog intervention;
the corrected runner rejected its late PASS. This separates successful large uploads from the remaining
PE32 presentation/message problem. It does not qualify either game's gameplay.
See [CONFORMANCE.md](CONFORMANCE.md) and
[the repeated paired probe](../evidence/conformance-upload-analysis-2026-10-07-02.json).

Patch 0955 subsequently isolated and fixed the malformed WoW64 input crossing. Both
architectures pass the focused 60-check HID/device-change probe. The PE32 upload run
now completes naturally in 11.4 seconds, maximum whole-frame time 44 ms, with no
unknown-message-zero errors. The 32-bit native game's repeat run shows active gameplay in two captures,
4,317 displayed frames, human-confirmed controls and sound, and normal return to the
launcher. This qualifies that short session and relaunch; it does not qualify every
32-bit game or resolve the remaining startup gap and memory API differences.

## A fixed-base program at 0x400000: the carrier's image (2026-10-19, console)

A 32-bit program linked `/fixed` at `0x400000` (old installers, such as many NSIS setup programs) cannot map in a helper
(c0000018), and MihawkOS refused it before Wine started (`pw_pe_arch_fixed_base32`, NOTICE_FIXED_BASE32). What holds that
range is not our helper: `proton-payload-child.elf` is position-independent and websrv's elfldr maps it at
`0x200000000`. It is the carrier elfldr starts every payload in, Sony's `/system/vsh/app/NPXS40112/eboot.bin`, which the
kernel places at `0x400000` and whose code never runs (elfldr takes the process over at exec). It cannot be relinked, so
the earlier plan to link the helper high was moot; the owner chose to probe giving the carrier's image back instead.

`tools/run-carrier-image-probe.py` (`tools/carrier-image-probe.c`, a payload in that carrier, no title running):

| Run | What it showed |
| --- | --- |
| `carrier-image-2026-10-19-01` (list) | Below 4 GiB a helper holds only the carrier: `0x400000`-`0x584000` (1.5 MiB), one module `eboot.bin` in four segments: text `+0x54000` execute-only, two read-only (`+0x1c000`, `+0x4000`), data `+0x110000` read-write. The after-check's load of `libSceSystemService.sprx` never returned: the kernel's debug-event signal (`mDBG ... 0xa0020101`) stopped the process, and SIGKILL does not end it (state 4 until a restart). Not a helper path; the check now loads `ntdll.prx`. |
| `-02` (list) | The same map; a thread ending in `pthread_exit`, a signal, and the helper's own first load (`ntdll.prx` from the runtime) all succeed. |
| `-03` (list, then release) | The control as `-02`. Release: the four segments unmapped with status 0, nothing left below 4 GiB, `0x400000` mapped read-write for `0x184000` bytes at exactly that address without replacing anything, read back with 0 wrong; then the thread, the signal and the `ntdll.prx` load said what the control said, and the kernel logged `carrier calls exit() exit_value=0`: no fault, no core dump. |

So a helper can give the range back before Wine starts. Two things the fix must keep in mind: libkernel's module list
still names `eboot.bin` with its old segments after the release (any reader of handle 0 would see Windows memory there),
and only ordinary read-write memory was taken at `0x400000` here; executable PE pages there are Wine's own path
(proven at `0x590000` above).

`-04` repeated `-03` in a helper's order (the release, then the thread, the signal and `ntdll.prx`'s load, then the
map below 4 GiB, then `0x400000`): after the load nothing lay below 4 GiB, so a module does not land in the freed range.

The fix built on that (`native/proton_payload_child.c` `carrier_release`, the rule `pw_prx_release_low_image`, the
refusal removed, gate case `fixed-base32`) failed on the console, `fixed-base32-2026-10-19-01`:

| What | Measured |
| --- | --- |
| the release | all 16 helpers of the session: `carrier image refused=0 end=0x584000 errno=0` |
| `syswow64\rundll32.exe` (new prefix, `Wow64Install`) | loaded at `0x400000` (at `0x110000` in 92 earlier runs); 1 s after its HID bus line and a `ThreadWow64Context` set, `mDBG: Sending signal(... signo: 0xa0020328)`, SYSTEM_XO_VIOLATION; the process stays stopped (state 4), SIGKILL held |
| the `virtual-query32` probe | loaded at `0x400000`; stopped the same way at the same point |
| the session | hung behind the stopped helper; the runner bailed at 82 s; the title's own stop request ended in the title's SIGSEGV |

Execute-only memory is not new to Wine here: payload code runs from execute-only aliases, and a read of one faults
and is converted (`exec_protection`, 1049 conversions in that run). What is new is the address: libkernel still
lists `eboot.bin` (handle 0) with its execute-only text at `0x400000`-`0x454000` after the unmap, and the hypothesis
is that a fault there is taken for a read of system code (the kernel's debug stop, as the 2026-10-07 console-memory
payload's read of libkernel's text) instead of the signal Wine converts. Unproven; the next probe reads an
execute-only page at `0x400000` and at `0x590000`. The refusal stays until a fix passes.

`carrier-xo-2026-10-19-01` (after a console restart; four processes, each releasing the image first) disproved that
hypothesis. A first read of fresh anonymous memory, a first read of a file mapping, and a JIT page made as Wine makes
executable memory (contents written, read-only, JIT object mapped over, read+execute, query `0x5`), run and then read,
behaved alike at `0x590000`, in the old execute-only segment (`0x400000`-`0x430000`) and in the old other segments
(`0x458000`-`0x488000`): no fault, no stop. `sceKernelStopUnloadModule(0)` is refused (`0x80020016`) and the stale
`eboot.bin` record stays. So the stop in the real helpers comes from something these steps do not do; not yet tested
there: a write fault on Wine's code page, a first execute fault, direct memory (helpers use it; a payload gets it only
while a title runs), and 32-bit execution.

`carrier-xo32-2026-10-19-01` ran 32-bit code (compatibility mode, `cs=0x33`) from JIT pages made as Wine makes them, at
`0x590000` and at `0x400000` after the release: both returned 42, no stop. So the range runs code once it is JIT
memory, and the fix converts an executable range there eagerly: `__wine_ps5_set_payload_low_image` records the released
range and `__wine_ps5_mprotect` converts a protection change with execute inside it to JIT memory before anything
runs there (`wine/ps5/pw_wine_dmem_ps5.c`).

With that, `free-0x400000-cases-2026-10-19-01` (build `778a2bdd`, every helper releasing) passed `fixed-base32`, `pe32`
and `virtual-query32` (with a new prefix's setup) but failed `wow64`: its program, relocatable, now loaded at
`0x400000`, and its 32-bit audio and controller drivers failed to load (`mmdevapi ... c0000135`, `winebus ... c0000135`).
The control on the release build `9901998d`, same console, prefix and pad (`free-0x400000-control-2026-10-19-01`),
passed: the program at `0x110000`, `audio devices=1 played=1`, `xinput pad0=0`. So the release must not apply to
relocatable programs.

**The fix (2026-10-19): only a fixed-base image's helper gives the carrier back.** The title decides before the spawn:
`pw_wine_process.c` asks the title's `pw_ps5_set_fixed_base_lookup` callback (`title_fixed_base`: the image's PE header,
`pw_pe_arch_fixed_base32`) about a started process's image or the initial client's `argv[1]`, and sets
`PW_CHILD_FIXED_BASE` in that helper's bootstrap. The loader releases the carrier only when the flag is set, after the
bootstrap and before Wine starts, and logs `carrier image kept` or `carrier image released for a fixed-base image`;
the eager conversion then applies only in that helper. Every other helper is exactly as on the release build.

| Run (build `93357310`) | Result |
| --- | --- |
| `free-0x400000-narrow-2026-10-19-01` | 5/5 PASS: `wow64` at `0x110000` with `audio devices=1 played=1` and `xinput pad0=0`; `pe32`; `pe64`; `virtual-query32`; `fixed-base32` at `0x400000`. Only `fixed-base32`'s helper released the carrier; every other helper of every case kept it. |
| a game launcher's NSIS installer, run twice with `/S` (on the first fix build and on `93357310`; the program is not named here, and its two records are kept outside the public tree) | Fixed at `0x400000`, it loaded at `0x400000` and installed its program files; 1 helper released the carrier, 12 kept it; the session ended with launcher status 0 and no stop. **The runner's verdict on both runs is FAIL:** in the installer's helper, one eager execute conversion in the released range failed (`exec_protection convert_failed=1`) and fell back to the ordinary path. The install completed; the cause of that conversion failure is open. |

Known limits: audio and controller inside a program at `0x400000` are not measured (`-cases-01` suggests they fail
there; an installer does not need them); an installer's windows (not `/S`) are not yet driven; the full gate battery
on this build is the release's.

## Open questions (answered by M0, not by reasoning)

- Does compatibility mode work in a payload helper as it does in a title?
- Which libkernel does a helper have, and how can its signal entry be made to restore
  host FS first without binding to one build?
- How much of the 2.8 µs is the `sysarch` system call itself, and how much the state
  saved around it?
- Is there a cheaper legitimate way to switch FS than `sysarch`? (Unknown; measured,
  never assumed.)
