#!/usr/bin/env bash
# PS5_Proton - src/pw_shell_link.c's .lnk bytes against Wine's own reader.
#
#   tools/check-shell-link.sh
#
# Builds tests/shell-link-oracle.c, an x86_64 Windows console program built the way tools/build-wine-probe.sh
# builds probes, into .deps/probes/, and tests/test_pw_shell_link.c for the host the way tools/host-tests.sh does.
# The test binary's --samples mode writes sample .lnk files with pw_shell_link_build and, beside each, the fields
# Wine must read from it, written out by hand in the test. The oracle loads each sample through Wine's
# IShellLinkW and IPersistFile, as Explorer does, under the host's Wine (HOST_WINE, default /usr/bin/wine) in a
# throwaway prefix, .deps/shell-link/prefix (never ~/.wine; delete it to start over). Every field is compared:
# one PASS or FAIL line each. Then the "open" sample is opened as Explorer's double click does (the oracle's
# --open: ShellExecuteW on the .lnk); its target, cmd.exe, must write the marker file within 20 seconds.
# Exit status: 0 PASS, or skipped when there is no host Wine; 1 FAIL.
#
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
host_wine=${HOST_WINE:-/usr/bin/wine}
if [[ ! -x $host_wine ]]; then
    echo "check-shell-link: skipped: no wine ($host_wine)"
    exit 0
fi
wine=$root/.deps/wine-runtime
work=$root/.deps/shell-link
samples=$work/samples
oracle=$root/.deps/probes/shell-link-oracle.exe
arch=x86_64-windows
mkdir -p "$root/.deps/probes" "$samples"

clang --target=x86_64-windows --no-default-config -fuse-ld=lld -nostdlib -O2 -Wall -Wextra -Werror \
    -I"$wine/source/include" -I"$wine/build/include" -I"$wine/source/include/msvcrt" \
    -Wl,/entry:mainCRTStartup -Wl,/subsystem:console \
    "$root/tests/shell-link-oracle.c" "$wine/build/dlls/kernel32/$arch/libkernel32.a" \
    "$wine/build/dlls/ole32/$arch/libole32.a" "$wine/build/dlls/shell32/$arch/libshell32.a" \
    "$wine/build/dlls/msvcrt/$arch/libmsvcrt.a" -o "$oracle"
"${CC:-cc}" -std=gnu11 -O2 -g -Wall -Wextra -Werror -fsanitize=address,undefined -o "$work/test_pw_shell_link" \
    "$root/tests/test_pw_shell_link.c" "$root/src/pw_shell_link.c"
rm -f "$samples"/*.lnk "$samples"/*.expected "$work"/*.actual "$work"/*.stderr "$work/opened.txt"
"$work/test_pw_shell_link" --samples "$samples"

export WINEPREFIX=$work/prefix WINEARCH=win64 WINEDEBUG=-all
export WINEDLLOVERRIDES='winemenubuilder.exe=d;mscoree=d;mshtml=d'
[[ -d $WINEPREFIX ]] || echo "check-shell-link: making the throwaway prefix $WINEPREFIX (about a minute, once)"

# A long value is shown by its start and its length.
show() {
    if (( ${#1} > 80 )); then printf '%s... (%d characters)' "${1:0:60}" "${#1}"; else printf '%s' "$1"; fi
}
passes=0 failures=0
for expected in "$samples"/*.expected; do
    sample=$(basename "$expected" .expected)
    lnk=$samples/$sample.lnk
    if ! "$host_wine" "$oracle" "Z:${lnk//\//\\}" > "$work/$sample.actual" 2> "$work/$sample.stderr"; then
        reason=$(grep shell-link-oracle "$work/$sample.stderr" || tail -n 3 "$work/$sample.stderr")
        echo "FAIL $sample: the oracle failed: $reason"
        failures=$((failures + 1))
        continue
    fi
    declare -A got=()
    while IFS= read -r line; do got[${line%%=*}]=${line#*=}; done < "$work/$sample.actual"
    fields=0
    while IFS= read -r line; do
        field=${line%%=*} want=${line#*=}
        fields=$((fields + 1))
        if [[ -v got[$field] && ${got[$field]} == "$want" ]]; then
            echo "PASS $sample $field: $(show "$want")"
            passes=$((passes + 1))
        else
            echo "FAIL $sample $field: expected '$(show "$want")', Wine read '$(show "${got[$field]-<no line>}")'"
            failures=$((failures + 1))
        fi
    done < "$expected"
    if (( ${#got[@]} != fields )); then
        echo "FAIL $sample: Wine printed ${#got[@]} fields, $fields expected"
        failures=$((failures + 1))
    fi
    unset got
done

# The double click: no LinkTargetIDList is written, so this is shell32's lnkfile handler running the LinkInfo path.
marker=$work/opened.txt
export PW_SHELL_LINK_MARKER="Z:${marker//\//\\}"
if "$host_wine" "$oracle" --open "Z:${samples//\//\\}\\open.lnk" > /dev/null 2> "$work/open.stderr"; then
    for _ in $(seq 100); do [[ -s $marker ]] && break; sleep 0.2; done
    if grep -q opened "$marker" 2> /dev/null; then
        echo "PASS open: ShellExecuteW on open.lnk ran its target with its arguments"
        passes=$((passes + 1))
    else
        echo "FAIL open: ShellExecuteW on open.lnk returned, and its target wrote no marker in 20 seconds"
        failures=$((failures + 1))
    fi
else
    echo "FAIL open: $(grep shell-link-oracle "$work/open.stderr" || tail -n 3 "$work/open.stderr")"
    failures=$((failures + 1))
fi
"${host_wine%/*}/wineserver" -k 2> /dev/null || true
version=$("$host_wine" --version 2> /dev/null || echo "unknown wine")
if (( failures )); then
    echo "check-shell-link: FAIL ($failures failed, $passes passed; host $version)"
    exit 1
fi
echo "check-shell-link: PASS ($passes checks; host $version)"
