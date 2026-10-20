#!/usr/bin/env bash
# Generate Proton Wine headers using the pinned Vulkan registry.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
tree=$(cd -- "$1" && pwd)
# The registry the pinned Wine commit carries (dlls/winevulkan), checked by hash below: a host's vulkan-headers
# package moves on (1.4.363 on the 2026-10-10 build machine did not match, and the build stopped here).
registry=${VULKAN_REGISTRY:-$tree/dlls/winevulkan}
(cd "$registry" && sha256sum --check <<'HASHES'
264d0d7350e37d70c82407fb430d085040fc01a9a961d43dec8c2d6ed1dfd183  vk.xml
d018b914014c06605e367a3b929670511e6f6de2f225c405a8b5e2d912408b76  video.xml
HASHES
)
cp "$registry/vk.xml" "$registry/video.xml" "$tree/"
(cd "$tree" && ./autogen.sh)
