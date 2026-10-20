// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// One-shot service queries and client-side rejected requests; never spawns.
bool proton_service_preflight(bool elevated, int &status);
// Dedicated experimental build only: fresh service-owned executable, no Wine/GPU.
bool proton_service_native(bool elevated, int &status);
