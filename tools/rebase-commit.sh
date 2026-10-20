#!/usr/bin/env bash
# PS5_Proton - commit the tree as the rebased form of one patch, and write it back.
#
#   tools/rebase-commit.sh <patch-file>
#
# Keeps the patch's authorship, date, subject and body, adds the rebase note, and
# regenerates the patch file in place (same name, so the series order is unchanged).
# The Wine tree is .deps/wine-proton unless WINE_TREE says otherwise.
#
# Two guards, because a partial commit is invisible in the diff: it refuses while a
# .rej file is on disk, and it refuses a regenerated patch that lost hunks.
#
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
wine=${WINE_TREE:-$root/.deps/wine-proton}
patch=$1
[[ -f $patch ]] || { echo "no such patch: $patch" >&2; exit 2; }
[[ -d $wine/dlls ]] || { echo "no Wine tree at $wine" >&2; exit 2; }

if find "$wine" -name '*.rej' | grep -q .; then
    echo "$(basename "$patch"): unresolved hunks:" >&2
    find "$wine" -name '*.rej' | sed "s|$wine/|  |" >&2
    echo "place them and remove the .rej files, then commit" >&2
    exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git mailinfo "$tmp/message" "$tmp/diff" < "$patch" > "$tmp/headers"
author="$(sed -n 's/^Author: //p' "$tmp/headers") <$(sed -n 's/^Email: //p' "$tmp/headers")>"
date=$(sed -n 's/^Date: //p' "$tmp/headers")
subject=$(sed -n 's/^Subject: //p' "$tmp/headers")
body=$(cat "$tmp/message")
hunks_before=$(grep -c '^@@' "$patch" || true)

[[ -n $subject ]] || { echo "$patch: no subject" >&2; exit 2; }
git -C "$wine" add -A
if git -C "$wine" diff --cached --quiet; then
    echo "$(basename "$patch"): nothing to commit" >&2
    exit 1
fi
hunks_after=$(git -C "$wine" diff --cached --no-ext-diff --no-textconv -U3 | grep -c '^@@' || true)
if [[ $hunks_after -lt $hunks_before ]]; then
    if [[ -n ${REBASE_HUNK_REDUCTION_REASON:-} ]]; then
        body+=$'\n\n'"Hunk reduction ($hunks_before -> $hunks_after): $REBASE_HUNK_REDUCTION_REASON"
    else
        echo "$(basename "$patch"): the resolution has $hunks_after hunks, the original had $hunks_before" >&2
        echo "review the resolution; neither HEAD nor the original patch file was changed" >&2
        exit 1
    fi
fi
git -C "$wine" commit -q --author "$author" ${date:+--date="$date"} \
    -m "$subject" -m "$body

Rebased onto $(git -C "$wine" rev-parse --short=12 "$(cat "$root/.deps/wine-proton.pin")") for PS5_Proton (see wine/patches/README.md)."
git -C "$wine" format-patch -1 --stdout --no-ext-diff --no-textconv -U3 > "$patch.new"
mv "$patch.new" "$patch"
echo "rebased $(basename "$patch"): $(git -C "$wine" log -1 --format='%h %an %s') (hunks $hunks_before -> $hunks_after)"
