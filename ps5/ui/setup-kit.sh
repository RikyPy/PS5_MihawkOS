#!/usr/bin/env bash
# PS5 Vulkan Template - the UI kit this title builds with, at a pinned revision.
#
#   ps5/ui/setup-kit.sh       export it into .deps/hui (once a revision); prints the path
#
# The kit is PS5_VKHomebrewUI, my fork of BlackBearReloaded's ps5-homebrew-ui
# with a Vulkan backend. The pinned revision is exported with git archive from
# the sibling checkout (../PS5_VKHomebrewUI, or PS5_VKHOMEBREWUI), or from
# GitHub when there is none, so a build never depends on a working tree.
# ps5/ui/ui.cmake runs this when the build is configured.
#
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
kit="$root/.deps/hui"
fork="${PS5_VKHOMEBREWUI:-$root/../PS5_VKHomebrewUI}"
# 2188642: the Vulkan backend, with import_texture (a program's own image)
revision=2188642afbcc8d84d36149124f3d9e39dc1b1a05

# What is exported, beside the revision: a change to the list exports again
stamp="$revision src assets/fonts assets/audio third_party/fonts tools/font-baker european"
if [[ ! -f $kit/.revision || $(<"$kit/.revision") != "$stamp" ]]; then
    if ! git -C "$fork" cat-file -e "$revision^{commit}" 2>/dev/null; then
        fork="$root/.deps/PS5_VKHomebrewUI.git"
        [[ -d $fork ]] || git clone --quiet --bare https://github.com/mihawk-99/PS5_VKHomebrewUI.git "$fork" >&2
        git -C "$fork" fetch --quiet origin "$revision" >&2 || true
        git -C "$fork" cat-file -e "$revision^{commit}" 2>/dev/null || {
            echo "PS5_VKHomebrewUI $revision is neither in ../PS5_VKHomebrewUI nor on GitHub" >&2
            exit 2
        }
    fi
    rm -rf -- "$kit"
    mkdir -p "$kit"
    # The code, the assets a title ships (the TTFs are for the samples' ImGui
    # windows, ps5/ui/overlay_theme.cpp), and the licence texts
    git -C "$fork" archive "$revision" src assets/fonts assets/audio third_party/fonts LICENSE \
        THIRD_PARTY_NOTICES.md tools/font-baker third_party/stb/stb_truetype.h | tar -x -C "$kit"
    # The three faces the launcher writes with, baked again with the kit's own baker and its "european" set
    # (accented Latin and Cyrillic): the kit's bakes carry ASCII alone, and the guided setup speaks French
    # (docs/GUIDED_SETUP.md). 2048 is the atlas that set needs at 56 pixels (tools/font-baker/bake_font.cpp).
    "${HOST_CXX:-clang++}" -std=c++20 -O2 -w "$kit/tools/font-baker/bake_font.cpp" -o "$kit/bake_font" >&2
    for face in Inter-Regular:inter-regular Inter-SemiBold:inter-semibold Montserrat-Medium:montserrat-medium; do
        "$kit/bake_font" "$kit/third_party/fonts/${face%%:*}.ttf" "$kit/assets/fonts/${face##*:}.huifont" 56 8 2048 european >&2
    done
    echo "$stamp" > "$kit/.revision"
fi
echo "$kit"
