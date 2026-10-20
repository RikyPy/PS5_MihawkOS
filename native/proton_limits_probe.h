// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTON_LIMITS_PROBE_H
#define PROTON_LIMITS_PROBE_H
// Measures, in the title's own process, the page size and whether a 4 KiB protection inside a
// 16 KiB page holds. "limits:" lines, also in /data/ps5proton/logs/limits-probe.txt.
//
// What the console allows a title, measured on firmware 10.01 (2026-10-04):
//  - pages: 16 KiB; a 4 KiB mprotect inside one returns 0 and protects the whole 16 KiB page
//    (limits-probe-01).
//  - The earlier imports of fork/link/symlink/readlink/ptrace were zero in the title
//    (limits-probe-01: SIGSEGV at rip 0). Loading libkernel_sys gets the title killed
//    during module start (limits-probe-02: SYSTEM_ILLEGAL_FUNCTION_CALL).
//    rfork itself is the SDK's raw-syscall shim; it is not a title-safe import.
//  - Ordinary libkernel does export rfork_thread. child-native-04 confirmed its
//    actual title binding, then ended in a user-confirmed kernel panic at or after
//    the RFPROC|RFFDG spawn boundary, without a recorded return. This does not prove
//    all native spawn paths impossible. See docs/CHILD_PROCESSES.md; do not repeat
//    that bare spawn. Wine process creation remains disabled.
// The probe does not repeat what crashes. scratch: unused, kept for a later broker test.
void proton_limits_probe(const char *scratch);
#endif
