---
name: proton-agent-native
description: How PS5_Proton is designed for the agent's loop, given that agents write all of its code - the loop's stages mapped to this repository's commands, where code goes and the shape it takes, the oracles a result is compared against, the instruments that make failures visible, and what the method demands of the 32-bit translator. The method itself is PS5_VulkanTemplate's ps5-agent-native skill. Read both before designing or restructuring code, a probe, a tool or a document here.
---

# PS5_Proton, designed for the agent's loop

Agents write all of this code; I direct the work and judge what it does. So the
repository is designed for the agent's loop, not for a human reader:

```text
inspect → hypothesize → change → build → deploy → collect evidence → compare → iterate
```

The method, and why the target is the loop's bandwidth rather than any coding style
or language level, is `../PS5_VulkanTemplate/skills/ps5-agent-native/SKILL.md`. Read
it first. This skill is what the method means here.

## The loop, in this repository's commands

| Stage | Here |
| --- | --- |
| inspect | `docs/PLAN.md` (status), `docs/ARCHITECTURE.md` (owners), the module's own header, the last `evidence/<run>.json` for what is being changed |
| hypothesize | one question a run can settle, written down before the run (`docs/CONSOLE_QUEUE.md` when the console is not free) |
| change | portable logic in `src/` or `wine/ps5/`; console code in `native/` or `platform/`; Wine only through `wine/patches/` |
| build | `tools/host-tests.sh [words]` (seconds, host compiler); `bash tools/build-production.sh` for the console |
| deploy | `tools/deploy-wine.py`, `ps5/tools/deploy.sh` |
| collect evidence | `tools/run-console.py` (one probe or game: `klog/<run>*.log`, ignored, and `evidence/<run>.json`, committed); `tools/run-battery.py`; `tools/gate.py run` |
| compare | the run's JSON against the previous run's, the gate against its fingerprint, Wine's tests against their baseline (below) |

The commit gate is this loop with a lock on it: code is committed only when the
console passed the battery on a build of exactly that code (AGENTS.md).

## Code an agent can change from one place

- **Our code is flat C and C++.** Modules are `pw_<subsystem>` (`pw_audio_ring`,
  `pw_memory_guard`, `pw_wine_dmem`): structs passed in, functions named
  `pw_<subsystem>_<verb>[_<object>]`, one owner per resource. A new module follows
  the same pattern, and `grep pw_<subsystem>_` finds all of it.
- **The header holds the contract.** `src/pw_audio_ring.h` is the model: who writes
  and who reads, what the counters guarantee, what a corrupted counter can and cannot
  do, why it never locks, and where the design is documented.
- **Invariants are checked, and a failure names its kind.** Compile-time ones with
  `static_assert` (the size of each option struct `native/` hands to the console's
  system services). Run-time ones at every entry that takes outside input, returning
  a typed error from `include/prospero_win.h` (`PW_ERR_PRECONDITION`,
  `PW_ERR_MALFORMED`, `PW_ERR_LIMIT`...) that the caller logs. Our code never aborts
  on the console: a title that aborts is a crash (rule 2). The host test asserts every
  invariant (`tests/test_pw_audio_ring.c`). Upstream code keeps its own `assert`,
  which prints where it fired before aborting (`wine/ps5/pw_wine_compat_libc.c`).
- **Wine stays Wine.** Upstream files keep upstream's shape and change as little as
  possible (rule 8), so the series rebases. A patch calls into `wine/ps5/pw_wine_*.c`,
  where the PS5 logic lives in our shape and is host-tested
  (`tests/test_pw_wine_*.c`). A patch that grows logic of its own is moved there.
- **Every portable module gets a host test**, registered in `tools/host-tests.sh`'s
  table, so its loop is seconds, not a deploy.

## Representation

- **Machine code is read through tools.** A game's PE files: `tools/audit-pe.py`
  (architecture, managed or native, imports; headers only, never game data). A
  crash: the symbolized backtrace (`../PS5_VulkanTemplate/skills/ps5-console/`,
  `crash-reading.md`). Guest code in the translator: its decoded dump (below).
- **Assembly only at boundaries**: entry and exit stubs, saving and restoring CPU
  state, calling-convention and system-call transitions. Each piece states its
  register contract beside it and is checked against a C version.

## Oracles: what a result is compared against

The strongest check runs the same thing two ways and compares the results.

| Question | Oracle |
| --- | --- |
| Is a Wine behaviour the port's fault? | the same Wine test on the pinned Wine on Linux: `tools/run-wine-tests.py`, `tools/classify-wine-failures.py` (a console failure the baseline lacks is the port's, or carries a written reason) |
| Is a program's odd behaviour the port's? | the same program under Wine on the PC, before blaming the console (`docs/PLAN.md`, a game launcher) |
| Did a change regress anything? | the previous `evidence/` record of the same probe, and the gate battery |
| Is the translated code right? | the same code run natively (below) |

## Instruments: make failures visible before guessing

A failure the records do not show is invisible to the agent. The next change adds
what would show it, not a guess, and cheap instruments stay in:

- the runtime counters `run-console.py` writes into every evidence record
  (`wine/ps5/pw_wine_perf.h`);
- the file-descriptor census (`src/pw_fd_census.*`) and the memory guard
  (`src/pw_memory_guard.*`);
- probe markers (`PS5_PROTON_*`): a probe prints one verdict line that
  `run-console.py` matches (`--marker`).

The controller bug of 2026-10-06 is why. Guesses at focus, a start-up race and a
reconnect trick each cost a run with me at the pad. Then counters along the pad's
path (patch 0940) showed that every report reached the game's process and that no
program had opened the controller, which located the cause, fixed for every program
by patch 0941. Fix the layer the evidence names, for every program.

## Knowledge where the next agent starts

- Status and the next steps: `docs/PLAN.md`; queued console runs:
  `docs/CONSOLE_QUEUE.md`; what each optimization tried: `docs/OPTIMIZATION_LEDGER.md`.
- Measurements live in `evidence/`, pointed at, never copied into prose.
- A failed run is recorded at the same detail as a passing one.

## 32-bit programs (`win-32bit`)

The design is `docs/IA32_DESIGN.md`: native execution, one measured crossing path, and
an interpreter as the exact fallback; a translator only if the crossing measurements
demand one. Whatever executes guest code meets the method, and it is where the method
matters most: its input is machine code, the one representation an agent cannot
reason about.

- **Guest code is lifted into meaning before anyone reasons about it.** A tool dumps
  any guest range as decoded instructions, and the translator's own form of each
  block (IR or emitted host code) beside it. The agent reads the dump, never bytes.
- **Instruction semantics are data**, in one table (`src/pw_ia32_isa.def`) that the
  decoder, the interpreter, the dump tool and the test generator all read, not
  knowledge spread through a large switch. One parser in the repository.
- **Differential tests are the main check.** Generated instruction sequences run on
  the real CPU in 32-bit mode on the PC (a `-m32` process) and through our code, and
  their registers, flags and memory are compared. One failure is one machine-readable
  line: the instruction, the state before, the expected and the actual state after.
- **Assembly only at the boundaries**: entering and leaving translated code, saving
  and restoring guest state, 32↔64 thunks. Everything else is C.
- **Block boundaries assert the guest's invariants** (stack, flags, segment state), so
  a wrong translation fails where it happens, not frames later.
- **Results go into the existing loop**: host tests for the translator's portable
  parts, `evidence/` records and battery cases for what needs the console.
