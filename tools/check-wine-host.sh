#!/usr/bin/env bash
# Compile the rebased Unix side on Linux; this is not a PS5 runtime build.
# Usage: tools/check-wine-host.sh [wine-tree]
# Inputs: committed Wine tree, system C toolchain, Vulkan registry 1.4.357.
# Nothing is downloaded; VULKAN_REGISTRY may name a local registry directory.
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
wine=${1:-$root/.deps/wine-proton}
registry=${VULKAN_REGISTRY:-/usr/share/vulkan/registry}
pin=$(cat "$root/.deps/wine-proton.pin")
git -C "$wine" merge-base --is-ancestor "$pin" HEAD
[[ -z $(git -C "$wine" status --porcelain) ]] || {
    echo 'Commit the Wine resolution before checking its build.' >&2; exit 1;
}
revision=$(git -C "$wine" rev-parse HEAD)
work=$root/.deps/wine-host-check-$revision
source=$work/source
build=$work/build
mkdir -p "$source" "$build"
echo "Host build of $revision; logs: $work"
(
    cd "$registry"
    sha256sum --check <<'HASHES'
264d0d7350e37d70c82407fb430d085040fc01a9a961d43dec8c2d6ed1dfd183  vk.xml
d018b914014c06605e367a3b929670511e6f6de2f225c405a8b5e2d912408b76  video.xml
HASHES
)
if [[ ! -f $work/configured ]]; then
    git -C "$wine" archive "$revision" | tar -x -C "$source"
    cp "$registry/vk.xml" "$registry/video.xml" "$source/"
    (cd "$source" && ./autogen.sh) > "$work/autogen.log" 2>&1
    (cd "$build" && ../source/configure --enable-win64 --without-mingw --disable-tests \
        --without-x --without-wayland CFLAGS='-O2 -g -DWINE_INPROCESS_SERVER -DWINE_PS5_USER_DRIVER') \
        > "$work/configure.log" 2>&1
    touch "$work/configured"
fi
make -C "$build" -j"${JOBS:-8}" dlls/ntdll/ntdll.so dlls/win32u/win32u.so server/wineserver \
    > "$work/make.log" 2>&1
printf 'PASS: ntdll.so, win32u.so and wineserver (host only; no PS5 run)\n'
