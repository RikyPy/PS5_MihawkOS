# Windows environment, boot and services

Current guidance, reviewed 2026-10-07. Windows child processes now run as payloads.
The old premise that wineboot/services cannot start is obsolete. Earlier missing
boot-state diagnoses remain in [the boot-state history](history/2026-10-07/docs/BOOT_STATE.md).

## Current setup contract

Each game has a separate Wine prefix. The title creates required directories and
drive mappings; Wine/wineboot populates the environment. The console's link-table
representation supplies drive mappings without native filesystem symlinks.
The runtime includes pinned Wine Mono and Gecko components.

Services are enabled during prefix creation/update when needed, or by an explicit
profile request. They are not automatically enabled for every ordinary game.
Payload spawning removes the previous Sony seven-helper ceiling, but services
still consume process slots, memory and startup time.

Earlier fresh-prefix tests verified system modules, registry version, APPDATA,
22 listed services, successful 64-bit MSI installation and execution of the
installed program. `prefix-14` and `prefix-15` completed in 87.7 and 88.2 s on
their tested layouts. See [prefix-14](../evidence/prefix-14.json) and
[prefix-15](../evidence/prefix-15.json).

## What still needs verification

These setup tests predate today's complete payload/Zink defaults. A fresh prefix
does not have `system32` until wineboot creates it; current Mesa staging happens
before that directory may exist. First-launch OpenGL acceptance is therefore open.
Do not claim every Windows service or driver works because the service manager
listed entries or one installer succeeded.

Historical mount-manager, volume naming, KnownDlls and hardware-registry
differences must be rerun under the current startup sequence before being marked
fixed or permanent. Keep boot-state fallbacks that still serve the port, and
remove/change them only with host and console evidence.

The supplied FNA game's installations require x86 execution. Creating a fresh prefix
or editing a CLR header cannot supply the missing 32-bit runtime/native dependencies.

## Related guidance

[Architecture](ARCHITECTURE.md), [processes](WINDOWS_PROCESSES.md),
[conformance](CONFORMANCE.md), [current plan](PLAN.md).
