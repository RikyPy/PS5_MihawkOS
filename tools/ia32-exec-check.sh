#!/usr/bin/env bash
# PS5_Proton - the IA-32 interpreter against the real CPU in 32-bit mode (docs/IA32_DESIGN.md, M2).
#
#   tools/ia32-exec-check.sh
#
# Builds tools/ia32_exec_check.c with -m32, the decoder and the interpreter, runs every form from
# random states natively and through the interpreter, prints one JSON summary and writes each form
# with a mismatch to .deps/ia32/exec-mismatches.jsonl.
#
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=$root/.deps/ia32
mkdir -p "$out"
"${CC:-cc}" -m32 -std=gnu11 -O2 -g -Wall -Wextra -Werror -fno-pie -no-pie -o "$out/ia32_exec_check" \
    "$root/tools/ia32_exec_check.c" "$root/src/pw_ia32_isa.c" "$root/src/pw_ia32_decode.c" "$root/src/pw_ia32_interp.c"
"$out/ia32_exec_check" "$out/exec-mismatches.jsonl"
