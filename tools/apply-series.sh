#!/usr/bin/env bash
# PS5_Proton - apply the PS5 patch series to the pinned Wine tree, in order.
#
#   tools/apply-series.sh [wine-tree]        apply what applies, report the rest
#   tools/apply-series.sh --reset [tree]     put the tree back to the pin
#
# The series is applied the way the build applies it: in file-name order, each
# patch on top of the ones before it, with a three-way merge so Proton's changes
# and ours meet where they can. A patch that does not apply is left out and
# named, so the tree stays usable while the rebase is in progress.
#
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
wine=$root/.deps/wine-proton
if [[ ${1:-} == --reset ]]; then
    [[ -n ${2:-} ]] && wine=$2
    git -C "$wine" reset -q --hard HEAD
    git -C "$wine" clean -qfd
    echo "reset $wine to $(git -C "$wine" rev-parse --short HEAD)"
    exit 0
fi
[[ -n ${1:-} ]] && wine=$1
[[ -d $wine/dlls/ntdll ]] || { echo "no Wine tree at $wine" >&2; exit 2; }
# Always from the pin: a series applied on top of a half-applied tree measures
# nothing (and "everything conflicts" is the symptom of exactly that).
# reset --hard, not checkout: a three-way apply writes its merge into the index,
# so restoring from the index would keep conflict markers and fail every patch.
git -C "$wine" reset -q --hard HEAD
git -C "$wine" clean -qfd

applied=0; failed=0; list=""
for patch in "$root"/wine/patches/*.patch; do
    name=$(basename "$patch")
    if git -C "$wine" apply --3way --whitespace=nowarn "$patch" >/dev/null 2>&1; then
        applied=$((applied + 1))
        printf 'applied   %s\n' "$name"
    else
        git -C "$wine" reset -q --hard HEAD 2>/dev/null || true
        git -C "$wine" clean -qfd 2>/dev/null || true
        failed=$((failed + 1)); list+=" $name"
        printf 'REBASE    %s\n' "$name"
    fi
done
printf '\n%d applied, %d need a rebase\n' "$applied" "$failed"
[[ -n $list ]] && echo "queue:$list" >&2
exit 0
