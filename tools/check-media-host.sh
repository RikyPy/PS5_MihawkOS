#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Media Foundation playback on the host's pinned Wine, the oracle before any console run: tests/mf-video-probe.c
# (32- and 64-bit) in a fresh prefix of the pinned Wine built for Linux (.deps/wine-runtime, never written), with this
# tree's media modules over it exactly as the console gets them: the patched PE modules tools/build-wine-ps5.sh built
# (<work>/pe), its wine.inf (which registers winedmo), and winedmo's Unix side compiled here from the same patched
# sources against the same pinned FFmpeg configuration built natively (tools/build-ffmpeg-ps5.py --host). The 32-bit
# probe runs under WoW64 through winedmo.so's wow64 table, as a 32-bit game does on the console.
#
# A second clip covers what the generated one cannot (FFmpeg encodes no VC-1): a public WVC1-in-ASF sample from FFmpeg's
# test samples, pinned by SHA-256 and fetched here, for this host check only (it is neither committed nor staged: its
# licence is not recorded). A Direct3D 12 game's intro is VC-1 in ASF, and a decoder that never got a frame of it played
# nothing (2026-10-17).
#
#   tools/check-media-host.sh [--work DIR]     PASS/FAIL per architecture, exit 1 on any failure
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work=$root/.deps/media-host-check
[ "${1:-}" = --work ] && work=$2
host=$root/.deps/wine-runtime
ps5=$root/.deps/wine-ps5
tree=$ps5/source
ff=$root/.deps/ffmpeg-host
VC1_URL=https://samples.ffmpeg.org/V-codecs/WVC1/Test_1440x576_WVC1_6Mbps.wmv
VC1_SHA256=4c8b9575cfbaf4099a0c8c667e7d4f3174ff6c07219960e360dfaafe21d4222d
vc1=$root/.deps/probes/vc1-sample/mf-vc1.wmv
fail() { echo "check-media-host: $*" >&2; exit 1; }
[ -x "$host/build/loader/wine" ] || fail "no host Wine build at $host/build (tools/build-wine-runtime.sh)"
[ -f "$ps5/pe/wine.inf" ] || fail "no patched PE modules in $ps5/pe (tools/build-wine-ps5.sh)"
for name in mf-video32-probe.exe mf-video64-probe.exe mf-video.mp4 mf-video.txt; do
    [ -f "$root/.deps/probes/$name" ] || fail "no .deps/probes/$name (tools/build-wine-probe.sh)"
done
python3 "$root/tools/build-ffmpeg-ps5.py" --host --work "$ff" > /dev/null
if [ ! -f "$vc1" ]; then
    mkdir -p "${vc1%/*}"
    curl -sSfL -o "$vc1.part" "$VC1_URL" || fail "cannot download $VC1_URL"
    mv "$vc1.part" "$vc1"
fi
[ "$(sha256sum "$vc1" | cut -c1-64)" = "$VC1_SHA256" ] || fail "$vc1 does not match its pinned SHA-256"
[ -f "${vc1%.wmv}.txt" ] || python3 "$root/tools/make-mf-video-clip.py" --expect "$vc1" > /dev/null

# winedmo's Unix side for Linux: the module's files Wine marks as Unix side, linked as the host build links its own.
rm -rf "$work"
mkdir -p "$work/obj"
objects=""
for unit in $(sed -n '/^SOURCES/,/^$/p' "$tree/dlls/winedmo/Makefile.in" | grep -o '[A-Za-z0-9_/-]*\.c'); do
    grep -q 'pragma makedep unix' "$tree/dlls/winedmo/$unit" || continue
    object=$work/obj/$(echo "$unit" | tr / _ | sed 's/\.c$//').o
    cc -std=gnu11 -O2 -fPIC -D__WINESRC__ -DWINE_UNIX_LIB -D_REENTRANT -D_WINEDMO -DHAVE_FFMPEG=1 \
        -DHAVE_LIBAVCODEC_BSF_H=1 -I"$tree/dlls/winedmo" -I"$host/build/include" -I"$tree/include" \
        -I"$ff/install/include" -c "$tree/dlls/winedmo/$unit" -o "$object" 2>> "$work/compile.log" ||
        fail "cannot compile winedmo's $unit; see $work/compile.log"
    objects="$objects $object"
