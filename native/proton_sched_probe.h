// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PROTON_SCHED_PROBE_H
#define PROTON_SCHED_PROBE_H
// Measures the console scheduler's treatment of a title's thread priorities
// (native/proton_sched_probe.cpp) and prints "sched:" lines to klog.
void proton_sched_probe();
#endif
