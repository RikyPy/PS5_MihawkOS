#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pinned, unmodified c-ares for the Wine DNSAPI PRX; no implicit moving inputs."""
from pathlib import Path
import argparse
import hashlib
import shutil
import subprocess
import tarfile
import urllib.request

VERSION = '1.34.8'
SHA256 = 'c222b6d681096f9444d2c4863d2c1174019e27cacca0a4a5c114d36dd7d7bf78'
URL = f'https://github.com/c-ares/c-ares/releases/download/v{VERSION}/c-ares-{VERSION}.tar.gz'


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, default=root / '.deps/wine-ps5/cares')
    parser.add_argument('--sdk', type=Path, default=root / '.deps/native/ps5-payload-sdk')
    parser.add_argument('--tarball', type=Path)
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    archive = args.tarball or args.work / f'c-ares-{VERSION}.tar.gz'
    if not archive.exists():
        with urllib.request.urlopen(URL, timeout=30) as response:
            archive.write_bytes(response.read())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise SystemExit('c-ares archive differs from pinned SHA-256')
    source = args.work / f'c-ares-{VERSION}'
    if not source.exists():
        with tarfile.open(archive) as tar:
            tar.extractall(args.work, filter='data')
    toolchain = args.work / 'toolchain.cmake'
    toolchain.write_text(f'set(CMAKE_SYSTEM_NAME FreeBSD)\nset(CMAKE_SYSTEM_VERSION 11)\n'
                         f'set(CMAKE_C_COMPILER "{args.sdk.resolve()}/bin/prospero-clang")\n'
                         'set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)\n'
                         'set(CMAKE_POSITION_INDEPENDENT_CODE ON)\n')
    subprocess.run(['cmake', '-S', str(source), '-B', str(args.work / 'build'), '-G', 'Ninja',
                    '-DCMAKE_TOOLCHAIN_FILE=' + str(toolchain.resolve()), '-DCMAKE_BUILD_TYPE=Release',
                    '-DCARES_SHARED=OFF', '-DCARES_STATIC=ON', '-DCARES_BUILD_TOOLS=OFF',
                    '-DCMAKE_C_FLAGS=-ffunction-sections -fdata-sections',
                    '-DCARES_BUILD_TESTS=OFF', '-DCARES_THREADS=OFF', '-DCARES_INSTALL=OFF',
                    # SDK headers declare these, but title libc exports do not.
                    '-DHAVE_ARC4RANDOM_BUF=0', '-DHAVE_GETSERVBYNAME_R=0',
                    '-DHAVE_GETSERVBYPORT_R=0', '-DHAVE_MEMMEM=0'], check=True)
    subprocess.run(['cmake', '--build', str(args.work / 'build'), '-j12'], check=True)
    shutil.copy2(source / 'LICENSE.md', args.work / 'LICENSE.md')
    print(f'Built pinned c-ares {VERSION}: {args.work / "build/lib/libcares.a"}')


if __name__ == '__main__':
    main()
