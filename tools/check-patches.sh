#!/usr/bin/env bash
# Replay the series in order from the pin, without changing the working tree.
# Usage: tools/check-patches.sh [--write] [wine-tree]
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
write=false
if [[ ${1:-} == --write ]]; then write=true; shift; fi
wine=${1:-$root/.deps/wine-proton}
pin=$(cat "$root/.deps/wine-proton.pin")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/tree"
git -C "$wine" archive "$pin" | tar -x -C "$tmp/tree"
# A temporary repository bounds git apply to this tree, even inside another repo.
git -C "$tmp/tree" init -q
clean=0; failed=0; pending=0
report="$tmp/report"
{
    echo '# Patch rebase status'
    echo
    echo "Sequential replay from Wine \`$pin\`, measured by \`tools/check-patches.sh\`."
    echo 'The check uses a temporary tree and never resets or cleans the source checkout.'
    echo 'After the first conflict, later patches are untested because their prerequisites are missing.'
    echo
    echo '| Patch | Sequential apply |'
    echo '| --- | --- |'
} > "$report"
for patch in "$root"/wine/patches/*.patch; do
    name=$(basename "$patch")
    if (( failed )); then
        state=untested; pending=$((pending + 1))
    elif git -C "$tmp/tree" apply --whitespace=nowarn "$patch" 2> "$tmp/error"; then
        state=clean; clean=$((clean + 1))
    else
        state=conflict; failed=1
        cat "$tmp/error" >&2
    fi
    printf '%-10s %s\n' "$state" "$name"
    printf '| `%s` | %s |\n' "$name" "$state" >> "$report"
done
printf '\n%d apply in order, %d conflict, %d untested\n' "$clean" "$failed" "$pending" | tee -a "$report"
if $write; then cp "$report" "$root/docs/PATCH_REBASE.md"; fi
exit "$failed"
