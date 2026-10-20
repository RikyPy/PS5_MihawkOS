#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
tree=$root/.deps/prx-foundation
pin=30597512539e7edfde079cbcaf4a626bc0a948c5
# A checkout with no commit is a fetch that was cut off: it holds nothing to keep, so it is fetched again.
if [[ -d $tree/.git ]] && ! git --git-dir="$tree/.git" rev-parse -q --verify HEAD >/dev/null; then
    rm -rf "$tree"
fi
if [[ ! -d $tree/.git ]]; then
    mkdir -p "$tree"
    git -C "$tree" init -q
    git -C "$tree" remote add origin https://github.com/mpereiraesaa/ps5-native-app-boilerplate.git
    git -C "$tree" fetch --depth=1 origin "$pin"
    git -C "$tree" checkout --detach FETCH_HEAD
fi
[[ $(git -C "$tree" rev-parse HEAD) == "$pin" && -z $(git -C "$tree" status --porcelain) ]] || {
    echo 'PRX tooling checkout differs from its pin; preserving it' >&2; exit 1;
}
mkdir -p "$tree/build/host"
clang++ -std=c++20 -O2 -Wall -Wextra -Werror \
    "$tree/tooling/native/"{native_app_builder,self_container,elf_object,sce_module_writer}.cpp \
    -lz -o "$tree/build/host/ps5-native-tool"
