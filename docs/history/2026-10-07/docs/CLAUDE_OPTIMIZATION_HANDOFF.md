> Historical snapshot of `docs/CLAUDE_OPTIMIZATION_HANDOFF.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../CLAUDE_OPTIMIZATION_HANDOFF.md) for present guidance; see the [archive index](../README.md).

# Claude handoff: PS5 Proton compatibility and optimization

You are continuing development of:

`<workspace>/PS5_Proton`

Your assignment has two connected goals:

**A. Establish complete, reproducible compatibility for these game/backend combinations:**

| Required target | Required renderer |
|---|---|
| The FNA game — appropriate Windows FNA build | Direct3D 11 |
| The FNA game — the same Windows FNA build where possible | OpenGL |
| The native 64-bit game — Windows `x64` executable | DirectX; verify the actual D3D API from runtime evidence |
| The native 64-bit game — Windows `x64vk` executable | Vulkan |
| The .NET game — existing installation | Preserve the working OpenGL baseline |

**B. Implement and validate all six optimization priorities:**

1. PS5-native Windows synchronization.
2. Windows-to-PS5 thread-priority mapping.
3. Reduced GPU submission stalls and presentation overhead.
4. Better use of unified memory.
5. Reliable shader and pipeline caching.
6. Genuine asynchronous GPU compute.

Work iteratively and autonomously through investigation, implementation, testing, deployment, measurement, and commits. Make **exactly one local project commit per completed round**.

Do not stop after writing a plan, getting a game to launch, or improving one renderer. Continue until the required compatibility gates and optimization work are validated, or a specific external blocker prevents further progress.

## 1. Non-negotiable working rules

- **Never push, publish, create releases, or open PRs. All commits remain local.**
- Work on the project's local `main`, following `AGENTS.md`.
- Preserve unrelated user changes and existing history.
- Make one durable commit in `PS5_Proton` per completed round. Include the implementation, relevant tests, dependency patches, and evidence together.
- Do not amend previous rounds or rewrite history.
- Sibling repositories remain read-only.
- For dependency changes, store reproducible patches in `PS5_Proton` and apply them to isolated, ignored copies under `.deps/`.
- Never leave an essential change only in an untracked vendor checkout.
- Preserve dependency revisions, licenses, and provenance.
- Use established PS5 driver build/link recipes.
- Never commit game binaries, assets, saves, console addresses, proprietary SDK material, or large captures.
- Preserve installed game files, prefixes, settings, and saves.
- Confirm the console is idle before deployment or launch.
- Run one console job at a time.
- Never close a title that I may be playing.
- End tests through the title's own stop/restart mechanism.
- Never call `exit()` or return from the title's `_start`.
- Do not bypass permissions or hardware restrictions.
- Do not improve benchmark scores by omitting rendering work, weakening synchronization, or degrading audio/input behavior.

A completed round must produce either a validated change worth keeping or a reproducible negative result that resolves a meaningful uncertainty. Remove failed implementation changes before committing a negative result. Avoid empty commits and repetitive "still blocked" commits.

## 2. Establish the actual starting state

Read:

- `AGENTS.md`
- `docs/PLAN.md`
- `docs/ARCHITECTURE.md`
- `docs/PATCH_REBASE.md`
- Applicable PS5 homebrew, porting, and console skills.
- Required prospero-win architecture/build references.

For driver work, inspect the relevant sibling instructions and targeted portions of:

- `../PS5_Vulkan/docs/CTS_GAPS.md`
- `../PS5_Vulkan/docs/HARDWARE_FINDINGS.md`

Verify current HEAD, dependency revisions, worktree state, and deployed artifacts. Do not assume historical documentation describes the current build.

Known local baseline:

- `72260db`: DualSense HID, touchpad mouse, and launcher handoff.
- `43fbd32`: AudioOut success-result normalization.
- `ee439e6`: User confirmation of smooth audio.

Do not reset to these commits.

Known working behavior:

- The .NET game's Windows x86-64 executable runs through bundled .NET 6/CoreCLR.
- Graphics: MonoGame DesktopGL → SDL2/WGL → Wine EGL → PS5 OpenGL SDK.
- Menus and character creation render near 60 FPS.
- I confirmed controller input, touchpad motion/click, Cross launching, and smooth sound.
- The D3D11 probe has rendered correctly through DXVK.
- The FNA game and the native 64-bit game have not yet been established as compatible.
- D3D12 game compatibility remains unproven.
- A short PE process previously stalled during exit while HID initialization was active.

Preserve the AudioOut correction: successful native submissions may return a positive frame count, while the Wine sink expects zero.

## 3. Audit the game installations before choosing implementation work

Locate my installations of the games without moving or modifying their original files.

For each target, record:

- Game edition and version.
- Executable path.
- PE architecture.
- Managed versus native executable.
- CLR metadata where applicable, including `32BITREQUIRED` and `32BITPREFERRED`.
- Native dependency architectures.
- Runtime dependencies.
- Available graphics backends.
- Legitimate launcher/store dependencies.
- Existing settings and save locations.

Do not infer executable architecture from a store's "64-bit Windows required" line.

**Requirements specific to the FNA game:**

- Distinguish XNA from FNA.
- Prefer a legitimate FNA build capable of running as x64 with matching native dependencies.
- Audit FNA/FNA3D, SDL, FMOD, and the required managed runtime.
- Do not assume the .NET game's successful .NET 6 execution proves compatibility with the FNA game's runtime.
- Determine which drivers the shipped FNA3D version actually supports.
- Use the game's documented renderer selection, such as `--graphics`, only after confirming support in that build.
- Treat XNA/D3D9 as a separate compatibility target if the installed edition requires it. Do not call it D3D11.
- Do not assume changing a CLR architecture flag converts incompatible native dependencies to x64.

The current project is x64-only. If the available FNA game installation fundamentally requires 32-bit execution, establish that precisely. Look for an available, legitimate x64-capable FNA installation before proposing expansion into 32-bit execution. Do not silently declare the FNA game complete or silently create a new CPU-translation project.

**Requirements specific to the native 64-bit game:**

- Inspect and distinguish the `x64`, `x64vk`, and `x86` executables.
- Use `x64` for the DirectX target and `x64vk` for Vulkan.
- Do not accidentally test `x86`.
- Verify the DirectX API actually used from device creation/import/runtime evidence.
- Do not infer the API from a minimum GPU feature-level requirement.

If required game files are unavailable, request their existing installation location and continue independent runtime/probe work. Do not purchase or download games.

## 4. Create isolated, persistent backend profiles

Implement the smallest profile mechanism needed to launch and reproduce:

```text
fna-d3d11
fna-opengl
native64-directx
native64-vulkan
dotnet-opengl
```

Reuse existing launcher/profile machinery where possible.

Each profile must explicitly specify:

- Executable.
- Working directory.
- Arguments.
- Prefix.
- Required environment.
- Relevant DLL overrides.
- Cache location.
- Expected renderer.

Keep backend settings scoped to the selected profile.

Do not let settings specific to the .NET game leak into the native 64-bit game or the FNA game. Audit existing global settings such as OpenGL enablement, DLL overrides, controller selection, and runtime environment variables.

Preserve normal launcher operation. Test-only files must remain one-shot and must not affect later manual launches.

Use separate test prefixes where needed to avoid contaminating the baseline. Do not overwrite user saves to obtain repeatable tests.

## 5. Require positive proof of the renderer

A launch option is not proof that its requested renderer was selected.

Capture sufficient evidence to distinguish:

- The FNA game's FNA3D selecting D3D11.
- The FNA game's FNA3D selecting OpenGL.
- The native 64-bit game creating the intended DirectX device.
- The native 64-bit game creating a Vulkan instance/device and presenting through Wine Vulkan/RADV.

Useful evidence includes:

- Engine backend logs.
- Graphics device/context creation.
- DXVK initialization and adapter information.
- Vulkan device and swapchain creation.
- OpenGL context information.
- Presentation records tied to the active process.

DLL loading alone is insufficient when a game can load unused graphics libraries.

Expected paths:

```text
the FNA game's D3D11 build:
FNA/FNA3D → D3D11 → DXVK → Wine Vulkan → RADV → display

