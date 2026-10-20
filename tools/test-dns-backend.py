#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Sanitized exact DNSAPI adapter against pinned c-ares, using the host's DNS.

Build prerequisites: tools/build-production.sh supplies the pinned source archive
and the host Wine headers. No downloads or configured addresses are printed.
"""
from pathlib import Path
import hashlib
import os
import subprocess
import tarfile
from importlib.util import spec_from_file_location, module_from_spec

ROOT = Path(__file__).resolve().parents[1]


def main():
    spec = spec_from_file_location('cares_pin', ROOT / 'tools/build-cares-ps5.py')
    pin = module_from_spec(spec)
    spec.loader.exec_module(pin)
    archive = ROOT / f'.deps/wine-ps5/cares/c-ares-{pin.VERSION}.tar.gz'
    if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != pin.SHA256:
        raise SystemExit('Run the pinned production dependency build first; c-ares archive is absent or differs')
    work = ROOT / '.deps/host-tests/dnsapi'
    work.mkdir(parents=True, exist_ok=True)
    source = work / f'c-ares-{pin.VERSION}'
    if not source.exists():
        with tarfile.open(archive) as tar:
            tar.extractall(work, filter='data')
    log = work / 'build.log'
    with log.open('w') as output:
        def run(args):
            subprocess.run(args, stdout=output, stderr=subprocess.STDOUT, check=True)
        run(['cmake', '-S', str(source), '-B', str(work / 'build'), '-G', 'Ninja',
             '-DCARES_SHARED=OFF', '-DCARES_STATIC=ON', '-DCARES_THREADS=OFF',
             '-DCARES_BUILD_TESTS=OFF', '-DCARES_BUILD_TOOLS=OFF', '-DCARES_INSTALL=OFF',
             '-DCMAKE_BUILD_TYPE=Debug', '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer'])
        run(['cmake', '--build', str(work / 'build'), '-j12'])
        wine = ROOT / '.deps/wine-runtime'
        run(['clang', '-std=gnu11', '-g', '-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             '-D__WINESRC__', '-DWINE_UNIX_LIB', '-D_REENTRANT',
             '-I' + str(wine / 'build/include'), '-I' + str(wine / 'source/include'),
             '-I' + str(wine / 'source/dlls/dnsapi'), '-I' + str(ROOT / 'wine/ps5'),
             '-I' + str(source / 'include'), '-I' + str(work / 'build'),
             str(ROOT / 'wine/ps5/pw_dns_unix.c'), str(ROOT / 'tests/test_pw_dns_unix.c'),
             str(work / 'build/lib/libcares.a'), '-lpthread', '-o', str(work / 'test')])
    env = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1', ASAN_OPTIONS='detect_leaks=1')
    result = subprocess.run([str(work / 'test')], env=env, capture_output=True, text=True, timeout=110)
    (work / 'test.log').write_text(result.stdout + result.stderr)
    if result.returncode or 'PW_DNS_UNIX PASS' not in result.stdout:
        raise SystemExit(f'DNSAPI sanitizer check failed; see {work / "test.log"}')
    print(result.stdout.strip())


if __name__ == '__main__':
    main()
