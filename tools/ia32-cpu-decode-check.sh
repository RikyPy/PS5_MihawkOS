#!/usr/bin/env bash
# PS5_Proton - the IA-32 decoder against the real CPU in 32-bit mode (docs/IA32_DESIGN.md, M2).
#
#   tools/ia32-cpu-decode-check.sh
#
# Builds tools/ia32_cpu_decode_check.c with -m32 and the decoder, runs the corpus single-stepped
# on this PC's CPU, prints one JSON summary and writes every group to
# .deps/ia32/cpu-decode-groups.jsonl. The CPU's vendor matters for vendor-specific encodings
# (docs/IA32_DESIGN.md, "Which CPU decides").
#
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=$root/.deps/ia32
mkdir -p "$out"
"${CC:-cc}" -m32 -std=gnu11 -O2 -g -Wall -Wextra -Werror -fno-pie -no-pie -o "$out/ia32_cpu_decode_check" \
    "$root/tools/ia32_cpu_decode_check.c" "$root/src/pw_ia32_isa.c" "$root/src/pw_ia32_decode.c"
"$out/ia32_cpu_decode_check" "$out/cpu-decode-groups.jsonl"