done
# shellcheck disable=SC2086
cc -o "$work/winedmo.so" -shared -Wl,-Bsymbolic -Wl,-soname,winedmo.so -Wl,-z,defs $objects \
    "$ff/install/lib/libavformat.a" "$ff/install/lib/libavcodec.a" "$ff/install/lib/libswscale.a" \
    "$ff/install/lib/libswresample.a" "$ff/install/lib/libavutil.a" "$host/build/dlls/ntdll/ntdll.so" \
    -lm -lpthread 2>> "$work/compile.log" || fail "cannot link winedmo.so; see $work/compile.log"

# The pinned Wine's build tree, shadowed (its install is not relocatable: ntdll has /usr/local in it): links to the
# shared build, real copies where Wine finds itself (the loaders, the server, ntdll) and where this tree's modules go.
cp -as "$host/build" "$work/runtime"
for file in tools/wine/wine loader/wine loader/wine-preloader server/wineserver dlls/ntdll/ntdll.so; do
    rm "$work/runtime/$file"
    cp "$host/build/$file" "$work/runtime/$file"
done
# Only the modules the media series (patches 2000 and up) changes: the rest of <work>/pe is built for the PS5 ntdll.
pin=$(sed -n 's/^WINE_COMMIT=//p' "$root/tools/build-wine-ps5.sh")
index=$work/before-media.index
GIT_INDEX_FILE=$index git -C "$tree" read-tree "$pin"
for patch in $(cd "$root/wine/patches" && ls | grep -E '^[01][0-9]{3}-.*\.patch$' | LC_ALL=C sort); do
    GIT_INDEX_FILE=$index git -C "$tree" apply --cached "$root/wine/patches/$patch" || fail "$patch does not apply"
done
before=$(GIT_INDEX_FILE=$index git -C "$tree" write-tree)
# ntdll's Unix side stays the host's: patch 2006 only drops a GStreamer setting for named games there.
media=$(python3 "$root/tools/wine-patched-modules.py" --unix-module dlls/winedmo --unix-module dlls/ntdll "$tree" "$before" \
    "$(cat "$ps5/source-tree")") ||
    fail "cannot tell which modules the media series changes"
for arch in x86_64-windows i386-windows; do
    for module in $media; do
        name=${module##*/}
        target=$work/runtime/${module%/*}/$arch/$name
        if [ ! -f "$ps5/pe/$arch/$name" ]; then
            # Some modules are one architecture's only (ir50_32 is 32-bit); one the host has must be there.
            [ ! -e "$target" ] || fail "no $arch $name in $ps5/pe"
            continue
        fi
        mkdir -p "${target%/*}"
        rm -f "$target"
        cp "$ps5/pe/$arch/$name" "$target"
    done
done
echo "media modules over the pinned Wine: $(echo $media | wc -w) ($(echo $media | tr ' ' '\n' | sed 's|.*/||' | tr '\n' ' '))"
rm -f "$work/runtime/dlls/winedmo/winedmo.so"
cp "$work/winedmo.so" "$work/runtime/dlls/winedmo/winedmo.so"
rm "$work/runtime/loader/wine.inf"
cp "$ps5/pe/wine.inf" "$work/runtime/loader/wine.inf"

export WINEPREFIX=$work/prefix WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=-all,+debugstr DISPLAY= WAYLAND_DISPLAY=
wine=$work/runtime/tools/wine/wine
timeout 300 "$wine" wineboot -u > "$work/wineboot.log" 2>&1 || fail "wineboot failed; see $work/wineboot.log"
for name in mf-video32-probe.exe mf-video64-probe.exe mf-video.mp4 mf-video.txt; do
    cp "$root/.deps/probes/$name" "$WINEPREFIX/drive_c/"
done
cp "$vc1" "${vc1%.wmv}.txt" "$WINEPREFIX/drive_c/"
status=0
for clip in mf-video.mp4 mf-vc1.wmv; do
    for bits in 64 32; do
        log=$work/${clip%.*}$bits.log
        timeout 300 "$wine" "C:\\mf-video$bits-probe.exe" "C:\\$clip" > "$log" 2>&1 || true
        verdict=$(grep -a -o 'PS5_PROTON_MF_VIDEO [A-Z]* checks=[0-9]* failures=[0-9]*[^\\]*' "$log" | head -1 || true)
        grep -a -o 'mf-video: [^\\]*' "$log" | sed "s/^/  $clip $bits: /"
        echo "$clip $bits: ${verdict:-no verdict (see $log)}"
        case $verdict in "PS5_PROTON_MF_VIDEO PASS"*) ;; *) status=1 ;; esac
    done
done
"$work/runtime/server/wineserver" -k 2>/dev/null || true
exit $status
