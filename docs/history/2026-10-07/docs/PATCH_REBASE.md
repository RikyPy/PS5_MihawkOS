> Historical snapshot of `docs/PATCH_REBASE.md`, archived on 2026-10-07 from `9b07ebb`.
> Defaults, next steps, counts and failure classifications below belong to their original runs.
> Use the [current document](../../../PATCH_REBASE.md) for present guidance; see the [archive index](../README.md).

# Patch rebase status

Sequential replay from Wine `1c78596625336a73dbfc035dbbcf0dbdd95b06eb`, measured by `tools/check-patches.sh`.
The check uses a temporary tree and never resets or cleans the source checkout.
After the first conflict, later patches are untested because their prerequisites are missing.

| Patch | Sequential apply |
| --- | --- |
| `0100-ntdll-no-fstab-device-on-ps5.patch` | clean |
| `0101-server-realpath-into-server-memory.patch` | clean |
| `0102-server-user-shared-data-fills-a-host-page.patch` | clean |
| `0110-server-run-in-process-on-ps5.patch` | clean |
| `0111-ntdll-connect-to-the-in-process-server.patch` | clean |
| `0120-server-ps5-default-audio-driver.patch` | clean |
| `0130-ntdll-ps5-report-main-exe-load-failure.patch` | clean |
| `0140-ntdll-ps5-list-directory-by-name.patch` | clean |
| `0150-server-ps5-save-the-registry-in-1mib-writes.patch` | clean |
| `0160-kernelbase-stored-computer-name-without-wineboot.patch` | clean |
| `0161-kernelbase-drives-without-the-mount-manager.patch` | clean |
| `0400-win32u-ps5-user-driver.patch` | clean |
| `0410-win32u-ps5-fontless-metrics-and-server-desktop.patch` | clean |
| `0420-win32u-ps5-driver-cursor.patch` | clean |
| `0430-win32u-ps5-driver-window-view.patch` | clean |
| `0440-win32u-ps5-driver-cursor-edges.patch` | clean |
| `0450-winevulkan-host-display-extension.patch` | clean |
| `0455-win32u-vulkan-optional-host-external-capabilities.patch` | clean |
| `0456-win32u-vulkan-keep-swapchain-chain-when-scaling.patch` | clean |
| `0457-win32u-vulkan-placed-map-properties-khr.patch` | clean |
| `0460-win32u-ps5-driver-vulkan-display-plane.patch` | clean |
| `0470-xinput-ps5-title-controller.patch` | clean |
| `0500-ntdll-ps5-signal-context-and-segment-bases.patch` | clean |
| `0510-ntdll-ps5-host-page-size.patch` | clean |
| `0520-ntdll-ps5-ntdll-directory.patch` | clean |
| `0530-ntdll-syscall-dispatcher-slot-in-large-host-page.patch` | clean |
| `0540-ntdll-count-host-vm-calls-and-skip-unchanged-mprotect.patch` | clean |
| `0545-ntdll-keep-the-pages-that-fault-most-often.patch` | clean |
| `0550-ntdll-ps5-no-process-creation.patch` | clean |
| `0560-ntdll-stderr-output-sink.patch` | clean |
| `0570-ntdll-ps5-reserve-virtual-range.patch` | clean |
| `0580-ntdll-ps5-config-dir-parent-ownership.patch` | clean |
| `0590-ntdll-ps5-repair-null-fs-on-unix-entry.patch` | clean |
| `0600-ntdll-ps5-direct-memory-for-anonymous-memory.patch` | clean |
| `0620-ntdll-generic-processor-topology.patch` | clean |
| `0630-win32u-ps5-driver-display-modes.patch` | clean |
| `0640-win32u-ps5-flush-period.patch` | clean |
| `0650-win32u-ps5-black-surfaces.patch` | clean |
| `0670-win32u-ps5-relative-mouse.patch` | clean |
| `0680-server-ps5-wait-watchdog.patch` | clean |
| `0700-strmbase-renderer-state-wait-without-filter-lock.patch` | clean |
| `0730-server-ps5-anonymous-mappings-in-shared-memory.patch` | clean |
| `0740-win32u-ps5-vulkan-hardware-cursor.patch` | clean |
| `0750-proton-non-linux-build.patch` | clean |
| `0760-proton-ps5-cross-build-fixes.patch` | clean |
| `0770-ntdll-non-linux-link.patch` | clean |
| `0780-win32u-ps5-frame-capture.patch` | clean |
| `0790-ntdll-ps5-back-fixed-reservations.patch` | clean |
| `0800-ntdll-ps5-signal-fp-state.patch` | clean |
| `0810-win32u-ps5-scale-swapchains.patch` | clean |
| `0820-win32u-ps5-opengl-egl.patch` | clean |
| `0830-win32u-ps5-opengl-capture.patch` | clean |
| `0840-win32u-ps5-gpu-presentation.patch` | clean |
| `0850-wineps5-pe-driver-shell.patch` | clean |
| `0860-ntdll-unixlib-name-terminator.patch` | clean |
| `0870-ps5-dualsense-hid.patch` | clean |
| `0875-setupapi-skip-a-device-interface-that-cannot-be-created.patch` | clean |
| `0880-ps5-runtime-counters.patch` | clean |
| `0885-ntdll-ps5-end-killed-threads-at-the-user-mode-boundary.patch` | clean |
| `0886-server-ps5-durable-registry-and-flush-on-request.patch` | clean |
| `0890-ps5-in-process-synchronization-device.patch` | clean |
| `0891-ntdll-say-which-reservation-failed-on-ps5.patch` | clean |
| `0892-ps5-end-a-game-cleanly.patch` | clean |
| `0893-ps5-a-thread-releases-its-opengl-context-as-it-ends.patch` | clean |
| `0894-ntdll-say-which-threads-a-kill-reaches-and-ends.patch` | clean |
| `0895-winebus-let-the-ps5-bus-thread-leave-its-unix-call-when-killed.patch` | clean |
| `0896-server-ps5-native-thread-scheduling.patch` | clean |
| `0897-include-list-the-ps5-ntsync-header.patch` | clean |
| `0898-ps5drv-say-which-threads-register-for-the-title-input.patch` | clean |
| `0899-server-let-the-title-end-a-game-that-does-not-close.patch` | clean |
| `0900-ntdll-ps5-standard-output-to-a-file.patch` | clean |
| `0901-ntdll-ps5-a-directory-handle-knows-its-path.patch` | clean |
| `0902-server-ps5-poll-for-directory-changes.patch` | clean |
| `0903-server-ps5-read-and-write-the-processs-own-memory.patch` | clean |
| `0904-ntdll-ps5-volume-information-without-the-mount-manager.patch` | clean |
| `0905-ntdll-ps5-process-memory-counters-from-the-views.patch` | clean |
| `0906-ntdll-ps5-processor-times-from-the-process.patch` | clean |
| `0910-ntdll-ps5-thread-alert-without-a-descriptor.patch` | clean |
| `0911-ntdll-ps5-flush-process-write-buffers-by-waiting.patch` | clean |
| `0912-ntdll-ps5-free-memory-from-direct-memory.patch` | clean |
| `0913-ntdll-freebsd-no-battery-means-mains-power.patch` | clean |
| `0914-ntdll-ps5-a-missing-image-is-not-found.patch` | clean |
| `0915-ntdll-reading-a-directory-is-an-invalid-device-request.patch` | clean |
| `0916-ntdll-ps5-extend-narrow-syscall-arguments.patch` | clean |
| `0917-ntdll-ps5-extend-win32u-narrow-syscall-arguments.patch` | clean |
| `0918-server-ps5-run-requests-on-client-threads.patch` | clean |
| `0919-server-ps5-completion-ports-on-client-threads.patch` | clean |
| `0920-server-ps5-inproc-sync-lookup-on-client-threads.patch` | clean |
| `0921-server-ps5-snapshot-and-device-tags-for-hangs.patch` | clean |
| `0922-ntdll-ps5-report-signals-inside-the-sync-device.patch` | clean |
| `0923-ntdll-ps5-defer-signals-inside-the-sync-device.patch` | clean |
| `0924-win32u-ps5-report-the-refresh-rate-asked-of-the-display.patch` | clean |
| `0925-ntdll-ps5-service-managed-children.patch` | clean |
| `0926-ntdll-ps5-broker-descendant-spawning.patch` | clean |
| `0927-ntdll-ps5-owner-graphics-child.patch` | clean |

95 apply in order, 0 conflict, 0 untested
