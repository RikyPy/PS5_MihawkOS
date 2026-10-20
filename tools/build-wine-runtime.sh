#!/usr/bin/env bash
# Build the PE runtime (x86_64, and i386 for WoW64: docs/IA32_DESIGN.md, M3) and host tools from the rebased
# Proton Wine tree.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
wine=$root/.deps/wine-proton
work=$root/.deps/wine-runtime
revision=$(git -C "$wine" rev-parse HEAD)
configure_args=(--enable-win64 --enable-archs=i386,x86_64 --with-mingw=clang --disable-tests
    --without-x --without-wayland --without-gstreamer --without-ffmpeg --without-alsa --without-pulse)
stamp=$(printf '%s\n' "$revision" "${configure_args[@]}" | sha256sum | cut -c1-64)
[[ -z $(git -C "$wine" status --porcelain) ]] || { echo 'Wine rebase is not clean' >&2; exit 1; }
# failed LOG: a step's output goes to its log; when the step fails, show the log's end and stop.
failed() { tail -n 20 "$1" >&2; echo "tools/build-wine-runtime.sh failed: the whole log is $1" >&2; exit 1; }
mkdir -p "$work/source" "$work/build"
if [[ $(cat "$work/stamp" 2>/dev/null) != "$stamp" ]]; then
    git -C "$wine" archive HEAD | tar -x -C "$work/source"
    bash "$root/tools/generate-wine.sh" "$work/source" > "$work/generate.log" 2>&1 || failed "$work/generate.log"
    (cd "$work/build" && ../source/configure "${configure_args[@]}" > "$work/configure.log" 2>&1) ||
        failed "$work/configure.log"
    echo "$revision" > "$work/revision"
    echo "$stamp" > "$work/stamp"
fi
make -C "$work/build" -j"${JOBS:-12}" > "$work/make.log" 2>&1 || failed "$work/make.log"
# Installed beside the stage and renamed when complete: tools/fetch-all.sh takes stage/ as this build being done.
rm -rf "$work/stage.partial"
make -C "$work/build" install DESTDIR="$work/stage.partial" > "$work/install.log" 2>&1 || failed "$work/install.log"
rm -rf "$work/stage"
mv "$work/stage.partial" "$work/stage"
echo "Wine runtime built and staged in $work/stage"