The FNA game's OpenGL build:
FNA/FNA3D → OpenGL/WGL → Wine EGL → PS5 OpenGL SDK → display

The native 64-bit game's DirectX build:
Game renderer → verified D3D API → appropriate translation layer
→ Wine Vulkan/RADV where applicable → display

The native 64-bit game's Vulkan build:
Game Vulkan renderer → Wine Vulkan → RADV → display
```

Prevent silent backend fallback from producing a false pass. If fallback occurs, record the run as failing the requested backend gate even if the game renders.

## 6. Define compatibility more strictly than "launches"

For each required game/backend combination, validate these separately:

| Gate | Required evidence |
|---|---|
| Launch | Correct executable/runtime starts without unresolved required dependencies |
| Renderer | Requested backend is positively identified |
| Rendering | Menus and representative gameplay are visually correct |
| Input | Controller navigation and gameplay work; no unwanted duplicate input |
| Touchpad | Mouse movement and click remain correct where applicable |
| Audio | Music/effects play continuously without recurring starvation/errors |
| Gameplay | Representative gameplay continues without hangs or crashes |
| Saves | Create progress in a disposable test slot, exit, relaunch, and verify it |
| Lifecycle | Normal return to launcher and successful relaunch |
| Performance | Frame-time distribution, stalls, CPU/GPU timing where available |
| Stability | Repeated bounded runs pass |

For the FNA game, include movement, jump/dash, death/retry, transitions, and effects in representative rooms.

For the native 64-bit game, include menu navigation, movement, attacks, combat effects, and room transitions.

Use existing user progress only with permission to alter it. Prefer disposable test slots or isolated test state.

Do not claim complete-game coverage from a short session. State exactly what was exercised.

## 7. Use one implementation-and-evidence loop per round

For each round:

1. Inspect state and read the previous result.
2. State one hypothesis.
3. Set correctness and measurement acceptance criteria.
4. Implement the smallest relevant change.
5. Add focused tests for nontrivial logic.
6. Build affected components.
7. Verify patch reproducibility and artifact identity.
8. Deploy to an idle console.
9. Run the narrow probe.
10. Run affected game/backend regression gates.
11. Compare against the baseline.
12. Keep, revise, or reject the candidate.
13. Record evidence and make one local commit.
14. Continue without asking whether to proceed.

Split large changes into smaller rounds.

Maintain a concise progress ledger and small machine-readable evidence summaries. Keep raw logs, screenshots, and captures ignored.

Record:

```text
Round:
Hypothesis:
Parent commit:
Changed components:
Dependency revisions and patch hashes:
Artifact hashes:
Game/build/backend:
Workload/settings:
Baseline:
Candidate:
Correctness:
Performance interpretation:
Known limitations:
Disposition:
Next action:
```

Record the finished commit hash in the following update.

## 8. First establish reliable tests and compatibility

Suggested initial rounds:

- **B0:** Reproduce the .NET game and existing PE/HID/D3D11 baselines.
- **B1:** Resolve the short-process shutdown stall.
- **B2:** Audit the FNA game's and the native 64-bit game's installations and implement explicit backend profiles.
- **C1:** Bring up the FNA game's D3D11 build.
- **C2:** Bring up the FNA game's OpenGL build.
- **C3:** Bring up the native 64-bit game's DirectX build.
- **C4:** Bring up the native 64-bit game's Vulkan build.
- **C5:** Complete gameplay, audio, input, save/reload, and lifecycle gates.

Split each bring-up into multiple commits when separate root causes are found.

Investigate failures in this order:

1. Executable/dependency architecture.
2. Loader and managed/native runtime.
3. Filesystem, working directory, and prefix.
4. Graphics initialization.
5. Resource/shader correctness.
6. Presentation.
7. Input/audio.
8. Gameplay/save/lifecycle.

Do not modify the graphics driver to work around a missing runtime DLL. Fix the demonstrated cause in its owning layer.

## 9. Add low-overhead measurements before optimizing

Measure:

- Median and p95/p99 frame times.
- CPU frame work versus GPU execution.
- Presentation waiting and queue depth.
- Wine server traffic and synchronization time.
- Wake-up latency.
- Audio starvation and scheduling gaps.
- Allocation/mapping latency and peak memory.
- Shader compilation events and cache reuse.
- Upload/presentation copy volume where practical.

Avoid synchronous logging on critical paths.

Use matched baseline/candidate runs. Separate cold-cache and warm-cache results. Start with at least three paired runs; expand when variance obscures the result.

A refresh-capped 60 FPS result does not demonstrate spare GPU capacity. Use CPU/GPU timings and deterministic probes when games cannot expose the relevant bottleneck.

## 10. Implement thread-priority mapping

First establish supported native scheduler behavior through bounded probes:

- Native thread identification.
- Priority range/direction.
- Scheduling policy.
- Permission/failure behavior.
- Actual effective priority.

Then implement a conservative Windows-to-native mapping.

Preserve defaults, handle thread-lifetime races, and avoid blanket real-time priorities or arbitrary core pinning.

Test foreground/background contention and periodic audio-like work. Verify there is no starvation or priority inversion.

Regression gate:

- All established backends of the FNA game and the native 64-bit game.
- The .NET game audio.
- Shader compilation under load.
- Repeated launch/exit.

## 11. Implement PS5-native synchronization

Profile object types and waits before selecting the first fast path.

Do not simply enable Linux esync/fsync without satisfying their platform dependencies.

Implement one object type per round, retaining fallback where semantics cannot be handled correctly.

Test:

- Manual/auto-reset events.
- Semaphore counts.
- Mutex ownership, recursion, and abandonment.
- Signal/wait races.
- Multiple waiters.
- Wait-any and atomic wait-all.
- Timeouts.
- Alertable waits/APCs where applicable.
- Handle duplication/closure.
- Memory visibility.
- Thread/process teardown.
- Mixed fast-path and server-managed objects.

Never implement wait-all as sequential individual waits.

Measure handoff latency, CPU overhead, server traffic, and game frame-time changes. Stress race behavior before enabling the path by default.

## 12. Reduce graphics submission and presentation overhead

Profile OpenGL and Vulkan-based paths separately.

Investigate:

- Unnecessary readbacks.
- Conversion/scaling passes.
- Full-device/full-queue waits.
- Excessive submissions.
- Acquire/present blocking.
- Resource-retirement stalls.
- Unnecessary CPU/GPU serialization.

Change one demonstrated bottleneck per round.

Direct presentation requires proven compatibility of format, tiling, size, alignment, scanout eligibility, ownership, and synchronization. Keep the existing GPU conversion path when needed.

Validate:

- The FNA game's OpenGL build versus D3D11 images.
- The native 64-bit game's DirectX build versus Vulkan images.
- Multiple resolutions/aspect ratios.
- Resize/fullscreen transitions where supported.
- Swapchain/context recreation.
- Input latency.
- Clean teardown.

Use deterministic scenes for comparisons; account for animation and randomness. Do not require unrelated APIs to produce byte-identical images when documented rendering differences are legitimate.

## 13. Improve unified-memory usage

Measure allocation churn, staging copies, mapping/protection changes, and retained memory.

Optimize one high-volume path:

- Upload-ring reuse.
- Suitable resource suballocation.
- Persistent mappings.
- Compatible allocation batching.
- Proven-safe elimination of staging copies.

Preserve:

- CPU/GPU visibility and ordering.
- Windows memory semantics.
- Fixed-address reservations.
- Guard pages/protection transitions.
- CLR JIT behavior.
- Resource lifetime.
- Alignment and platform page-size requirements.

Measure actual usable memory; do not budget against total advertised console RAM.

Run existing memory/JIT tests and long-enough gameplay transitions to expose leaks or fragmentation.

## 14. Make shader and pipeline caching reliable

Audit caches actually used by each required backend:

- The FNA game's OpenGL build.
- The FNA game's D3D11 build/DXVK.
- The native 64-bit game's DirectX build.
- The native 64-bit game's Vulkan build/RADV.
- The .NET game's OpenGL path.

Treat VKD3D separately when its D3D12 path becomes operational.

Verify persistence, per-game separation, writable paths after elevation, invalidation, corruption recovery, and build identity.

Use upstream mechanisms before adding custom caching.

Test cold launch, warm launch, launcher restart, runtime/compiler update, and damaged disposable cache.

Validate supported early/background compilation mechanisms and bound worker contention.

Never skip draws or substitute incomplete shaders to conceal compilation stutter.

## 15. Establish real asynchronous GPU compute

First prove the native queue interface and its supported ABI using existing SDK knowledge and conservative probes.

Establish:

- Queue creation/destruction.
- Submission and completion.
- Cross-queue dependencies.
- Memory visibility.
- Process lifetime.
- Hardware workarounds.

Do not equate multiple Vulkan queue handles with concurrent execution.

Use a deterministic graphics-plus-compute probe with verified pixels/checksums, a serial baseline, and timestamps demonstrating overlap.

Integrate into an isolated RADV dependency only after the native probe passes. Preserve serial fallback while validating queue ownership, fences/semaphores, teardown, and failure behavior.

Run targeted multi-queue tests and the relevant driver regressions.

**Do not assume the FNA game or the native 64-bit game will benefit from asynchronous compute.** Their compatibility must remain intact, but dedicated workloads may be necessary to prove this optimization's usefulness.

If the platform interface cannot be established safely, record the precise blocker. Do not claim the priority is implemented.

## 16. Apply a backend-aware regression policy

After a Wine loader/runtime/synchronization change:

- Run all established game/backend targets.

After an OpenGL-only change:

- Run the FNA game's OpenGL build and the .NET game.
- Run a targeted unrelated-path smoke test at the integration boundary.

After a Wine Vulkan/RADV change:

- Run the FNA game's D3D11 build.
- Run the native 64-bit game's DirectX build.
- Run the native 64-bit game's Vulkan build.
- Run the existing D3D11 probe.

After input/audio/lifecycle changes:

- Run all established targets.

At priority boundaries and final acceptance, run the complete matrix.

Never silently drop a failing backend from the matrix to make a round appear successful.

## 17. Preserve reproducible build and deployment

Start with the existing scripts, verifying current options before use:

```bash
cd <workspace>/PS5_Proton

