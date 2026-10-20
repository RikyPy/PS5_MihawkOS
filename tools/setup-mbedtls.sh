#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The title's own TLS (docs/SELF_INSTALL.md, phase 6): an elevated title's sceSsl rejects every public certificate
# (0x8095f00c, ps5-native-app-boilerplate/docs/CURL.md), and the title must be elevated to write /data. Mbed TLS
# 3.6.7, the release ../PS5_RetroArch/tools/build-curl.sh pins, unpacked once into .deps/mbedtls-3.6.7; the title's
# CMake and the host tests compile its library/ with platform/tls/pw_mbedtls_config.h. Prints the source folder.
#
#   tools/setup-mbedtls.sh
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=3.6.7
digest=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
out=$root/.deps/mbedtls-$version
if [[ ! -f $out/.unpacked ]]; then
    # A checkout's own download, or one this machine already holds by the same hash (no fetch then).
    archive=${MIHAWKOS_DOWNLOADS:-$root/.deps/downloads}/mbedtls-$version.tar.bz2
    if [[ ! -f $archive ]]; then
        mkdir -p "$(dirname "$archive")"
        curl -sSfL -o "$archive.part" \
            "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$version/mbedtls-$version.tar.bz2"
        mv "$archive.part" "$archive"
    fi
    printf '%s  %s\n' "$digest" "$archive" | sha256sum --check --status ||
        { echo "$archive does not match its pinned SHA-256" >&2; exit 1; }
    staging=$(mktemp -d "$root/.deps/mbedtls.XXXXXX")
    tar -xjf "$archive" -C "$staging" --strip-components=1
    touch "$staging/.unpacked"
    rm -rf "$out"
    mv "$staging" "$out"
fi
printf '%s\n' "$out"
