// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// docs/SELF_INSTALL.md phase 0, the title's side: an isolated build (PS5_INSTALL_PROBE) only, never production.
// The plan is /app0/install-probe.txt: the run's identity, then one step a line (proton_install_probe.cpp lists them).
// The answers go to install-probe-results.txt in the title folder, one line a step, for tools/run-child-probe.py.

// Before the /data request, single-threaded: what a title that is not root may create in /data (probe 3).
void proton_install_probe_preroot();
// After the grant and platform_init. True when the title should end now with `status`; false when it should go on
// into main's test run (the plan's "launcher" step: the launcher drawn while notifications are sent).
bool proton_install_probe(bool elevated, int &status);
