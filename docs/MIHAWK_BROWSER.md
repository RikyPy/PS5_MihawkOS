# MihawkBrowser (proposal, 2026-10-16, revised after review)

Status: **accepted 2026-10-16; the user chose the open web (decision 2), a real browser.** Nothing is built and nothing
has run on the console. Written from the
repository at `53b0208` (branch `browser`), with `main`'s `0d04fc0` taken into account (taskbar layout with pins, patch
1140; taskbar acrylic, 1141), from local klogs (not committed), and from web sources read on 2026-10-16. No binaries
were downloaded.

Rule 7 shapes the whole plan. Every Chromium session on record ran between 2026-10-05 and 2026-10-12. On 2026-10-15
the PS5 memory backend replaced the one they ran on: code now runs from JIT copies (patch 1127), writes are tracked per
4 KiB page on 16 KiB host pages (1122/1134), and large sections live in direct memory
([PLAN.md](PLAN.md):3-30, :399-431). Those sessions were also CEF on the Alloy bootstrap, which CEF 150 no longer has
(§3.1). So nothing below about Chromium on the console is proven for the current build, including the numbers.

## 0. Measured since the plan was accepted (host, 2026-10-16)

What the build showed on the pinned Wine on Linux, which corrects the plan below where they differ. Host memory is a
PC oracle, not console proof; nothing here has run on the console yet.

- **libcef.dll 150 is bigger than a launcher's CEF 126.** `.text` 221.3 MiB, `.rdata` 31.0 MiB, file alignment 512 bytes (§2's
  176.2 MiB was a launcher's CEF 126). With 512-byte alignment Wine cannot map the image and copies it whole into each
  process; on the host every CEF process holds the 221 MiB text resident. C0 measures what patch 1127 copies of it.
