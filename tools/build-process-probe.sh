#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
wine=$root/.deps/wine-runtime
output=${1:-$root/.deps/probes/process-probe.exe}
mkdir -p "$(dirname -- "$output")"
clang --target=x86_64-windows --no-default-config -fuse-ld=lld -nostdlib -O2 -ffreestanding -fno-builtin -Wall -Wextra -Werror \
    -I"$wine/source/include" -I"$wine/build/include" -I"$wine/source/include/msvcrt" \
    -Wl,/entry:mainCRTStartup -Wl,/subsystem:console \
    "$root/tests/process-probe.c" "$wine/build/dlls/kernel32/x86_64-windows/libkernel32.a" \
    "$wine/build/dlls/msvcrt/x86_64-windows/libmsvcrt.a" \
    -o "$output"
