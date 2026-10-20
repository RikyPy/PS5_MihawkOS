#!/usr/bin/env bash
# PS5_Proton - fetch the pinned runtime sources into .deps/.
#
#   tools/fetch-runtime.sh              fetch what is missing, verify the rest
#   tools/fetch-runtime.sh --check      report only, fetch nothing: exit 0 every pin present,
#                                       2 something to fetch, 1 a checkout is off its pin
#
# The pin is one place: the Proton-GE tag, and the revisions its submodules point
# at, taken from `git ls-tree GE_PROTON_TAG` of the fork. Nothing here patches
# anything: the patch series is applied by tools/build-wine-ps5.sh (milestone 2),
# and tools/check-patches.sh reports whether it still applies.
#
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

GE_REPO=${GE_REPO:-https://github.com/mihawk-99/proton-ge-custom}
GE_TAG=GE-Proton11-7

# name | repository | commit at that tag
pins=(
    "wine-proton|https://github.com/ValveSoftware/wine|1c78596625336a73dbfc035dbbcf0dbdd95b06eb"
    "wine-staging|https://github.com/wine-staging/wine-staging|6cc805ea57132eeaf44764e9213823c9b8d0d300"
    "dxvk|https://github.com/doitsujin/dxvk|601930949d111edbbcf9dd463948426d9f8f6ddd"
    "vkd3d-proton|https://github.com/HansKristian-Work/vkd3d-proton|af89350cc2eacd9da2293fbae96bd9ab4987c9bb"
)

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
deps=$root/.deps
mkdir -p "$deps"
check_only=false
[[ ${1:-} == --check ]] && check_only=true
status=0

fetch() {
    local name=$1 repo=$2 commit=$3 directory=$deps/$1 present=false
    if [[ -d $directory/.git ]] && [[ $(git -C "$directory" rev-parse HEAD 2>/dev/null) == "$commit" ]]; then
        echo "ok        $name $commit"
        return 0
    fi
    if [[ $name == wine-proton && -d $directory/.git ]] &&
            git -C "$directory" merge-base --is-ancestor "$commit" HEAD; then
        echo "preserved $name local rebase on $commit"
        return 0
    fi
    [[ -e $directory ]] && present=true
    # A checkout with no commit is a fetch that was cut off: it holds nothing to keep, so it is fetched again.
    if [[ -d $directory/.git ]] && ! git --git-dir="$directory/.git" rev-parse -q --verify HEAD >/dev/null; then
        present=false
    fi
    if $check_only; then
        if $present; then
            echo "differs   $name $commit ($directory)"; status=1
        else
            echo "missing   $name $commit ($directory)"; [[ $status == 1 ]] || status=2
        fi
        return 0
    fi
    if $present; then
        echo "$directory differs from its pin; preserving existing work" >&2; return 1
    fi
    rm -rf "$directory"
    mkdir -p "$directory"
    git -C "$directory" init -q -b main .
    git -C "$directory" remote add origin "$repo"
    echo "fetching  $name $commit"
    git -C "$directory" fetch -q --depth 1 origin "$commit"
    git -C "$directory" checkout -q FETCH_HEAD
    [[ $(git -C "$directory" rev-parse HEAD) == "$commit" ]] || {
        echo "$name: fetched the wrong revision" >&2; exit 1; }
    echo "ok        $name $commit"
}

echo "pin: $GE_REPO $GE_TAG"
for pin in "${pins[@]}"; do
    IFS='|' read -r name repo commit <<<"$pin"
    fetch "$name" "$repo" "$commit"
done
$check_only && exit "$status"
printf '%s\n' "${pins[@]}" > "$deps/runtime-pins.txt"
echo "pins recorded in .deps/runtime-pins.txt"
