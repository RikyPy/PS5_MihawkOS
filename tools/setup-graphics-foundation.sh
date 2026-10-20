#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Export only the pinned graphics foundation; never use its mutable working tree.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# The git checkout the pinned revision is exported from (read only); never a build folder.
fork=${PS5_VULKAN_GIT:-$root/../PS5_Vulkan}
revision=f410e67a0241b3e8edcdbcd2a9b285dfc4a412ed
out=$root/.deps/graphics-foundation
patches=("$root/platform/graphics/patches/"*.patch)
patch_stamp=$(cat "${patches[@]}" "$root/src/pw_window_snapshot.h" "$root/src/pw_window_page.h" "$root/src/pw_window_scene.h" "$root/src/pw_window_effects.h" "$root/src/pw_frame_pacing.h" "$root/src/pw_window_direct.h" "$root/src/pw_gpu_timing.h" "$root/src/pw_display_copy.h" "$root/src/pw_display_capture.h" "$root/src/pw_log_drain.h" "$root/src/pw_display_rate.h" "$root/src/pw_present_retry.h" | sha256sum | cut -d' ' -f1)
if [[ ! -f $out/.revision || $(cat "$out/.revision") != "$revision" || ! -f $out/.patches-sha256 ||
      $(cat "$out/.patches-sha256") != "$patch_stamp" || ! -f $out/tooling/gpu-broker/radv_ps5_platform.h ]]; then
    mkdir -p "$root/.deps"
    staging=$(mktemp -d "$root/.deps/graphics-foundation.XXXXXX")
    git -C "$fork" archive "$revision" tooling/gpu-broker tools/build-gpu-broker-client.sh \
        tools/build-mesa-windows-zink.sh tools/radv-link.sh tooling/native/ps5-pie.ld | tar -x -C "$staging"
    git -C "$fork/../PS5_Mesa" show 1ba9874f478657ccf6da72ce55f0260164cc0bd0:src/amd/vulkan/winsys/ps5/radv_ps5_platform.h > "$staging/tooling/gpu-broker/radv_ps5_platform.h"
    for patch in "${patches[@]}"; do
        patch --batch --forward -d "$staging" -p1 < "$patch" >&2
    done
    cp "$root/src/pw_window_snapshot.h" "$root/src/pw_window_page.h" "$root/src/pw_window_scene.h" "$root/src/pw_window_effects.h" "$root/src/pw_frame_pacing.h" "$root/src/pw_window_direct.h" "$root/src/pw_gpu_timing.h" "$root/src/pw_display_copy.h" "$root/src/pw_display_capture.h" "$root/src/pw_log_drain.h" "$root/src/pw_display_rate.h" "$root/src/pw_present_retry.h" "$staging/tooling/gpu-broker/"
    printf '%s\n' "$revision" > "$staging/.revision"
    printf '%s\n' "$patch_stamp" > "$staging/.patches-sha256"
    rm -rf "$out"
    mv "$staging" "$out"
fi
printf '%s\n' "$out"
