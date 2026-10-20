#!/usr/bin/env bash
# PS5_Proton - the real CPU in 32-bit mode as an oracle (docs/IA32_DESIGN.md, M2).
#
#   echo "66 f3 0f 84 00 00" | tools/ia32-cpu-run.sh
#
# Builds tools/ia32_cpu_run.c with -m32 (the host compiler's 32-bit target) and runs it: one hex
# encoding per line in, one JSON line per encoding out (what the CPU did: its length, or the fault).
#
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=$root/.deps/ia32
mkdir -p "$out"
if [[ ! -x $out/ia32_cpu_run ]] || [[ $root/tools/ia32_cpu_run.c -nt $out/ia32_cpu_run ]] || [[ $root/tools/ia32_cpu_core.h -nt $out/ia32_cpu_run ]]; then
    "${CC:-cc}" -m32 -std=gnu11 -O1 -g -Wall -Wextra -Werror -fno-pie -no-pie -o "$out/ia32_cpu_run" "$root/tools/ia32_cpu_run.c"
fi
exec "$out/ia32_cpu_run"
