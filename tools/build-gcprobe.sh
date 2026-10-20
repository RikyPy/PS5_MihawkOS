#!/usr/bin/env bash
# Build the graphics-device probe as a payload ELF.
#
#   tools/build-gcprobe.sh [output]
#
# A plain payload: the SDK's prospero-clang, no platform layer and no title
# packaging. Sent to the console with websrv's payload launcher (POST /elfldr,
# pipe=1), which streams the probe's stdout back to the sender.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk=${PS5_PAYLOAD_SDK:-$root/.deps/native/ps5-payload-sdk}
output=${1:-$root/build/gcprobe.elf}
source_file=$root/tools/gcprobe.c

[ -x "$sdk/bin/prospero-clang" ] || {
    echo "no payload SDK at $sdk (set PS5_PAYLOAD_SDK)" >&2
    exit 1
}

mkdir -p "$(dirname -- "$output")"
"$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -g -o "$output" "$source_file"
echo "built $output"
sha256sum "$output"
