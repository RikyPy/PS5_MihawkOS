#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pinned, unmodified FFmpeg for winedmo.prx, the media backend of GE-Proton's video rework (patches 2001-2062).

GE-Proton builds winedmo against FFmpeg n8.1 (proton-ge-custom's ffmpeg submodule, 9047fa1b0 = n8.1); this is the
8.1.3 point release of that line, checked against the release tarball's SHA-256 (its signature, by FFmpeg's release
key FCF986EA15E6E293A5644F10B4322F04D67658D8, was verified when the pin was taken). LGPL-2.1-or-later only: no
--enable-gpl, no --enable-nonfree, nothing external. Static archives built with the payload SDK's compiler; only the
title's libraries resolve them, in the PRX link (tools/build-wine-ps5.sh).

The components are GE's (proton-ge-custom Makefile.in, FFMPEG_CONFIGURE_ARGS): everything off, then every demuxer,
decoder but VVC, encoder and bitstream filter, and no parser beyond what those pull in. That last part matters:
winedmo hands WMV, VC-1, MPEG video and H.264 without codec data to av_parser_parse2 when FFmpeg has a parser for the
codec, and its own packet assembly otherwise, the path GE runs. With every parser enabled, a Direct3D 12 game's VC-1
intro (WVC1 in ASF) and a public WVC1 sample decoded no frame at all (2026-10-17; the sample on the
host oracle, 0 of 1451 frames), and they decode bit-exact without. Two differences, each for the console: no HTTPS,
lzma or bzip2 (the console has no TLS, xz or bzip2 library for FFmpeg: winedmo's HLS and HTTP paths and the rare
compressed tracks fail as on a PC without them), and inline assembly kept (GE turns it off; H.264's CABAC decoding
uses it, and GE's winedmo decodes H.264 on one thread). No muxers, as in GE's build: MF's MP4 and MP3 sinks fail to
create.

--host builds the same configuration natively for the host oracle (tools/check-media-host.sh).

  tools/build-ffmpeg-ps5.py [--work DIR] [--sdk DIR | --host] [--tarball FILE] [--jobs N]
"""
from pathlib import Path
import argparse
import hashlib
import os
import shutil
import subprocess
import tarfile
import urllib.request

VERSION = '8.1.3'
SHA256 = '7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3'
URL = f'https://ffmpeg.org/releases/ffmpeg-{VERSION}.tar.xz'
# GE-Proton's selection (Makefile.in FFMPEG_CONFIGURE_ARGS).
COMPONENTS = ['--disable-everything', '--enable-demuxers', '--enable-decoders', '--enable-encoders', '--enable-bsfs',
              '--disable-decoder=vvc']


def configure_args(sdk, install):
    if sdk is None:   # the host oracle: the same configuration, native
        target = ['--cc=cc']
    else:
        tool = lambda name: str(sdk / 'bin' / name)
        target = ['--enable-cross-compile', '--target-os=freebsd', '--arch=x86_64', '--cc=' + tool('prospero-clang'),
                  '--ar=' + tool('llvm-ar'), '--ranlib=' + tool('llvm-ranlib'), '--nm=' + tool('llvm-nm'),
                  '--strip=' + tool('llvm-strip')]
    return ['--prefix=' + str(install)] + target + [
            '--x86asmexe=nasm', '--enable-static', '--disable-shared', '--enable-pic', '--enable-pthreads',
            '--disable-autodetect', '--disable-programs', '--disable-doc', '--disable-network', '--disable-avdevice',
            '--disable-avfilter', '--disable-debug', '--extra-cflags=-ffunction-sections -fdata-sections'] + COMPONENTS


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--work', type=Path, default=root / '.deps/wine-ps5/ffmpeg')
    parser.add_argument('--sdk', type=Path, default=root / '.deps/native/ps5-payload-sdk')
    parser.add_argument('--host', action='store_true', help='build natively for the host oracle')
    parser.add_argument('--tarball', type=Path)
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    work, sdk = args.work.resolve(), None if args.host else args.sdk.resolve()
    install = work / 'install'
    stamp = hashlib.sha256('\n'.join([SHA256, str(sdk)] + configure_args(sdk, install)).encode()).hexdigest()
    if (work / '.prospero-stamp').is_file() and (work / '.prospero-stamp').read_text().strip() == stamp:
        print(f'FFmpeg {VERSION} already built: {install}')
        return
    if not shutil.which('nasm'):
        raise SystemExit('build-ffmpeg-ps5: no nasm on PATH (FFmpeg\'s x86 SIMD is assembled with it)')
    work.mkdir(parents=True, exist_ok=True)
    archive = args.tarball or work / f'ffmpeg-{VERSION}.tar.xz'
    if not archive.exists():
        with urllib.request.urlopen(URL, timeout=60) as response:
            archive.write_bytes(response.read())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise SystemExit(f'build-ffmpeg-ps5: {archive} differs from FFmpeg {VERSION}\'s pinned SHA-256')
    for path in (work / 'source', work / 'build', install):
        shutil.rmtree(path, ignore_errors=True)
    with tarfile.open(archive) as tar:
        tar.extractall(work, filter='data')
    (work / f'ffmpeg-{VERSION}').rename(work / 'source')
    (work / 'build').mkdir()
    env = {key: value for key, value in os.environ.items() if key not in ('LDFLAGS', 'CFLAGS', 'MAKEFLAGS', 'MFLAGS')}
    for step in ([str(work / 'source/configure')] + configure_args(sdk, install),
                 ['make', f'-j{args.jobs}'], ['make', 'install']):
        with open(work / 'build.log', 'a') as log:
            if subprocess.run(step, cwd=work / 'build', env=env, stdout=log, stderr=subprocess.STDOUT).returncode:
                raise SystemExit(f'build-ffmpeg-ps5: {step[0]} failed; see {work / "build.log"}')
    # What can be shipped: FFmpeg's own record of the licence this configuration gives.
    config = (work / 'build/config.h').read_text()
    for flag in ('CONFIG_GPL', 'CONFIG_NONFREE', 'CONFIG_GPLV3', 'CONFIG_VERSION3'):
        if f'#define {flag} 0' not in config:
            raise SystemExit(f'build-ffmpeg-ps5: {flag} is not 0: this build would not be LGPL-2.1-or-later only')
    licences = work / 'licenses'
    licences.mkdir(exist_ok=True)
    for name in ('LICENSE.md', 'COPYING.LGPLv2.1'):
        shutil.copy2(work / 'source' / name, licences / name)
    (work / '.prospero-stamp').write_text(stamp + '\n')
    print(f'Built pinned FFmpeg {VERSION}: {install}')


if __name__ == '__main__':
    main()