ps5/tools/build.sh

PROSPERO_FREETYPE_TARBALL="$PWD/.deps/freetype-2.13.3.tar.xz" \
tools/build-wine-ps5.sh \
  --radv "$PWD/../PS5_Vulkan" \
  --ps5-opengl-sdk "$PWD/.deps/ps5-opengl-sdk-1.0.0/sdk" \
  --jobs 12

python3 tools/stage-wine.py
```

Use isolated dependency builds when applying driver patches.

Extend the existing deployment/test tools minimally for the new profiles. Do not hard-code every test to the .NET game's profile `1`.

Preserve prefixes during deployment. Verify current control connectivity; `9111` was the last working port. Keep console settings private.

Regenerate `wine/prospero-adaptations.patch` after changing imported adapters. Verify Wine patch replay from its pinned base after patch-series changes.

Wait for build/stage/deploy dependencies to finish in order.

## 18. Acceptance, rollback, and autonomous continuation

Accept a change only when its intended behavior is demonstrated and relevant correctness gates pass.

Describe capability improvements separately from measured performance gains.

Reject or revise changes that:

- Corrupt rendering.
- Weaken synchronization semantics.
- Regress audio/input.
- Introduce unbounded memory growth.
- Gain speed by doing less required work.
- Cannot be reproduced.
- Add complexity without a demonstrated benefit.

Selectively remove unsuccessful uncommitted edits. Correct or revert already committed regressions through a new local round; never rewrite history.

Continue independent work when one target is blocked. Ask only for genuinely missing installation access, physical intervention, required permissions, or decisions that cannot be resolved from evidence.

Do not repeatedly retry an unchanged failure.

## 19. Final deliverables

Provide:

- Local commit list, one commit per completed round.
- Reproducible dependency patches and pinned builds.
- Launcher profiles for every required backend.
- A compatibility matrix with separate launch/render/input/audio/gameplay/save/lifecycle results.
- Baseline-versus-final performance measurements.
- Cold/warm cache results.
- Exact renderer evidence.
- Implemented, opt-in, rejected, and blocked optimization statuses.
- Known limitations.
- Confirmation that test hooks were removed and nothing was pushed.

**Completion requires both FNA game renderers and both renderers of the native 64-bit game to pass their stated gates, with the .NET game preserved.** If a required target is blocked, state that the assignment remains incomplete and identify the concrete blocker; do not substitute another game or renderer and call it equivalent.

Begin by inspecting the current repository and available game installations, then proceed autonomously through the baseline, compatibility, optimization, and combined-regression rounds.
