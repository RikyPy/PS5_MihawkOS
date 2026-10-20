#!/usr/bin/env bash
# PS5_Proton - the rebase loop, one patch at a time.
#
#   tools/rebase-next.sh            apply the next patch of the series and commit it
#   tools/rebase-next.sh --commit   commit the resolution of a patch that was rejected
#   tools/rebase-next.sh --status   how far the rebase has got
#
# State is the branch: the number of commits on ps5-rebase above the pin is the
# number of patches rebased, and the next patch is that many files into
# wine/patches/*.patch (name order, the order the build applies them). One patch
# per commit, always: a commit that carries two patches rewrites both files.
#
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
wine=$root/.deps/wine-proton
pin=$root/.deps/wine-proton.pin
[[ -f $pin ]] || git -C "$wine" rev-parse HEAD > "$pin"
mapfile -t patches < <(ls "$root"/wine/patches/*.patch)
done_count=$(git -C "$wine" rev-list --count "$(cat "$pin")"..HEAD)

case ${1:-} in
--status)
    printf '%d of %d patches rebased (%d to go)\n' "$done_count" "${#patches[@]}" "$(( ${#patches[@]} - done_count ))"
    [[ $done_count -lt ${#patches[@]} ]] && echo "next: $(basename "${patches[$done_count]}")"
    exit 0 ;;
--commit)
    patch=${patches[$done_count]}
    "$root/tools/rebase-commit.sh" "$patch"
    exit 0 ;;
"") ;;
*) echo "usage: rebase-next.sh [--commit|--status]" >&2; exit 2 ;;
esac

[[ $done_count -lt ${#patches[@]} ]] || { echo "all ${#patches[@]} patches rebased"; exit 0; }
patch=${patches[$done_count]}
[[ -z $(git -C "$wine" status --porcelain) ]] || {
    echo "Wine tree has unfinished changes; resolve them and use --commit before applying another patch" >&2
    exit 1
}
if git -C "$wine" apply --whitespace=nowarn "$patch" 2>/dev/null; then
    "$root/tools/rebase-commit.sh" "$patch"
else
    git -C "$wine" apply --reject --whitespace=nowarn "$patch" >/dev/null 2>&1 || true
    echo "REJECTS  $(basename "$patch")"
    find "$wine" -name '*.rej' | sed "s|$wine/|  |"
    echo "resolve them in the tree, then: tools/rebase-next.sh --commit"
    exit 1
fi
