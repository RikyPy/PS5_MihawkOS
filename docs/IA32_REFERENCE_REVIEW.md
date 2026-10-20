# Review: prospero-win's 32-bit (IA-32) execution, as a reference

Reviewed: [prospero-win](https://github.com/mpereiraesaa/prospero-win) at `a4d3e73`
(2026-10-07), cloned read-only into a scratch folder. No code was copied into this
repository and none will be: this is a reference, and our own design is
[`IA32_DESIGN.md`](IA32_DESIGN.md). The sibling `../prospero-win-main/` is an older
snapshot: it still says the console refuses 32-bit compatibility mode, which is no
longer true (below).

Nothing here was run on the console by us. Numbers quoted are prospero-win's own
measurements, cited by file.

## What it has: two CPU backends for Wine's WoW64

Both plug into Wine's standard WoW64 CPU contract (`BTCpu*`, the interface
`wow64cpu.dll` implements), which keeps every Windows service Wine's own.

| Backend | What it is | Size | Status (2026-10-07) |
| --- | --- | --- | --- |
| `wow64native.dll` (`wine/wow64native/`) | **Native**: the CPU runs i386 code in compatibility mode through the console's GDT selectors (CS `0x23`), derived from Wine's `wow64cpu` | ~1,070 lines | default for 32-bit games since `73675f7` (#432), with 32-bit Vulkan batching |
| `wowprospero.dll` (`wine/wowprospero/`, `src/pw_x86_*`) | **Dynamic binary translator** (DBT): i386 blocks re-encoded as x86-64 | ~12,300 lines, plus ~11,900 lines of tests | fallback, and still the default for games whose profile says `graphics = opengl` |

The per-game choice is made by the launcher from the game's profile:
`[runtime] cpu = native | translator`, defaulting to native unless
`[application] graphics = opengl` (`docs/WINE_PS5_BUILD.md`, game profiles).

## The facts it established (worth reusing as facts, re-measured by us)

| Fact | Where |
| --- | --- |
| The console runs i386 code natively in compatibility mode with its own GDT selectors; native Minesweeper, worker-thread TLS/TEB checks, and access-violation/UD2 recovery with x87/XMM/YMM preserved passed on hardware | `docs/native-wow64-provider.md` |
| User-mode FSGSBASE is disabled (SIGILL); FS and GS bases change only through `sysarch` | `docs/WINE_INTEGRATION.md` |
| So every guest→host crossing on the native backend switches FS twice through `sysarch`: about **2.8 µs per crossing, about ten times the translator's** | commit `73675f7`; `docs/WINE_PS5_BUILD.md` |
| A signal can arrive while the guest's FS is loaded; the entry must restore host FS before any C or TLS access, which needed raw signal registration that bypasses libkernel's own trampoline | `docs/native-wow64-provider.md`, patch `0883` |
| The same-ISA translator reached 85–93% of native on the console (7-Zip, nbench, pi; FW 12.02, estimated against x64 builds) and 91–98% on a PC | `docs/DBT_BENCHMARK.md` |
| Signal `ucontext` layout, 16 KiB pages, 64 KiB-aligned fixed reservations honoured, every 16 MiB slot from 16 MiB to 4 GiB free at title start | `docs/WINE_INTEGRATION.md` (platform table) |

## Ideas worth taking (as ideas, re-derived and re-tested)

1. **Plug into Wine's WoW64 CPU contract** instead of reimplementing services: a
   32-bit compatibility gap is then an execution gap, never a missing `Nt*` call.
2. **Native first.** The CPU is the most exact and fastest implementation of IA-32
   there is. Their switch to native-by-default is the right direction.
3. **Crossings are the cost to engineer.** Batching DXVK's Vulkan calls into a few
   crossings a frame (`wine/ps5/pw_vk_command_stream.*`, `pw_vk_wire.*`) attacks the
   right thing.
4. **For a translator, if one is ever needed:** pin the eight guest registers in host
   registers across linked blocks; identity-map the guest below 4 GiB and let a host
   fault on an access stand for the bounds check (a per-block fault table maps it back
   to a precise guest EIP); keep guest x87/SSE as the host's state while translated
   code runs; protect writable code pages on the host and invalidate on the write
   fault, falling back to source checks for pages that fault too often; invalidate
   exactly the unmapped extent; mask CPUID so the guest never sees AVX or XSAVE.
5. **Measure against native with public benchmarks** (7-Zip, nbench, pi), as box86,
   box64 and FEX publish.

## Where it falls short of what we need

### Architecture

1. **Three decoders for one instruction set.** The original emitter
   (`src/pw_x86_block.c`), the re-encoder (`src/pw_x86_reencode.c`, its own
   `decode()`) and the host-exec fallback (`src/pw_x86_hostexec.c`, its own planner)
   each parse prefixes, ModRM and SIB by hand. Each has its own coverage and its own
   bugs; a fix in one is not a fix in the others.
2. **No single description of instruction semantics.** Which flags an instruction
   defines and reads is written inline as magic masks (`flags_def=0x8d5`) inside
   the decode chains, so nothing can be generated or checked from it.
3. **One function of about 2,000 lines.** `pw_x86_translate_opts`
   (`src/pw_x86_block.c`, lines 1607–3652) decodes and emits in one `if`/`else if`
   chain over opcode bytes. An agent cannot change one instruction from one place.
4. **Two translation backends with different register contracts**, bridged through
   "canonical entries"; with `native_fp` they never link to each other. The older
   emitter survives for whatever the re-encoder does not take. That is accretion,
   not design.
5. **A configuration space nobody can test.** `PwX86TranslateOptions` has 15 fields,
   many valid only in combination ("With `unbounded_chains`…", "With
   `superblocks`…"), and the backend reads 38 distinct `PW_WOW_*` environment
   variables. Only a few combinations are exercised; a run's behaviour depends on
   which variables were set.
6. **Generated code that rewrites itself** (superblock side exits, call and jump
   prediction, jump tables). It needs read-write-execute code and cross-modifying-code
   discipline while other threads may be executing the same bytes.
7. **Per-game decisions in the engine path.** Which CPU a game gets is a profile
   setting, and the OpenGL rule exists because of how their OpenGL is routed (Wine's
   `opengl32` thunking every GL call to a host GL library). In our stack OpenGL is
   Mesa/Zink inside the process, so only Vulkan calls cross (ARCHITECTURE.md); the
   rule does not apply to us, and a per-game CPU choice is the kind of switch our
   "fix it for every program" rule forbids.

### The native backend

8. **The signal path is bound to one libkernel build.** Patch `0883` recognises
   libkernel by a hard-coded 20-byte fingerprint, segment layout and import offsets,
   and computes raw `sigaction`/`sigreturn` entries at fixed offsets from its base.
   Unknown identities, and the ELF loader's libkernel, are deliberately unsupported.
   Another firmware, or a process started the way our payload helpers are, gets no
   native execution. **This is the first thing we must measure in our own process
   model.**
9. **Every crossing saves and restores far more than it may need**: all volatile
   GPRs, an FXSAVE image and the upper YMM halves, twice per crossing, around a
   libkernel call (`wine/wow64native/cpu.c`, `pw_native_switch_fs`). Whether
   `sysarch`'s libkernel entry can clobber vector state at all has not been
   measured; if it cannot, most of that cost is avoidable.
10. **Crossing reduction exists only for Vulkan.** Other crossings (NT system calls,
    timers, audio, input, file I/O) pay the full cost; nothing counts them per frame
    by kind.

### Testing

11. **The bulk differential test is not against the real CPU in 32-bit mode.**
    `tools/dbt_differential.c` compares a translated block with the host-exec
    fallback, which is their own rewrite of the instruction into 64-bit form. That is
    real hardware for the arithmetic, but address size, stack and segment semantics
    pass through the same assumptions on both sides. The only comparison with native
    i386 execution is one hand-written program checking 100 bytes of output
    (`tests/test_pw_x86_reference.S`, `tests/test_x86_differential.py`).
12. **Coverage is defined by a workload, not by the ISA.** The differential's input
    is the encodings found in the Wine modules one game loads (203,843 encodings, of
    which the 92,536 both paths accept were compared; `docs/WINE_INTEGRATION.md`).
    Forms that set of modules never uses are covered by a checklist, not by tests.
13. **The native backend's hardware proofs are listed in prose** (Minesweeper, AV/UD2
    resumption, worker threads); there is no battery that re-runs them on every
    change and leaves a machine-readable verdict.

## What this means for us

- Native execution is now proven possible on the console, so the question is no longer
  "translator or interpreter" but **native, with crossings made cheap, plus a fallback**.
- The reference's translator is a large, carefully measured piece of engineering, but
  its shape (three decoders, one 2,000-line function, 15 interacting options) is what
  our [agent-native method](../skills/proton-agent-native/SKILL.md) exists to avoid.
- Its native backend's dependence on one libkernel fingerprint is the risk most likely
  to decide our first milestone, because our Windows programs run in payload helpers.
