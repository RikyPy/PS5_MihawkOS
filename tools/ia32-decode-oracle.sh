#!/usr/bin/env bash
# PS5_Proton - the x86 decoder against LLVM's disassembler (docs/IA32_DESIGN.md, M1; docs/MMU4K.md).
#
#   tools/ia32-decode-oracle.sh          32-bit mode
#   tools/ia32-decode-oracle.sh --64     long mode, with the table's memory, stack and control-flow
#                                        attributes checked against LLVM's MC layer
#
# Builds tools/ia32_decode_oracle.c (and, for --64, tools/x86_llvm_semantics.cpp) with the decoder
# and printer against the host's libLLVM (llvm-config), runs it over the generated corpus and prints
# one JSON object: the verdict, the counts and the LLVM version. Every disagreement group is in
# .deps/ia32/decode-groups.jsonl (decode-groups-64.jsonl); the ones tests/ia32-llvm-differences.txt
# (x64-llvm-differences.txt) explains do not fail the run. Exit status: 0 PASS, 1 FAIL.
#
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out=$root/.deps/ia32
mkdir -p "$out"
command -v llvm-config > /dev/null || { echo "ia32-decode-oracle: llvm-config not found" >&2; exit 2; }
objects=()
for source in tools/ia32_decode_oracle.c src/pw_ia32_isa.c src/pw_ia32_decode.c src/pw_ia32_print.c; do
    object=$out/$(basename "$source" .c).o
    "${CC:-cc}" -std=gnu11 -O2 -g -Wall -Wextra -Werror -I"$(llvm-config --includedir)" -c -o "$object" "$root/$source"
    objects+=("$object")
done
# shellcheck disable=SC2046
"${CXX:-c++}" $(llvm-config --cxxflags) -O2 -g -Wall -Wextra -Werror -c \
    -o "$out/x86_llvm_semantics.o" "$root/tools/x86_llvm_semantics.cpp"
# shellcheck disable=SC2046
"${CXX:-c++}" -o "$out/ia32_decode_oracle" "${objects[@]}" "$out/x86_llvm_semantics.o" \
    -L"$(llvm-config --libdir)" $(llvm-config --libs x86disassembler x86desc x86info mc support)
if [[ ${1:-} == --64 ]]; then
    "$out/ia32_decode_oracle" --64 "$root/tests/x64-llvm-differences.txt" "$out/decode-groups-64.jsonl"
else
    "$out/ia32_decode_oracle" "$root/tests/ia32-llvm-differences.txt" "$out/decode-groups.jsonl"
fi
