#!/usr/bin/env bash
# Build VKD3D-Proton's own tests (tests/d3d12.exe) for Windows, at the revision GE-Proton11-7 ships.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# VKD3D-Proton is built with MinGW; the host has none, so a pinned llvm-mingw (clang) is unpacked user-locally into .deps
# (nothing is installed system-wide) and meson cross-compiles with it. The result is .deps/wine-tests/exe/vkd3d_d3d12.exe,
# which tools/run-vkd3d-tests.py runs.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
deps=$root/.deps
vkd3d_commit=af89350cc2eacd9da2293fbae96bd9ab4987c9bb       # vkd3d-1.1-5604-gaf89350cc, as in GE-Proton11-7
mingw_tag=20260922
mingw_name=llvm-mingw-$mingw_tag-ucrt-ubuntu-22.04-x86_64
mingw_sha256=bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21

if [[ ! -x $deps/llvm-mingw/bin/x86_64-w64-mingw32-clang ]]; then
    curl -fsSL -o "$deps/llvm-mingw.tar.xz" "https://github.com/mstorsjo/llvm-mingw/releases/download/$mingw_tag/$mingw_name.tar.xz"
    echo "$mingw_sha256  $deps/llvm-mingw.tar.xz" | sha256sum --check --status || { echo "llvm-mingw checksum mismatch" >&2; exit 1; }
    mkdir -p "$deps/llvm-mingw"
    tar -xJf "$deps/llvm-mingw.tar.xz" -C "$deps/llvm-mingw" --strip-components=1
fi
if [[ ! -d $deps/vkd3d-proton/.git ]]; then
    git clone --filter=blob:none https://github.com/HansKristian-Work/vkd3d-proton.git "$deps/vkd3d-proton"
fi
git -C "$deps/vkd3d-proton" checkout -q "$vkd3d_commit"
git -C "$deps/vkd3d-proton" submodule update --init --recursive --depth 1
# VKD3D_TEST_RANGE=from-to: a slice of the suite per console launch (tests/vkd3d-test-range.patch)
if git -C "$deps/vkd3d-proton" apply --check "$root/tests/vkd3d-test-range.patch" 2>/dev/null; then
    git -C "$deps/vkd3d-proton" apply "$root/tests/vkd3d-test-range.patch"
fi
m=$deps/llvm-mingw/bin
cat > "$deps/vkd3d-cross-llvm.txt" <<CROSS
[binaries]
c = '$m/x86_64-w64-mingw32-clang'
cpp = '$m/x86_64-w64-mingw32-clang++'
ar = '$m/llvm-ar'
strip = '$m/llvm-strip'
windres = '$m/llvm-windres'
widl-mingw-tools-fallback = '$m/x86_64-w64-mingw32-widl'

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
CROSS
export PATH=$m:$PATH
[[ -f $deps/vkd3d-build/build.ninja ]] ||
    (cd "$deps/vkd3d-proton" && meson setup --cross-file "$deps/vkd3d-cross-llvm.txt" --buildtype release -Denable_tests=true --strip "$deps/vkd3d-build")
ninja -C "$deps/vkd3d-build"
mkdir -p "$deps/wine-tests/exe"
cp "$deps/vkd3d-build/tests/d3d12.exe" "$deps/wine-tests/exe/vkd3d_d3d12.exe"
ls -la "$deps/wine-tests/exe/vkd3d_d3d12.exe"