- **Three persistent processes, not two.** The browser, one renderer, and `storage.mojom.StorageService`, which CEF
  150 always runs out of process (no switch keeps it in). Named transients: `chrome.mojom.ProcessorMetrics` (a Chrome
  metrics utility a few seconds after start, lifetime under 0.2 s on the host) and `quarantine.mojom.Quarantine` (at
  a download's completion; it writes `<file>:Zone.Identifier`, which Wine stores as a second file). No GPU process
  (in-process GPU) and no audio or network process.
- **The Chrome bootstrap phones home.** Its profile asked `accounts.google.com/ListAccounts`, `www.google.com/async/folae`
  (AI-mode eligibility) and `sb-ssl.google.com/safebrowsing/clientreport/download` (a report of each download) on its
  own (NetLog, browser-host-30). Chrome policies written under `HKCU\Software\Policies\MihawkBrowser` and named by
  `chrome_policy_id` (sign-in, Safe Browsing reporting, metrics and prediction off; DuckDuckGo as the search engine;
  PDFs external; no spell check), `--disable-features=AimEligibilityService` and `--gaia-url=https://gaia.invalid/`
  leave only page traffic (browser-host-32). The two preferences of §3.1 became policies.
- **`can_download` must be implemented.** Left unset, CEF's C wrapper answers false and a link that turns into a
  download is dropped with no further callback (browser-host-27); `StartDownload` worked, a click did not.
- **Closing needs our own second `WM_CLOSE`.** CEF's close notification after `do_close` never reached the main window
  under Wine (browser-host-18); `do_close` posts it.
- **A missing system font recursed Chromium to a stack overflow.** When DirectWrite cannot find the message font
  (Tahoma, not in a prefix's registry Fonts key), Chromium's system-font code recurses (7 frames a level, 575 levels,
  browser-host-16). The staged console prefix registers Tahoma and carries it in `C:\windows\Fonts`, so the host runs
  use a copy of that prefix.
- **With no GL at all, Chromium retries a GPU context forever** (1.4 million attempts in 3 minutes, 100% CPU, pages
  stalled; browser-host-21). The host Wine has no display driver, so host runs use software compositing
  (`--disable-gpu --disable-gpu-compositing --disable-software-rasterizer`, 0 attempts). On the console a broken GPU
  path would look the same, so C1 checks `debug.log` for it.
- **Host result** (2026-10-16, record removed from the tree and in git history; `tools/run-browser-host.py`): example.com,
  a launcher vendor's about page, a DuckDuckGo search and a Wikipedia article all 200; a click-style navigation
  downloaded a launcher's PE32 installer (2,380,800 bytes, SHA-256 checked, 288 ms); Open started it, its first window came 146 ms
  later and the browser handed off and exited. Window 654 ms after process start, first page 1.94 s. The browser tools'
  default scripts no longer download a launcher installer: a download is named with `--script` and `--expect-sha256`.

### On the console (2026-10-16, build `52aa67fcbfe9f2a2`: main 322228b plus MihawkBrowser)

- **C0 could not run:** the two CEF launchers are no longer installed in the `mihawk` prefix (it was recreated), and
  the library held no launcher entries. C1 measures CEF 150's own processes instead.
- **C1 run 1** (record removed from the tree; in git history) faulted in the browser's GPU thread at startup: Wine's DXCore answered
  FALSE to every `IsAttributeSupported`, so Chromium's NPU scan took the PS5 GPU for an NPU and read its driver
  description "PS5" into an eight-byte pointer (0x00355350, `gpu::CollectNPUInformation`, found with CEF's release
  symbols). Patch 1170 makes every DXCore adapter report Direct3D 11 and 12 graphics and core compute. The browser
  process had converted 89,776 KiB of code in its 3 s.
- **C1 run 2** (record removed from the tree; in git history; `--limit 600`): **https://example.com loaded with status 200** (window
  2.9 s, page 6.4 s after process start, load 195 ms), with the renderer and StorageService started; the native chrome
  draws as designed (capture 02). Then the memory guard ended it by SIGKILL, cleanly, the title back at the launcher:
  system memory past 670 MiB for 3 s, helpers 456 MiB in 10 processes, the browser process the largest at 192 MiB.
  The browser process had **converted 125,280 KiB (122 MiB) of code** (patch 1127) and held 165 MiB flexible
  resident and 337 MiB of images in direct memory; the renderer and StorageService were ended before their first
  10 s census. The baseline before the browser was 159 MiB in 89 processes.
- **Verdict:** over the go/no-go (60-80 MiB converted per CEF process) in the browser process alone, so CEF 150 does
  not fit the 200 MiB ceiling as built. Reported to the user, who chose to measure first.
- **Run (a)** (record removed from the tree; in git history; the guard's default 870 MiB, build `f43887469074c3ff`): passed.
  example.com, then a DuckDuckGo search for a launcher's client download, whose results led on to the vendor's store, where
  its PE32 installer downloaded byte-exact (2,380,800 B, SHA-256 checked, 3.7 s); clean close at 49 s. Memory: system
  peak 683 MiB, helpers 530 MiB in 11. Browser process 211 MiB peak, 167,008 KiB converted in 740 conversions;
  renderer 155 MiB / 130,912 KiB; StorageService 65 MiB / 49,488 KiB; the transients ProcessorMetrics 45,520 KiB and
  Quarantine 47,328 KiB converted in their seconds. About 430 MiB for the three persistent processes.
- **Run (b)** (record removed from the tree; in git history; an uncommitted build with patch 1127's `PS5_EXEC_CLUSTER` cut from
  256 to 16 KiB): example.com at 200, then a fault ~27 s in, in Chromium's call-stack sampling profiler, which copies
  the main thread's stack up to the TEB's stack base: the top 1 MiB below that base is unreadable in a PS5 helper
  (read at 0x1000110000; `base::StackCopier::CopyStackContentsAndRewritePointers`). `--disable-stack-profiler` now
  avoids it; the stack bounds stay a Wine follow-up. Browser process 113 MiB / 66,288 KiB in 4,143 conversions,
  renderer 62 MiB / 43,120 KiB, StorageService 32 MiB / 13,440 KiB: about 207 MiB.
- **What they show**, at the same point (example.com loaded): 16 KiB clusters convert 63 instead of 138 MiB in the
  browser process, 40 instead of 101 in the renderer, 12 instead of 44 in StorageService. 55-73% of what 256 KiB
  clusters copy is code that never runs. The cost is 5.6 times the conversions and about 1.2 s more to the first page
  (6.8 s against 5.5 s). Even at 16 KiB the three processes stand at about 207 MiB on a trivial page, so the ceiling
  also needs shared code copies (§3.3, lever 1): the renderer and StorageService re-convert the same libcef code.
- **Run (a)'s neighbour** (record removed from the tree; in git history) is void: another session deployed its own build during it.
- **The cluster A/B** (`evidence/cluster-ab-2026-10-16-ab.json`, the user's choice, rule fixed before the runs in
  CONSOLE_QUEUE.md): 64 against 16 KiB, each arm the browser run and a game workload in the earlier emulator experiment (removed) for 90 s, all four
  passing. 16 KiB won on every count: the three CEF processes 214 against 281 MiB (128,256 against 202,288 KiB
  converted), the first page 6.29 against 6.48 s, the game's first frame 14.0 against 19.6 s (64,480
  against 91,952 KiB converted). The browser process made 4,451 conversions, past K7's proven 4,096 objects, none
  failed. The rule's first choice of game timing, the title's
  "perf: first vulkan frame" line, never appears for that workload (it presents through the GPU broker); the title's own
  "loading screen: closed after N s (the game's presenter takes the display)" times the same event and replaced it
  before the 16 KiB arm ran. At 214 MiB the browser is 14 MiB over the 200 MiB ceiling on a light page; shared code
  copies (lever 1) are the next lever.
- **The gate on that build** (`gate-2026-10-16-browser-01`, fingerprint `866292903a683ef3`: main 322228b, MihawkBrowser,
  patch 1170 and the 16 KiB window): 36 of 82. Main's design commits, which bypassed the gate, fail four cases on their
  own: `shell32/64` expect the old square-Mica taskbar and the classic Explorer's controls, `window-effects32/64`
  expect backdrops 6 and 7 to be refused (patch 1160 accepts them). The 16 KiB window fails three launches (the grouped
  probes, `host-dmem-fallback64`, `game-mode64`): each time `winedevice.exe`'s exit left the virtual mutex and the
  dmem engine's lock held, with the engine recording a protect (or release) of the 16 KiB code copy at 0xdfffefc000
  (kernel32), until the stall backstop ended it 7 s later. So 16 KiB is not yet safe as the default, and none of this
  code is committed.
- **Why it stalled** (host analysis of those logs; the user chose to root-cause it before 16 KiB becomes the
  default): no protect hung. The lock's owner was the first thread the exit killed, service thread 004c, which had
  just failed to load mountmgr.sys and was converting kernel32 code it ran for the first time (with 256 KiB windows
  that code was copied earlier). A kill ends a thread outside a syscall at once, and Wine's trap and fault handlers
  count as outside one (they run on the signal stack, not the kernel stack); both take the virtual mutex unmasked. So
  the kill ended 004c mid-conversion ("thread 004c killed" in each log) with both locks held, and the engine's record
  of its last call stayed. Wine patch 1171 (uncommitted) makes a kill wait until those handlers return: in user mode
  the thread then ends holding nothing, in a syscall it is marked as before. Every program's exit and
  TerminateThread benefit, at any window size. `tests/kill-convert-probe.c` (gate case `kill-convert`) kills a thread
  300 times while it converts fresh code and checks that the virtual mutex is still free each time; it needs the
  console (the pinned Wine converts nothing).
- **64 KiB, the user's choice for the default** (2026-10-16): the three launches that stalled at 16 KiB, run again
  on the A/B's saved 64 KiB build (`stall-check64k-2026-10-16-01`): `winedevice.exe` exited cleanly in each, with no
  stall line and no stall backstop in any helper; `host-dmem-fallback64` and `game-mode64` passed. The grouped probes
  launch failed on something else: in `payload-eight` the 5th to 8th helpers never started ("payload loader failed at
  answer ... after 15000 ms", errno 35, with 13 helpers alive and the console at 350 of 870 MiB), and
  `payload-memory`'s helper failed the same way; every other probe passed on its own verdict. The design side traced
  that to the console, not the build: each helper process leaks one stdin TTY slot of a 128-entry kernel table, 95
  were leaked by that evening's runs, and spawns fail until a restart. So patch 1127 now defaults to 64 KiB, and the gate runs on main 762df4f with the browser, 64 KiB
  and patch 1171.
- **The gate passed** (`gate-2026-10-16-browser-02`, build `60e799a5074a3b9a`, fresh boot): 83 of 83, among them
  `shell32/64` and `window-effects32/64` (main's updated cases), the three formerly stalled launches,
  `payload-eight` and `payload-memory`, and the new `kill-convert` (300 kills mid-conversion, 1.5 s). No stall line
  in any of 516 helper logs, no exit stall; the one helper the backstop ended was `doom-backstop`'s, by design.
  System memory peaked at 520 MiB outside `memory-guard`. Not run on the console: `kill-convert` against a build
  without patch 1171 (its sensitivity), and 16 KiB with 1171.
- **kill-convert is not sensitive** (2026-10-17, `kill-convert-before-2026-10-17-01`, the saved 16 KiB build without
  patch 1171): it passed, 300 kills over 2,200 conversions with the mutex never kept (helpers: 10 exited by themselves,
  4 ended by the title; no maxpipekva line). So it does not reproduce winedevice's stall: a conversion of fresh private
  memory is too short for a kill at a random moment, where the gate's stall was a process exit during a conversion of
  file-backed kernel32 code. It stays in the gate as a check that 1171 holds, and proves nothing more until it kills
  the worker as it enters fresh code. The 16 KiB build with 1171 (`stall-check16k-1171-2026-10-17-01`) got one launch,
  closed from the console at 85 s before any case ended: it had been deployed when the user's own benchmark had just
  exited, which read as an idle console (a collision). 16 KiB with 1171 is still untested; 64 KiB stays the default.
- **C4** (record removed from the tree; in git history; same build): the same launcher installer downloaded byte-exact (2,380,800 B, 4.2 s)
  with a ` (1)` suffix beside run (a)'s copy, the downloads list showed it, and Open started it through
  `ShellExecuteExW`. The installer is PE32 and its process failed at once: "failed to create main module ...
  status c0000018" (the 32-bit image could not be mapped at its base, the known setup.exe high-base issue). No
  installer window, so no hand-off. **This blocks C4**: it is the runtime's WoW64 image placement, a runtime fix, not
  the browser's; C4 runs again once that lands.
- **A deadlock in the desktop session, fixed by Wine patch 1173** (2026-10-18). The owner, on build `2ca89afb`: a search
  ("Google") loaded html.duckduckgo.com at status 200 in 291 ms, then the page stayed blank and the window would not
  close. Reproduced in the desktop session (2026-10-18, record removed from the tree and in git history; `tools/run-console.py --path
  C:\windows\system32\mihawk.exe --args <MihawkBrowser.exe>` with the desktop's environment and the script `wait 6000;
  load <a search>; wait 20000; exit`): the load logged at 11.3 s and the script's next step never ran. The
  hang snapshot: the UI thread's last request `get_window_region` and Chromium's occlusion-tracker thread's
  `get_message`, both 96,746 ms old, neither in a server nor a sync-device wait, the WM_CLOSE unread in the UI thread's
  queue. The PS5 driver's surface flush (any thread that pumps messages flushes every surface) asked win32u for the
  window's style under the surface's lock, while `update_surface_region` takes win32u's lock and then the surface's
  (wine/patches/README.md, 1173). The 10-16 runs had no Explorer, whose windows keep the occlusion tracker busy. On
  build `5c451695` (1173) the same run (`-02`): the load at 11.2 s, every step, `close asked` and `close done` 71 ms
  apart, the browser's exit at 32.6 s, the page and the chrome (lock, reload) drawn; no exhaustion line. That run's
  verdict failed for another reason: on its 100 s budget the title's close reached nothing ("inputs delivered 0 of 0"),
  Explorer's main thread then waited forever on an unnamed event, and the title ended the session by SIGKILL after
  10 s (the gate's desktop cases end through a probe's marker and never take that path).
- **Open (2026-10-18, with the browser on main as 13a949f):**
  - C4: a downloaded installer's window. The launcher installer tried is PE32 and its image cannot be mapped at its base
    (c0000018, the setup.exe high-base issue), so Open starts nothing yet. A runtime fix, not the browser's.
  - Memory: CEF's three processes hold about 281 MiB at patch 1127's 64 KiB windows, over the 200 MiB ceiling (§3.3).
    Shared code copies between helpers are the next lever (PLAN.md, K11-K17); 16 KiB windows with patch 1171 are
    untested.
  - The desktop session does not close when the title ends it from outside: Explorer waits forever on an unnamed
    event and the title ends the session by SIGKILL (above; its own task in PLAN.md).

## 1. Goal and scope

MihawkBrowser is a web browser shipped by default in Mihawk OS. Its first job is getting a game launcher's installer
onto the console and started, through each launcher's own installer. Memory is the
constraint that decides most of the design.

**v1 does:**

- A native start view with four launcher buttons (two more were removed on 2026-10-19 at the owner's word). Each one downloads that vendor's installer from its direct HTTPS URL,
  with no web page and no renderer (decision 15). A secondary "Open download page" link is there for when a URL stops
  returning an installer.
- Browse HTTPS sites, one page at a time, with no tabs (decision 14). A typed `http://` address is upgraded to
  `https://`. Plain HTTP is refused, and popups load in the same view.
- Download into the prefix's Downloads folder (`FOLDERID_Downloads`) with no save dialog. Name collisions get Windows'
  ` (1)` suffix.
- A downloads list with progress, size, Cancel, Show in folder and **Open**. Open runs the file through
  `ShellExecuteExW`, the call Start makes ([wine/ps5/pw_shell_start.c](../wine/ps5/pw_shell_start.c):214-219).
- One address/search box, plus Back, Forward and Reload/Stop.
- Minimal settings: search engine, "Close MihawkBrowser when an installer starts", Clear browsing data, and About
  with versions and licences. Light and dark follow the system.

**v1 is done when** the download is byte-exact, Open starts the installer (PE32, PE64 and MSI), the installer's first
window is drawn, and the browser is gone. Whether a launcher then installs and runs is launcher compatibility, not the
browser. Each launcher gets its own [PLAN.md](PLAN.md) row with its known blocker (§3.5).

**v1 does not:** tabs, WebAssembly (jitless V8 has none, §3.1), sync, accounts, extensions, password manager,
autofill, print, spell check, proprietary codecs (H.264/AAC), Widevine, a PDF viewer, WebGPU, SwiftShader, an
on-screen keyboard, or session restore. PDFs are downloaded instead of shown.

## 2. Options

| Engine / build | Size (Windows x64) | Memory | Wine status | Licence | Effort | Risk |
| --- | --- | --- | --- | --- | --- | --- |
| **CEF 150 LTC, minimal distribution + our own C client** (Alloy style, windowed, native chrome) | 156.7 MiB archive (`cef_binary_150.0.22+gc1a097e+chromium-150.0.7871.256_windows64_minimal`, [index.json](https://cef-builds.spotifycdn.com/index.json)). Unpacked size measured at H1; for scale, a launcher's CEF 126 `libcef.dll` alone is 219,859,608 B | Target 150, ceiling 200 MiB of census for all its processes (§3.3). Decided by code copies: `libcef.dll`'s text is 176.2 MiB | CEF 150 and the Chrome bootstrap have never run here. Two launchers' CEF 126 and CEF 108 reached signed-in UIs, both on the Alloy bootstrap and the old backend ([PLAN.md](PLAN.md)) | BSD-3 plus `CREDITS.html`; FFmpeg parts LGPL-2.1 | Medium-high: a C program, fetch/build/stage scripts, two gate cases | High until C0 (§5) |
| ungoogled-chromium 155.0.8059.39-1.1 ([releases](https://github.com/ungoogled-software/ungoogled-chromium-windows/releases)) | 190.7 MiB zip | Not measured. Chrome's default process model; one code copy per process (§3.3) | No run | BSD-3 | Low: flags and a shortcut | Same code-copy cost with more processes; Chrome's UI; no LTC |
| WebView2 Fixed Version ([docs](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution)) | "over 250 MB" | 399 MB against Electron's 318 MB on one page ([tauri#5889](https://github.com/tauri-apps/tauri/issues/5889)) | Black window on win10 prefixes, `DCompositionCreateDevice3` stub ([58921](https://bugs.winehq.org/show_bug.cgi?id=58921)) | Microsoft terms | Medium | Excluded: a Microsoft binary |
| Supermium v150-r2 ([repo](https://github.com/win32ss/supermium)) | 179.7 MiB zip | Like Chrome | No run | **No licence stated** | Low | Excluded: rule 6 |
| Thorium ([releases](https://github.com/Alex313031/Thorium-Win/releases)) | n/a | Like Chrome | No run | BSD-3, bundles proprietary codecs and Widevine | Low | Excluded: codec and Widevine bundling (it is maintained: M156.0.8078.28, 2026-10-11) |
| Chromium snapshot r1720726 | 346.9 MiB zip | Like Chrome | No run | BSD-3 | Low | Excluded: trunk builds, no security branch |
| *No browser:* the native start view over WinHTTP | A few KB | One helper, 10-14 MiB ([PLAN.md](PLAN.md):1336-1338) | WinHTTP/schannel strict HTTPS is gated (`launcher-tls`) | Ours | Low | Not a browser; breaks when vendors move URLs |

**Recommendation: CEF 150 LTC with a small C client of our own, behind one console go/no-go (C0).**

- **We own the executable**: its name, icon, process model, downloads and URL policy.
- **It has an LTC branch**: 7871 (Chromium 150.0.7871.256) gets platform-agnostic security fixes, last refresh
  2027-04-13; the next LTC is M160 ([branches](https://chromiumembedded.github.io/cef/branches_and_building)).
- **It has a C API** (`include/capi`), so the client builds like `mihawk-appearance.exe`
  ([tools/build-wine-probe.sh](../tools/build-wine-probe.sh)), with no C++ runtime or MSVC.
- **The go/no-go.** Each CEF process may pay a JIT copy of every code page it runs, out of a 176.2 MiB text. If C0
  shows one CEF process converting more than about 60-80 MiB, CEF does not fit until converted image code is shared
  between processes (§3.3). That fix is in the backend and helps every CEF launcher too. Until then, the start view
  over WinHTTP does the v1 job in one helper.

**Runner-up: ungoogled-chromium** with the §3.1 flags, only if the C client stalls. It does not fix memory: it pays the
same code copies with more processes.

## 3. Architecture

### 3.1 Processes and flags

| Process | Holds |
| --- | --- |
| `MihawkBrowser.exe` (browser) | Native UI (Win32), CEF, network service, audio, the GPU **as a thread** (C2's first pair tests out of process), downloads |
| one renderer | The page: Blink and V8, jitless. Starts at the first navigation, not for the start view |
| named transients | The Quarantine Service utility at a download's completion: Chrome launches it on Windows ([chrome_download_manager_delegate.cc](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/chrome/browser/download/chrome_download_manager_delegate.cc):2692-2696), and CEF 150 likely uses Chrome's download delegate. Storage or data-decoder utilities if CEF runs them out of process. H3 names them |

Process-count pass criteria read "2, plus at most the named transients". `--renderer-process-limit` is a soft limit.
Two ordinary paths add a renderer and are avoided: `chrome://` pages are always isolated, and Chromium's main-frame
error pages get a process of their own ([process model](https://chromium.googlesource.com/chromium/src/+/main/docs/process_model_and_site_isolation.md)).
So load errors show in a native panel (`on_load_error`), and GPU status comes from `debug.log`.

Each process costs more here than on a PC: a 25-60 MiB flexible baseline for a Chromium helper, measured before patch
1127 ([PLAN.md](PLAN.md):1338), plus its own code copies (§3.3).

**The bootstrap.** CEF deleted the Alloy bootstrap in M128 ([cef#3685](https://github.com/chromiumembedded/cef/issues/3685)).
Every CEF 150 browser, Alloy-style windows included, runs on the Chrome bootstrap: Chrome's profile and prefs services
and its component and download delegates. Two launchers' CEF (`Chrome/126.0.6478.183` and CEF 108) ran on the Alloy bootstrap, so
their process shape is not evidence for this one. Alloy style is still used, for the windowed browser with native
chrome.

**`CefSettings`:** `no_sandbox = 1`; `multi_threaded_message_loop = 0`; `root_cache_path` = `cache_path` (§3.4);
`persist_session_cookies = 1`; `log_severity = WARNING` with `log_file` set (§3.4); `locale = "en-US"`;
`javascript_flags = "--jitless"`.

**Preferences** (`CefRequestContext::SetPreference`): `plugins.always_open_pdf_externally = true` and
`browser.enable_spellchecking = false`. They replace `--disable-pdf-extension` and `--disable-spell-checking`: both
are still defined in [cef_switches.cc](https://raw.githubusercontent.com/chromiumembedded/cef/master/libcef/common/cef_switches.cc):82,94,
but nothing else in CEF's source reads them since the Alloy bootstrap went. H3 checks both prefs.

**Switches:**

| Switch | Why |
| --- | --- |
| `--in-process-gpu`, `--disable-gpu-watchdog` | One process less. The watchdog ended GPU processes at about +125 s for an unknown reason ([PLAN.md](PLAN.md):1044-1047) |
| `--renderer-process-limit=1`, `--disable-site-isolation-trials` | One renderer. With no sandbox, site isolation buys little |
| `--enable-features=NetworkServiceInProcess2` | Network service in the browser process. CefSharp calls this not officially supported |
| `--disable-features=AudioServiceOutOfProcess,SpareRendererForSitePerProcess,BackForwardCache,AsyncDns,DnsOverHttps` | No audio process, no spare renderer, no cached pages. Every lookup goes through `getaddrinfo`, the resolver the `dnsapi` and `launcher-tls` cases gate. One switch only: a repeated switch keeps its last value, so the host test asserts it appears once |
| `--enable-low-end-device-mode` | Wine reports 11,711 MiB (patch 1005). This sizes Chromium for 512 MiB ([sys_info.cc](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/base/system/sys_info.cc)) and cuts the discardable limit from 512 to 64 MiB (`GetDefaultMaxBytes`, [discardable_shared_memory_manager.cc](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/components/discardable_memory/service/discardable_shared_memory_manager.cc)) |
| `--js-flags=--jitless` (through `javascript_flags`) | No runtime code, so no JIT conversions, and less attack surface with no sandbox. About 40% slower on Speedometer 2 ([v8.dev](https://v8.dev/blog/jitless)). **No WebAssembly at all**: V8 exposes it under jitless only with the DrumBrake interpreter (`expose_wasm` in `src/wasm/wasm-js.cc`), and `v8_enable_drumbrake` is false outside tvOS (`gni/v8.gni`) |
| `--disk-cache-size=67108864` | A 64 MiB disk cache |
| `--use-angle=<H3>`, `--disable-direct-composition` | The backend that passes H3 (§3.2). Wine has no working DirectComposition |
| `--disable-quic`, `--no-proxy-server` | TCP+TLS only, as the two launchers' CEF used; Winsock error 10045 was open with CEF ([PLAN.md](PLAN.md):3147-3149). No WPAD lookups |
| `--disable-extensions`, `--disable-component-update`, `--disable-background-networking`, `--autoplay-policy=user-gesture-required` | No background work. `--disable-extensions` is Chrome's switch now, not CEF's; H3 checks it holds |
| `--force-gpu-mem-available-mb=<C2>` | Caps tile memory |

No `crash_reporter.cfg` is shipped, so no crashpad processes. `--single-process` is not used or measured: CEF's
maintainer wrote "The single-process flag is not supported" ([cef#4067](https://github.com/chromiumembedded/cef/issues/4067),
2026-01-12; crashes after M138), and a crashing Chromium session is the kind of run that freezes this console.

**Security posture.** There is no sandbox. It is untested in PS5 payload helpers, and each sandboxed process type
would be one more helper with its own code copies. ([Wine 57118](https://bugs.winehq.org/show_bug.cgi?id=57118) is a
sandbox failure of CEF's 32-bit sample under WoW64; its 64-bit build works.) Three things compensate: jitless V8, HTTPS
only with no certificate-error bypass, and LTC fixes (decision 10). A renderer exploit would run in the user's prefix;
decision 2 names what that exposes.

### 3.2 Graphics on the console

Two candidate paths, both presenting into the browser's own child HWND on the window-surfaces route, the desktop
default ([ps5/src/proton_launch.cpp](../ps5/src/proton_launch.cpp):714):

- **ANGLE → D3D11 → DXVK → winevulkan → GPU broker → RADV.** It is proven only with the vendors' own Microsoft
  compiler: in every local klog of the two launchers, `D3DCompiler_47.dll` and `libglesv2.dll` load as native from the
  vendor's CEF folder. One drew 248 frames; the other ran ANGLE D3D11 at feature level
  11_1 ([PLAN.md](PLAN.md)). With Wine's builtin `d3dcompiler_47` it has never run, here or on the host. CEF's
  [README.redistrib.txt](https://raw.githubusercontent.com/chromiumembedded/cef/master/tools/distrib/win/README.redistrib.txt)
  says canvas, 3D CSS and WebGL "may fail" without its bundled copy.
- **ANGLE Vulkan → winevulkan.** No DXVK and no D3D compiler, but an unexamined loader. On Windows, ANGLE builds with
  `angle_use_custom_libvulkan = !is_android && !is_fuchsia` and opens `vulkan-1.dll` only from its own module
  directory (`SearchType::ModuleDir`, `src/common/vulkan/libvulkan_loader.cpp`). So it works only if Wine substitutes its
  builtin when the app-folder file is missing, or with CEF's Khronos `vulkan-1.dll` in the folder finding winevulkan
  through the registry ICD. Neither is verified.

H3 runs four arms on the Linux oracle: D3D11 with Wine's builtin compiler (`WINEDLLOVERRIDES=d3dcompiler_47=b`),
Vulkan with no `vulkan-1.dll`, D3D11 with CEF's bundled compiler, and Vulkan with CEF's Khronos loader. Each records
`WINEDEBUG=+loaddll`, ANGLE's shader-compile errors in `debug.log`, whether `vkCreateInstance` reached winevulkan, and
the fixture page's sentinel colours. The first arm that passes, in that order, is C1's default. Any arm that needs a
CEF-shipped Microsoft or Khronos file makes that file a pinned, licensed input under rule 6 (decision 5), settled
before C1. Every console run records which compiler and loader were loaded, and whether ANGLE fell back.

Not chosen: software compositing (a fully transparent window, [PLAN.md](PLAN.md):518-520; "hardware acceleration must
stay on", [CONSOLE_QUEUE.md](CONSOLE_QUEUE.md):237), and Mesa/Zink, which no Chromium run has used. A minimized window
calls `was_hidden(1)` so it stops producing frames; a launcher's CEF kept drawing at 2-4 fps behind a game it started
([CONSOLE_QUEUE.md](CONSOLE_QUEUE.md):274).

### 3.3 Memory budget

**What the guard counts.** The census is flexible-class resident memory (FMEM, `sceKernelGetResidentFmemCount`)
summed over every console process except the title: the console's own services plus Wine's helpers
([src/pw_memory_helper.c](../src/pw_memory_helper.c):195-235, [pw_memory_helper.h](../src/pw_memory_helper.h):35-40).
The guard samples that whole figure, `census.system` ([native/proton_wine.cpp](../native/proton_wine.cpp):1826). It
asks the largest process to close after 20 of 30 s at 870 MiB, ends it after 3 s past 940, and ends it at once on a
climb of 120 MiB in 8 s near the limit ([src/pw_memory_guard.h](../src/pw_memory_guard.h):28-35). The kernel's
"RSS x of N" is the same figure (919 MiB census beside `RSS 928.9`, [PLAN.md](PLAN.md):1186-1187). N is 952 to
1,065 MiB depending on the boot (:497, :1219).

**What counts, and what does not.** Private memory (PartitionAlloc, the V8 heap, `VirtualAlloc`) is served from direct
memory and costs nothing against either figure: 1,280 MiB of heap left RSS flat (`memcap-heap-2026-10-12-01`,
[PLAN.md](PLAN.md):493-500). What counts is sections (Chromium's shared memory: discardable memory, GPU transfer
buffers, shared images), JIT code copies, thread stacks, libc and Wine's Unix side. [PLAN.md](PLAN.md):516 still says
the census includes direct pages. That line is stale, and the commit that lands this plan should correct it.

**The ceiling for all MihawkBrowser processes:**

| Term | MiB | Source |
| --- | --- | --- |
| The guard's limit | 870 | `src/pw_memory_guard.h` |
| − the console's own share (`system` − `helpers`), p90 | 249 | 6,271 `Wine: system memory` lines in local title logs, 2026-10-02 to 10-11: median 147, p10 132, max 316 |
| − the idle Mihawk OS desktop | 137 | 9 helpers before patch 1127 ([PLAN.md](PLAN.md):583). Today's desktop runs 15 helpers and converts 57,552 KiB of code (`gate-2026-10-15-01-desktop-session`), so C0 re-measures it |
| − an installer before the hand-off | 150 | Estimate: 1-2 helpers plus msiexec |
| − spike headroom (the steep-climb size) | 120 | `src/pw_memory_guard.h` |
| **= ceiling** | **214, set at 200** | 316 at the median share |

**Target 150 MiB, ceiling 200 MiB**, both measured as the session census minus the idle desktop read in the same run.
At that peak the kernel figure is about 736 MiB, 216 below the lowest N on record (952). At the guard's 870 it is 82
below.

| Item (browser + renderer) | Expected | Note |
| --- | --- | --- |
| Helper baselines: stacks, libc, Wine's Unix side | 50-120 | 25-60 each, before 1127 ([PLAN.md](PLAN.md):1338) |
| Discardable memory | ≤ 64 | Low-end mode caps it |
| GPU host-visible memory | about 0 | Direct memory since patch 0009 (§6, risk 3) |
| Code copies | unknown | Up to 176.2 MiB per process; C0 measures |

The baselines and the discardable cap already approach the ceiling, so code copies decide whether CEF fits. Patch 1127
copies each executed host page, plus same-protection neighbours within a 256 KiB window (`PS5_EXEC_CLUSTER`, "a first
guess"), into a per-process JIT object charged to flexible memory ([PLAN.md](PLAN.md):429). The figures on record are
run totals summed over all helpers ([tools/run-console.py](../tools/run-console.py):183-193): dnsapi64 30,160 KiB over
9 helpers, a game workload 70,464 KiB over 10. They do not show a per-process cost.

**Levers if C0 or C1 is over, in order:**

1. **Shared code copies, in the backend.** One JIT object per (image file, offset, load base) cluster, made once and
   mapped by every helper that loads the same DLL at the same base. It helps every CEF launcher
   as much as the browser. Not yet shown possible: sharing a JIT object between payload processes is unmeasured.
2. GPU in process or out of process (C2's first pair).
3. The other ANGLE backend.
4. `--force-gpu-mem-available-mb`.
5. A memory-pressure signal from the title's census into Chromium. This is not in v1.

The 200 MiB ceiling is a run pass threshold, not a runtime limit. In normal use the title's 870/940 guard applies. If
the guard closes the browser (in process GPU makes the browser the largest process), the browser cancels its
downloads in the teardown order (§3.5), deletes the partial files and exits.

### 3.4 Profile, cache and logs

| Item | Location |
| --- | --- |
| Profile, cookies, `Cache`, `Code Cache`, `GPUCache` | `%LOCALAPPDATA%\MihawkBrowser\User Data` (`root_cache_path` = `cache_path`); the 64 MiB disk-cache cap applies |
| Chromium log | `%LOCALAPPDATA%\MihawkBrowser\debug.log`, warnings and above. It records URLs, so it is never copied into evidence |
| Evidence lines | The browser's own `browser:` lines on stderr, which land in the helper log |

The `browser:` lines carry no query, fragment, userinfo or address (rule 5):

- `process type= pid=`
- `load scheme= host= path_sha1= status= ms=`. A fixture host is written as its role (`trusted-fixture`,
  `untrusted-fixture`), and an IP literal as `ip`.
- `download start/done host= bytes= ms= sha256=`
- `handoff child= window_ms= exiting`

The formatter lives in `pw_browser_policy.c`. Its host test feeds a URL with a query, a fragment, userinfo and an IP
literal, and asserts that none of them survive. `tools/run-browser.py` passes every line through `console_log.redact()`
and `tools/privacy_check.py` before writing evidence. Backups leave the cache folders out ([MIHAWK_OS.md](MIHAWK_OS.md)).

### 3.5 Downloads and the hand-off to installers

1. A start-view button starts its installer download from the browser process, with no page loaded. H3 counts
   processes on that path. If `StartDownload` still starts a renderer, `CefURLRequest` writes the file instead.
   `on_before_download` continues to `Downloads\<sanitized name>` with no dialog. A tile download is accepted only
   from its expected host and with its expected type, and the host test checks both.
2. `on_download_updated` feeds the list. Open is enabled only on `is_complete`. Expect 14-19 MB/s from the network
   ([PLAN.md](PLAN.md):1157-1162) and 54.6-56.5 MB/s to disk ([PLAN.md](PLAN.md):1256-1265).
3. **Open** calls `ShellExecuteExW`. In practice that is a PE64 parent starting a PE32 setup program under WoW64, or
   msiexec, and some installers ask for elevation. None of that is gated: the shell probe starts a PE64 copy of itself
   ([tests/shell-probe.c](../tests/shell-probe.c):443-475). Four fixtures cover it (H3, C4).
4. **Hand-off.** When the opened file is an installer and the setting is on, MihawkBrowser waits for the child's first
   visible window and for no other download to be active, then exits. If no window appears, it stays open. Teardown
   order, host-tested: cancel, wait for the file's last write and close, `CefShutdown`, exit.
5. **A second Chromium.** If another process starts MihawkBrowser with a URL while a Chromium-based process runs (any
   process whose command line has `--type=gpu-process` or `--type=renderer`), a native "Open link / Copy link" prompt
   appears before CEF is created.
6. **Popups.** `on_before_popup` cancels every popup and loads its target in the current view. `window.open` without a
   user gesture is blocked. Both are host-tested.

Installer evidence on record: Wine Mono/Gecko MSIs through wineboot, and 7-Zip's MSI on the older architecture
([docs/history/2026-10-07/docs/WINDOWS_PROCESSES.md](history/2026-10-07/docs/WINDOWS_PROCESSES.md):669-673).
A launcher's service installer (`/install`) was run by hand on copied-in files ([PLAN.md](PLAN.md)), and the service is registered
but not started (:1008). No launcher's own setup program has run. Elevation has no evidence at all. Known blockers for
the per-launcher rows: one launcher's CEF died of B3; another's service; the other four have never run.

### 3.6 Build, pins and install location

**`tools/fetch-cef.py`** (new, idempotent, run by `tools/fetch-all.sh`):

- Pins `cef_binary_150.0.22+gc1a097e+chromium-150.0.7871.256_windows64_minimal.tar.bz2` (index SHA-1
  `01011e3f5b05caacb3b724c7965b047ffd33fd18`, plus our SHA-256). Records the full CEF commit from
  `include/cef_version.h`, and Chromium 150.0.7871.256.
- Builds the ship list from the archive's own `Release/` and `Resources/` listing through a name allowlist, and fails
  when an expected name is missing.
- Writes `provenance.json` with each file's SHA-256, as `tools/fetch-cascadia-mono.py` does. A re-pin is a reviewed
  change, and it re-checks the launcher URL list (§4).

**Ship list:** `libcef.dll`, `chrome_elf.dll`, **`libEGL.dll`, `libGLESv2.dll`** (ANGLE: both GPU paths live in
them), `icudtl.dat`, `v8_context_snapshot.bin`, `resources.pak`, `chrome_100_percent.pak`, and every
`locales/en-US*.pak` (CEF ships gendered variants beside `en-US.pak`: Arch's `cef 152.0.6` lists
`en-US_FEMININE.pak` and others). `d3dcompiler_47.dll` and `vulkan-1.dll` ship only if the H3 arm that wins needs them
(decision 5). Not shipped: `vk_swiftshader*`, `dxcompiler.dll`, other locales, `chrome_200_percent.pak`.

**`tools/build-mihawk-browser.sh`** (new): compiles `src/pw_browser.c` and `src/pw_browser_policy.c` with clang
`--target=x86_64-windows -Werror` against `include/capi` with the API version pinned, links `Release/libcef.lib` and
Wine's import libraries, and runs `tools/audit-pe.py` on the result. `tools/build-production.sh` calls it, so the gate
fingerprint covers it. x64 only.

**Install location (decision 13): once, read-only, in the runtime, outside every prefix.** `tools/stage-wine.py` puts
it in `runtime/<ge>/lib/mihawk-browser/` with its own content-version stamp, reached through the `z:` → `/` mapping
that `stage-wine.py`:210 already creates. The prefix bundle cannot hold it: `pw_wine_prefix_prepare` syncs only the
bundle's `windows` directory ([wine/ps5/pw_wine_prefix.h](../wine/ps5/pw_wine_prefix.h):4-10, `stage-wine.py`:75)
into every prefix at its initial launch ([SHARED_PREFIX.md](SHARED_PREFIX.md)). A Program Files leaf would copy
hundreds of MB into every game prefix. With one copy, every prefix pays the disk cost once and every association and
shortcut resolves. CEF writes only under `%LOCALAPPDATA%`, and H3 runs it from a read-only folder to prove that.

**Licences.** `share/licenses/cef/` holds `LICENSE.txt`, `CREDITS.html` and the LGPL-2.1 text. **`NOTICE.md`** gets:

- the version, the full CEF commit, the Chromium tag, and the archive SHA-1 and SHA-256;
- the BSD-3 notice, the unmodified binaries, and where the exact CEF, Chromium and FFmpeg source can be obtained
  (FFmpeg's LGPL-2.1 parts are linked into `libcef.dll`,
  [README.chromium](https://chromium.googlesource.com/chromium/third_party/ffmpeg/+/refs/heads/master/README.chromium));
- Google, Chromium, the launcher vendors and DuckDuckGo in the trademark paragraph.

That reading is not legal advice.

**Host tests** (`tests/test_pw_browser_policy.c`, through `tools/host-tests.sh`):

- address input to a URL or a search; the HTTPS upgrade and the HTTP refusal;
- certificate errors: `on_certificate_error` returns false, so there is no proceed path;
- download names, collisions, installer recognition, and each tile's host and type;
- the privacy formatter;
- popups, the second-Chromium prompt, the hand-off condition and the teardown order;
- the exact switch list (with `--disable-features` once) and the two prefs.

The backend gets a host test of a 1 TiB `MEM_RESERVE` with sparse commits under the fake kernel, as the
`test_pw_wine_dmem_*` tests model the console (risk 4).

### 3.7 Shipping in Mihawk OS

| Surface | Change | Code that owns it |
| --- | --- | --- |
| Start | `MihawkBrowser.lnk` in the common Programs folder, which `pw_start_catalog` already scans (`CSIDL_COMMON_PROGRAMS`). The built-ins table resolves only names in `system32` and the Windows directory, so it stays unchanged. Pinning follows decision 9 | [wine/ps5/pw_shell_start.c](../wine/ps5/pw_shell_start.c):108-121 |
| Desktop | `MihawkBrowser.lnk` in `C:\users\Public\Desktop`, owned by the shortcut manifest, which rewrites both shortcuts when the runtime path changes | [src/pw_shell_link.h](../src/pw_shell_link.h) |
| Taskbar | A pinned app after Terminal, added first to the prototype, then to `pw_taskbar_layout` (patch 1140, `main` `0d04fc0`) | `src/pw_taskbar_layout.h` |
| Default browser | A Wine patch to `loader/wine.inf`: `http` and `https` `shell\open\command` go to the runtime's MihawkBrowser, plus `Clients\StartMenuInternet` and `RegisteredApplications`. Every prefix reaches the one install, so nothing dangles. `htmlfile` stays with Wine, because v1 refuses `file://`. A user's `HKCU\Software\Classes` choice wins | `wine/patches/` |
| `.exe` / `.msi` | No change | — |

## 4. UI

**The chrome is native Win32, not HTML inside CEF.** It adds no process and no extra GPU frames, it is drawn by the
theme that already matches the prototype, and a hung page cannot take it down. HTML chrome would be a second
`CefBrowser`: it would share the page's renderer (a page hang hangs the toolbar), or it would need its own renderer
and code copies. CEF's Views toolkit and Chrome style are not Windows 11.

The layout follows [DESIGN.md](../DESIGN.md) and the approved prototype (Proton UI type, 1920x1080 at 96 DPI):

- **Frame.** The standard 32 px title bar and 46x32 caption buttons, already pixel-exact (patch 1139). No tabs in v1.
  If tabs come later, they go in the title bar as WinUI TabView, the way the prototype's Explorer, Notepad and
  Terminal draw them (`prototypes/windows11-shell/js/apps/explorer.js`:2, :395; `small.js`:57).
- **Command row.** Back, Forward and Reload/Stop as 32x32 subtle icon buttons. A 32 px address box: 4 px radius, an
  accent 2 px bottom line on focus, a lock glyph. Downloads, with progress while active. More (…): Settings, About,
  Exit.
- **Start view.** Four native buttons with vendor names (no logos) and a search box, drawn by the toolbar's code. The
  URLs live in one data file, `launchers.json`, with the URL, the expected host and the file type for each.
- **Errors.** A native panel with the net error name (`on_load_error`), not Chromium's error page.
- **Downloads flyout.** Anchored under its button, 360 px wide. Each row has the shell icon, the name, the size or a
  ProgressBar, then Open (accent for a finished installer), Show in folder and Cancel. Its material is patch 1141's
  acrylic if that backdrop can serve a flyout; otherwise the user approves the opaque fallback.
- **Glyphs.** Fluent System Icons (MIT), pinned in `prototypes/windows11-shell/assets/icons/fluent/upstream.json` and
  `themes/windows11/icons/fluent/upstream.json`. New toolbar glyphs are recorded there and go through
  `tools/build-mihawk-icons.py`. The application icon is new original artwork (decision 11). The icon review checks it
  cannot be mistaken for the Chrome, Chromium or Edge logos.
- **No Microsoft marks.** No Edge or Windows logos, no Segoe Fluent Icons font, no Microsoft product names.
- **Input.** The touchpad pointer, plus USB keyboard and mouse ([PLAN.md](PLAN.md):912-913). The native buttons take
  keyboard focus (Tab and arrows), so they follow the controller-drives-focus decision
  ([WINDOWS11_PLAN.md](WINDOWS11_PLAN.md):159) once it reaches applications. Page content needs the pointer.

The surface is added to `prototypes/windows11-shell` (`js/apps/browser.js`) and the taskbar pin for the user's approval
first. The native window is then checked against it the way `explorer_frame_verdict` checks Explorer.

### 4.1 Approved in the prototype (2026-10-16)

The owner approved the surface with the Downloads **flyout**, the taskbar pin after Terminal and icon **B3** (an
original window with a crimson title bar and a gold wing; `assets/icons/object/app-browser.svg`, nothing of the
Chrome, Chromium or Edge marks). Prototype: `js/apps/browser.js`, `css/apps/browser.css`, captures
`?open=browser|browser-page|browser-loading|browser-error|browser-downloads`. No console run was made for this.

**The reference** is [windows11-reference/mihawk-browser.json](windows11-reference/mihawk-browser.json), written by
`node tools/measure-browser-prototype.mjs <out.json> [png dir]` (headless Chromium 154 on `CDP_PORT`, the prototype at
`PROTOTYPE_BASE`): for the five states in light and dark, every part's box in client coordinates (the window body,
under the 32 px title bar and inside the 1 px border), each text line's ink box, the computed colours and type, and
composited pixels at named points, including hover and press on Reload and on a launcher. The prototype's window is
1280 x 800 at 320,116, client 1278 x 766 at 321,149; a check places the native window there, as desktop-session places
Explorer at 410,166.

**Already equal** (`src/pw_browser.c` against the measurement): the command row (40 px; Back, Forward, Reload at x 4,
40, 76, y 4, 32 x 32; the address box from x 112 to width - 40; Downloads at width - 36), the solid base (243 / 32),
and the subtle buttons' hover and press (light 234 / 237, dark 45 / 40: `load_theme`'s blends give the same levels).
The start view's positions agree too: text from client y 80, launchers 200 x 40 at (width - 616) / 2, y 160 and 208.

**What the native window must change to match** (each measured in the reference):
1. **Downloads as a flyout**, not the docked full-height panel: 360 px wide at client (width - 364, 40), its height
   from its rows (265 px with one running and one finished download), on the flyout material (8 px radius, flyout
   stroke and shadow; light fill 252, dark 44). Header "Downloads" (14 px semibold, 40 px tall); rows divided by a
   1 px divider: the 32 px icon at row (16, 11), the name at x + 60 (Body), a 3 px ProgressBar while running, the
   status (Caption, secondary), then Cancel, or Open (accent for an installer) and Show in folder, as 32 px Buttons.
2. **Downloads' progress under its glyph**: a 16 x 3 ProgressBar at client (width - 28, 29) while a download runs.
3. **The address box as a WinUI TextBox**: fill 255 / 30, top stroke 228 / 47, a 2 px accent line when focused
   (0,95,184 / 96,205,255), the placeholder in secondary text, and the lock glyph (16 px, at x + 11 inside the box)
   on a loaded https page, the text starting 34 px in.
4. **Start view type and controls**: the two lines in Body (14/20 px, regular), not the message font; the launchers
   as WinUI Buttons (fill 252 / 45, top stroke 228 / 53, bottom 204 / 47; hover 246 / 50, press 244 / 39).
5. **The error panel**: title in Subtitle (20/28 px semibold) and the net error name in Body secondary, centred in
   the page area, instead of a top-aligned STATIC.
6. **Icon B3** for the window, the shortcut and the pin (16-256 px through `tools/build-mihawk-icons.py`), replacing
   the interim Fluent globe; the **taskbar pin after Terminal** and the Start pin in the native shell.

**Drawn as approved (host, 2026-10-17; no console run yet).** `src/pw_browser.c` now draws items 1-5 and the
window's and shortcut's icon B3. Colours, plates and the flyout's geometry are `src/pw_browser_chrome.c`
(host-tested against the reference's pixels and boxes, `tests/test_pw_browser_chrome.c`); the Downloads flyout is a
popup on backdrop 7 (Wine patch 1160), so the compositor draws its acrylic, stroke and shadow as it does the shell's;
text is Proton UI laid out at its design advances with Blink's baseline (GDI alone rounds every advance, 8 px over a
line). The check is `tools/check-browser-reference.py`: a host run's `snapshot NAME` steps
(`tools/run-browser-host.py --snapshots [--dark]`) against the reference's pixels, and, given the prototype's own
renders (`node tools/measure-browser-prototype.mjs <json> <dir>`), each fixed text's ink box. Light and dark each pass
89 of 89 (67 pixels, 22 texts within 2 px). Not covered on the host: the compositor's flyout material, stroke and
shadow; the EDIT's own text, which keeps GDI's hinted advances; the flyout's opening animation (none).

**The pins (host, 2026-10-17; browser branch only, as MihawkBrowser is not on main yet).** The taskbar has five pins,
MihawkBrowser after Terminal (`wine/ps5/pw_shell_taskbar.h`), and Start has it as the built-in after Terminal
(`wine/ps5/pw_shell_start.c`), both started through the Public Desktop shortcut the title keeps. Their pictures are the
prototype's own: `browser` and `icon-browser` were captured alone with the reference set's settings and added to
`themes/windows11/{taskbar,start}/rest-*.json`; every existing sprite there stayed byte-identical (a fresh five-pin
capture of the moved taskbar icons differs by at most one level in about 100 channels, so it was not taken). The layout
for five apps equals the capture's boxes (`tests/test_pw_taskbar_layout.c`). Start pins saved before this get the browser
once, after Terminal (`BrowserPinned`). **Before merging into main**, the shell scorecard's reference set
(`PS5_Proton-design/.impeccable/reference-1920`) needs the pin too: the design and port sessions recapture it together;
it is not edited here. Still to do: a console capture of all of it.

The prototype titles a loaded page "<host> - MihawkBrowser" because its stand-in pages have no title; the native
window keeps the page's own title ("<title> - MihawkBrowser").

## 5. Proof plan

### Host (no console)

1. **H1, pins and policy.** `tools/fetch-cef.py` verifies SHA-1 and SHA-256 and records the unpacked size.
   `tools/host-tests.sh` passes.
2. **H2, build.** `-Werror` is clean and `audit-pe.py` imports are as listed. The staged leaf fails unless `libEGL.dll`,
   `libGLESv2.dll` and every `en-US*.pak` are there.
3. **H3, the oracle: pinned Wine on Linux with the bundle's DXVK**, run from a read-only install folder:
   - the four graphics arms (§3.2). `chrome://gpu` is saved only after the measurement window, and it must name ANGLE
     with the chosen backend and report hardware-accelerated compositing;
   - the start view: process count on the tile path, which must show no renderer;
   - a LAN fixture page (`tools/run-tls-fixture.py`): `typeof WebAssembly` and a small Wasm page with the JIT on and
     off; `MediaSource.isTypeSupported` for H.264 and AAC, both expected false; a PDF and an editable field for the
     prefs;
   - downloads of 2 MiB and 64 MiB, SHA-256-exact, with the process count at each completion;
   - four installer fixtures built with `tools/build-wine-probe.sh`: PE32 `asInvoker`; PE32 `requireAdministrator`,
     whose child writes its `TokenElevation` into the marker; PE64; and a tiny MSI whose deferred custom action runs a
     PE32 EXE. Each is opened from the downloads list, each marker is checked, and the hand-off waits for the first
     window;
   - the browser process's flexible-class items (sections, threads and stacks) and its process count.

   Record `evidence/browser-host-<date>.json`. Host memory is a PC oracle, not console proof.
4. **H4, prototype.** The user approves the surface and the pin. H3's captures are compared with it. **Approved
   2026-10-16** (§4.1; reference `windows11-reference/mihawk-browser.json`).

### Console

The user is present for every run, because a Chromium session can freeze the console
([CONSOLE_QUEUE.md](CONSOLE_QUEUE.md):208). C0 and C1 come after the queued hang-safety commit B and MMU4K runs
(CONSOLE_QUEUE.md:67). After a restart, run `console_guard.py new-boot`, and run no payload tools during a run.

**Every run records:**

- **Per process** (one record per helper, not summed): type, census peak and settled, `vm resident`,
  `direct_resident`, `converted_kib`, `conversions`, the `mmu4k runs/insns/exits` line and `opened=`
  ([MMU4K.md](MMU4K.md) §10), the foreign-entry and holes report, and the largest reservation. `convert_failed`,
  `exec_refused` and `sdk_refused` must be 0.
- **System:** `census.system` and `census.helpers` over the idle desktop's first 20 s, their peaks, the kernel's
  "RSS x of N" peak, and guard actions (none allowed).
- **Graphics:** the loaded `d3dcompiler_47` and `vulkan-1` paths, any ANGLE fallback, fps while scrolling,
  `compose_avg_us`, and frames while minimized (must be 0).
- **Pages and downloads:** load ms, bytes, MB/s and SHA-256, through the §3.4 lines only.
- **Teardown:** time until every helper is gone, the title's return, and `console_guard`'s storage state. Any
  storage line stops the work.

C1 to C7 run with `PROTON_SYSTEM_MEMORY_LIMIT` set to C0's idle desktop census plus 200 (plus 150 where an installer
runs), with `PROTON_SYSTEM_MEMORY_END` unset, so the steep-climb rule stays on
([native/proton_wine.cpp](../native/proton_wine.cpp):1506-1526). An overrun then shows up as a recorded guard close,
not a near-freeze.

| Run | Question | Must show |
| --- | --- | --- |
| **C0** `cef-cost`: the queued B3 run with the CEF launcher already on the console (no new code beyond per-helper tallies in evidence) | What does one CEF process cost on the new backend? | The idle desktop's `system`/`helpers` in the first 20 s; per-helper `converted_kib` for a 176 MiB-text `libcef.dll`; the GPU process's census, `vm resident` against `direct_resident`, and `gpu-broker dmem_host` made = freed. **Go/no-go:** no CEF process above about 60-80 MiB converted |
| **C1** `browser-local` (90 s, no network) | Does CEF 150 run here, and what does it cost? | Window drawn, chrome pixels match, sentinel colours, GPU status from `debug.log`. Processes 2 plus named transients. MihawkBrowser census ≤ 200 MiB over the idle desktop. Page-load time within 3x of H3. Clean close |
| **C2** A/B, 90 s each | Which choices pay? | First GPU in process against out of process, judged on memory and on what the guard does in each. Then JIT on (its code-copy cost) and the other ANGLE arm. The `--force-gpu-mem-available-mb` value |
| **C3** `browser-tls` (LAN fixture) | Does Chromium's TLS and root handling work? | The run's fixture root is imported into HKCU `ROOT` before start and removed after, under `run-tls-fixture.py`'s lifetime rules. Trusted loads; untrusted ends in `ERR_CERT_AUTHORITY_INVALID` with no proceed. One vendor HTTPS page covers the Chrome Root Store path, with the net error code of any failure |
| **C4** `browser-download` (LAN) | Downloads and hand-off | 2 MiB and 64 MiB files byte-exact; MB/s and census during the download; the process count at completion. All four fixtures open from the list, with markers checked. The hand-off happens after the child's first window. Closed only through Cancel, then Exit |
| **C5** real launchers, `work` prefix (internet) | The v1 job, end to end | For each launcher's own installer (four; sizes measured for the six then offered: 2,141,232 B to 264,237,056 B, one an MSI), a verdict per stage: downloaded, started, installer window, installer exit code, launcher window. `audit-pe.py` architecture and requested elevation for each download. Vendor pages that break without Wasm. Sizes are from HEAD requests on 2026-10-16 |
| **C6** soak, 10 min | Does it stay in budget? | Launcher sites one after another, scrolling, then minimize. Census every second under the ceiling, no guard action, 0 frames while hidden |
| **C7** teardown, last | Does closing under load hang the console? | Runs only after a close-while-writing fix, or with the download cancelled first: a session stopped with 1 GiB of writes in flight hung the console ([PLAN.md](PLAN.md):1210-1213). Close during a page load. No hang, helpers gone, the title returns, storage clean |

`work` is a tool-made entry, created like `proton-tls-test`: marked with `.ps5-proton-test`, carrying the desktop
entry's environment (`PROTON_LAYOUT=launcher`, `WINE_PS5_WINDOW_SURFACES=1`, `MIHAWK_DESKTOP=1`), and deleted
afterwards. So C5 runs in a real desktop session without touching `mihawk`, which holds the user's launcher.

### Gate cases (`GATE_CASES` in `tools/run-battery.py`)

- **`browser64`**: C1's essentials in 60 s or less.
- **`browser-download64`**: C3 and C4's essentials on the LAN fixture.

Both run with the lowered `PROTON_SYSTEM_MEMORY_LIMIT`. They are added after C6 passes. They run only in sessions with
the user present until the unattended criterion is met: C1 and `browser-download` repeated five times across two
boots, each with the kernel's "RSS x of N" peak at least 250 MiB below N, no guard action, and clean MMU4K and convert
counters. That decision is recorded in CONSOLE_QUEUE.md.

## 6. Risks and decisions

### Risks (each with its default handling)

1. **The Chrome bootstrap has never run here**, and all Chromium evidence predates the 2026-10-15 backend. *Default:*
   H3 measures it first, then C0 and C1. Nothing is claimed before them.
2. **Code copies per process (patch 1127).** `libcef.dll`'s text is 176.2 MiB. *Default:* C0's go/no-go, then shared
   code copies in the backend.
3. **The GPU spike (B3).** Chromium GPU processes reached 331-462 MiB ([src/pw_memory_guard.h](../src/pw_memory_guard.h),
   [PLAN.md](PLAN.md):1004, [native/proton_wine.cpp](../native/proton_wine.cpp):1829). It was flexible-class memory: the
   GPU broker's host-visible memory, made as physical shared memory (:1092-1097; direct heap and DLL images did not
   move, :497-500). Patch 0009 made direct memory the default for it (273 → 3 MiB flexible in a probe, :1326-1334), so
   it should have left the census. Not confirmed for Chromium. *Default:* C0 and C1 check `vm resident` against
   `direct_resident` and `dmem_host` made = freed. No GPU heap is budgeted unless they show one.
4. **The 4 KiB write tracking and huge reservations.** Chromium seals 4 KiB pages read-only (libcef's
   `VirtualProtect(page, 0x1000, PAGE_READONLY)`, [PLAN.md](PLAN.md):1019), and PartitionAlloc and V8 use 4 KiB guard
   pages. Stores beside them go through patches 1122/1134: 86 µs a store before MMU4K, a game workload at 2.0-2.4 fps
   ([MMU4K.md](MMU4K.md) §1). MMU4K has barely run on the console. V8's sandbox and PartitionAlloc's pools reserve far
   more address space than patch 1128's rules have handled. *Default:* the per-process MMU4K and foreign lines, C1's
   page-load bound, the 1 TiB host test, and C1 after the queued runs.
5. **Steep climbs freeze the console.** One went 670 → 919 MiB in 10 s ([PLAN.md](PLAN.md):1184-1190). *Default:*
   runs lower the limit with the steep-climb rule on; v1 cannot raise the limits.
6. **Two Chromium stacks at once.** *Default:* the hand-off, and the "Open link / Copy link" prompt when a launcher
   opens a link.
7. **The D3D compiler and ANGLE's Vulkan loader.** *Default:* the four H3 arms; decision 5 is settled before C1.
8. **No WebAssembly.** Challenge widgets that use it (hCaptcha's proof-of-work, some Cloudflare challenges) fail.
   *Default:* the direct-download buttons need no web page. C5 records which vendor pages break; decision 3.
9. **No sandbox.** *Default:* HTTPS only, no certificate bypass, jitless, LTC fixes. Decision 2 names the stake.
10. **The console's clock and DNS.** DNS returned SERVFAIL on 2026-10-15/16 because of the console's own setting
    ([CONSOLE_QUEUE.md](CONSOLE_QUEUE.md):35-40), and a wrong clock fails every certificate. *Default:* C3 runs first,
    DNS goes through `getaddrinfo`, and errors show in the native panel with the net error name.
11. **Closing while writing.** *Default:* the host-tested teardown order, C4 closes only through Cancel, and C7 runs
    last.
12. **Launchers themselves.** *Default:* per-stage verdicts in C5 and a PLAN.md row per launcher. A red launcher stage
    is not a browser failure.

### Decisions for the user (each with a default)

1. **Engine.** *Default:* CEF 150 LTC with our C client, subject to C0. If C0 fails, ship the native start view over
   WinHTTP while shared code copies land. ungoogled-chromium is the fallback only if the client stalls.
2. **Open web or launcher sites only.** *Default:* the open web, HTTPS only. The stake: there is no sandbox, and
   `mihawk` holds a launcher's saved sign-in ([PLAN.md](PLAN.md)), which a renderer exploit could read. An allowlist
   would guard that, but it would no longer be a general browser.
3. **Jitless.** *Default:* on, which means no WebAssembly. With the JIT on, every JIT code page is converted at its
   first execute (patch 1127); C2 measures that cost.
4. **Close the browser when an installer starts.** *Default:* on, after the child's first window and only with no
   other download active; a toggle in settings.
5. **GPU path and `d3dcompiler_47.dll`.** *Default:* the first H3 arm that passes, preferring no Microsoft binary.
   Shipping Microsoft's redistributable or the Khronos loader is a rule 6 question, settled before C1.
6. **Default browser.** *Default:* `http` and `https` through wine.inf, pointing at the one runtime install;
   `htmlfile` stays with Wine.
7. **Search engine.** *Default:* DuckDuckGo's script-free HTML endpoint (`html.duckduckgo.com/html/`). It is light and
   works without JIT.
8. **Taskbar pin.** *Default:* yes, after Terminal, approved in the prototype first. **Approved 2026-10-16** (§4.1).
9. **Pin in existing prefixes' Start.** *Default:* add once, then respect an unpin.
10. **Update policy.** *Default:* re-pin each CEF 150 LTC release within 14 days, each re-pin passing the gate. The LTC
    gets platform-agnostic fixes only, so Windows-only fixes may not reach it, and its last refresh is 2027-04-13. Move
    to M160, the next LTC, when it is published.
11. **Application icon and name.** *Default:* "MihawkBrowser" (`MihawkBrowser.exe`), with new original artwork that
    the user approves in the prototype. **Approved 2026-10-16:** the name, and icon B3 (§4.1).
12. **Chromium in the gate.** *Default:* add `browser64` and `browser-download64` after C6, run with the user present
    until the unattended criterion (§5) is met.
13. **Install location.** *Default:* once, read-only, in the runtime, outside every prefix (§3.6).
14. **Tabs in v1.** *Default:* none. If they are wanted, a cap of 2, drawn in the title bar.
15. **Start-view buttons download installers directly.** *Default:* yes. The URL list is data, re-checked at each
    re-pin.

## Review notes (2026-10-16)

The review's findings were checked against the repository, local klogs and primary sources, and almost all of them
are taken above. These parts were not taken:

- **The kernel figure as census plus the title's 60 MiB.** `pw_memory_helper.h`:35-37 defines the census as the
  `VM Stats: RSS` figure itself, and the 2026-10-11 freeze shows a 919 MiB census beside `RSS 928.9`. Headroom is
  computed without the extra 60.
- **"The census counts direct pages, so patch 0009 does not lower it", and "use decision 13's flexible-versus-census
  numbers in C1".** The census is FMEM only (first finding). The old decision 13 ("count flexible memory only?") is
  dropped: the guard already does, as `memcap-heap-2026-10-12-01` measured.
- **A smaller V8 heap and a tab cap of 2 as memory levers.** The V8 heap is private memory, which is direct and does
  not count, and v1 has no tabs.
- **Registering the association from the install step instead of wine.inf.** With one runtime-wide install, every
  prefix reaches the browser, so the wine.inf entry cannot dangle; `htmlfile` is dropped.
- **"Earlier launcher sessions logged CEF DNS warnings" (PLAN.md).** That line says the latest session had none.
  `AsyncDns` and `DnsOverHttps` are turned off anyway.

The commit that landed this plan also corrected [PLAN.md](PLAN.md)'s "direct-memory pages included" line and recorded
the gate's unattended criterion in [CONSOLE_QUEUE.md](CONSOLE_QUEUE.md).

## Sources

**Repository.** [PLAN.md](PLAN.md), [CONSOLE_QUEUE.md](CONSOLE_QUEUE.md), [MIHAWK_OS.md](MIHAWK_OS.md),
[SHARED_PREFIX.md](SHARED_PREFIX.md), [MMU4K.md](MMU4K.md), [WINDOWS11_PLAN.md](WINDOWS11_PLAN.md),
[DESIGN.md](../DESIGN.md), `src/pw_memory_helper.{c,h}`, `src/pw_memory_guard.h`, `native/proton_wine.cpp`,
`wine/ps5/pw_shell_start.c`, `wine/ps5/pw_wine_prefix.h`, `tools/stage-wine.py`, `tools/run-console.py`,
`tests/shell-probe.c`, `tests/tls-probe.c`, `prototypes/windows11-shell/js/apps/`, and `main` at `0d04fc0`.

**Local, not committed.** The title logs under `klog/` (census lines), two launchers' klogs (loaded DLLs;
only DLL and switch names were taken, because they contain account data), and a launcher's CEF 126 `libcef.dll` (size and
section table only).

**External**, all read on 2026-10-16:

- CEF: [index.json](https://cef-builds.spotifycdn.com/index.json), [branches](https://chromiumembedded.github.io/cef/branches_and_building),
  [README.redistrib.txt](https://raw.githubusercontent.com/chromiumembedded/cef/master/tools/distrib/win/README.redistrib.txt),
  [cef_switches.cc](https://raw.githubusercontent.com/chromiumembedded/cef/master/libcef/common/cef_switches.cc),
  [cef_download_handler.h](https://raw.githubusercontent.com/chromiumembedded/cef/master/include/cef_download_handler.h),
  issues [#3685](https://github.com/chromiumembedded/cef/issues/3685) and [#4067](https://github.com/chromiumembedded/cef/issues/4067)
- Chromium: [process model](https://chromium.googlesource.com/chromium/src/+/main/docs/process_model_and_site_isolation.md),
  [chrome_download_manager_delegate.cc](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/chrome/browser/download/chrome_download_manager_delegate.cc),
  [discardable_shared_memory_manager.cc](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/components/discardable_memory/service/discardable_shared_memory_manager.cc),
  [sys_info.cc](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/base/system/sys_info.cc)
- ANGLE: [src/common/vulkan/BUILD.gn](https://github.com/google/angle/blob/main/src/common/vulkan/BUILD.gn),
  [libvulkan_loader.cpp](https://github.com/google/angle/blob/main/src/common/vulkan/libvulkan_loader.cpp)
- V8: [flag-definitions.h](https://raw.githubusercontent.com/v8/v8/main/src/flags/flag-definitions.h),
  [wasm-js.cc](https://github.com/v8/v8/blob/main/src/wasm/wasm-js.cc), [v8.gni](https://github.com/v8/v8/blob/main/gni/v8.gni),
  [jitless](https://v8.dev/blog/jitless)
- Arch [cef 152.0.6](https://archlinux.org/packages/extra/x86_64/cef/) file list; Thorium-Win releases
- Wine bugs: [57118](https://bugs.winehq.org/show_bug.cgi?id=57118), [58921](https://bugs.winehq.org/show_bug.cgi?id=58921),
  [56378](https://bugs.winehq.org/show_bug.cgi?id=56378)
