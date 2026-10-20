#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
version=1.0.0
digest=f93643c04c843d56143b00951df1f8042ea7706ae9f19e4e9abf158e1ead77c5
archive="$root/.deps/ps5-opengl-sdk-$version.tar.gz"
prefix="$root/.deps/ps5-opengl-sdk-$version"
mkdir -p "$root/.deps"
if [[ ! -f $archive ]]; then
    curl -fL --retry 3 -o "$archive.part" \
        "https://github.com/blackbearreloaded/ps5-opengl/releases/download/v$version/ps5-opengl-sdk-$version.tar.gz"
    printf '%s  %s\n' "$digest" "$archive.part" | sha256sum -c -
    mv "$archive.part" "$archive"
fi
printf '%s  %s\n' "$digest" "$archive" | sha256sum -c -
# Unpacked beside its folder and renamed when complete, so a cut-off unpack is never taken for a finished one.
if [[ ! -d $prefix ]]; then
    rm -rf "$prefix.partial"
    mkdir -p "$prefix.partial"
    tar -xzf "$archive" -C "$prefix.partial" --strip-components=1
    mv "$prefix.partial" "$prefix"
fi
(cd "$prefix/sdk" && sha256sum --status -c manifest.sha256) || {
    echo "$prefix differs from its manifest: delete it to unpack it again" >&2; exit 1; }
printf 'Verified OpenGL SDK: %s/sdk\n' "$prefix"
