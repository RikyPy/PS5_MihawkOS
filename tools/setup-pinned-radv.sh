#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The PS5_Vulkan this build links RADV with, at the revision the graphics foundation pins
# (tools/setup-graphics-foundation.sh), with its RADV release archive built there.
#
#   tools/setup-pinned-radv.sh      prints the folder (a PS5_VULKAN_DIR for ps5/tools/build.sh, the --radv of
#                                   tools/build-wine-ps5.sh and the RADV checkout of tools/link_gpu_client.sh)
#
# The sibling ../PS5_Vulkan is read, never written: its working tree moves on by itself (on 2026-10-10 its RADV
# was rebuilt at Mesa 47e9eb6 while this build pinned 7b59ef2, and the GPU client's archive check stopped the
# build). The pinned revision is exported with git archive, the payload SDK its recipe pins is copied in after its
# revision is checked, and the recipe's own tools/build-radv.sh builds the release archive from a git archive of
# ../PS5_Mesa at its pinned revision. RADV is linked only with PS5_Vulkan's recipe (AGENTS.md rule 3): this is that
# recipe, at a pinned revision. Idempotent: a folder whose revision stamp and RADV provenance match is reused.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sibling=$(cd -- "${PS5_VULKAN_GIT:-$root/../PS5_Vulkan}" && pwd)
revision=$(sed -n 's/^revision=//p' "$root/tools/setup-graphics-foundation.sh" | head -1)
[[ $revision =~ ^[0-9a-f]{40}$ ]] || { echo "no pinned PS5_Vulkan revision in tools/setup-graphics-foundation.sh" >&2; exit 2; }
out=$root/.deps/ps5-vulkan-${revision:0:12}

if [[ ! -f $out/.revision || $(<"$out/.revision") != "$revision" ]]; then
    git -C "$sibling" cat-file -e "$revision^{commit}" ||
        { echo "$sibling does not have $revision" >&2; exit 2; }
    staging=$(mktemp -d "$root/.deps/ps5-vulkan.XXXXXX")
    git -C "$sibling" archive "$revision" | tar -x -C "$staging"
    printf '%s\n' "$revision" > "$staging/.revision"
    rm -rf "$out"
    mv "$staging" "$out"
fi

# The payload SDK the pinned recipe names (its tools/setup-native-dependencies.sh), copied from the sibling's cache
# only when that cache holds exactly that revision.
sdk_revision=$(sed -n 's/^sdk_revision=//p' "$out/tools/setup-native-dependencies.sh" | head -1)
sdk=$out/.deps/native/ps5-payload-sdk
if [[ ! -f $sdk/.ps5-sdk-revision || $(<"$sdk/.ps5-sdk-revision") != "$sdk_revision" ]]; then
    source_sdk=$sibling/.deps/native/ps5-payload-sdk
    [[ -f $source_sdk/.ps5-sdk-revision && $(<"$source_sdk/.ps5-sdk-revision") == "$sdk_revision" ]] ||
        { echo "the payload SDK $sdk_revision is not in $source_sdk; run that checkout's setup-native-dependencies.sh" >&2; exit 2; }
    mkdir -p "$out/.deps/native"
    rm -rf "$sdk.partial"
    cp -a --reflink=auto "$source_sdk" "$sdk.partial"
    rm -rf "$sdk"
    mv "$sdk.partial" "$sdk"
fi

# Two build artifacts the title's link takes from the checkout (ps5/tools/link-title.sh): its runtime libc.prx,
# checked against the pinned revision's libc.prx.sha256, and its host packaging tool, build/host/ps5-native-tool,
# which every gated title was linked with. Both are copied from the sibling, which built them.
if ! (cd "$out/runtime" && sha256sum --check --strict --quiet libc.prx.sha256 > /dev/null 2>&1); then
    cp -p "$sibling/runtime/libc.prx" "$out/runtime/libc.prx"
    (cd "$out/runtime" && sha256sum --check --strict --quiet libc.prx.sha256) ||
        { echo "$sibling/runtime/libc.prx is not the pinned libc.prx" >&2; rm -f "$out/runtime/libc.prx"; exit 2; }
fi
if ! cmp -s "$sibling/build/host/ps5-native-tool" "$out/build/host/ps5-native-tool" 2>/dev/null; then
    [[ -x $sibling/build/host/ps5-native-tool ]] || { echo "no $sibling/build/host/ps5-native-tool" >&2; exit 2; }
    mkdir -p "$out/build/host"
    cp -p "$sibling/build/host/ps5-native-tool" "$out/build/host/ps5-native-tool"
fi

# The recipe's own build: a no-op when the archive's PROVENANCE already names the pinned Mesa and SDK revisions.
PS5_MESA_FORK=$(cd -- "$sibling/../PS5_Mesa" && pwd) bash "$out/tools/build-radv.sh" release >&2
printf '%s\n' "$out"
