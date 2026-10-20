// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Consumes child-probe.txt before any workers or Wine start. The parent returns
// through main/CRT's shell-close path; only the native child returns to libkernel.
bool proton_child_probe(bool elevated, int &status);
