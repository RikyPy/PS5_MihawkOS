#!/usr/bin/env bash
# PS5_Proton - IA-32 bytes as decoded instructions (docs/IA32_DESIGN.md).
#
#   tools/ia32-dump.sh [--64] [--address A] [--offset N] [--count N] FILE
#   tools/ia32-dump.sh [--64] [--address A] --hex "8b 44 88 10"
#
# Builds tools/ia32_dump.c with the repository's decoder and printer (host compiler) and runs it.
#
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=$root/.deps/ia32
mkdir -p "$out"
sources=("$root/tools/ia32_dump.c" "$root/src/pw_ia32_isa.c" "$root/src/pw_ia32_decode.c" "$root/src/pw_ia32_print.c")
if [[ ! -x $out/ia32_dump ]] || [[ -n $(find "${sources[@]}" "$root/src/pw_ia32_isa.def" -newer "$out/ia32_dump") ]]; then
    "${CC:-cc}" -std=gnu11 -O2 -g -Wall -Wextra -Werror -o "$out/ia32_dump" "${sources[@]}"
fi
exec "$out/ia32_dump" "$@"
