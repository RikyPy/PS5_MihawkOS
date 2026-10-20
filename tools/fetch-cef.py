#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch CEF's Windows x64 minimal distribution at its pin and unpack what MihawkBrowser builds against and ships.

The archive is verified against the SHA-1 that cef-builds' index.json publishes and the SHA-256 pinned below, then
only the C headers, the import library, the ship list (SHIP), the licences and the H3-only candidates (CANDIDATES,
never shipped unless docs/MIHAWK_BROWSER.md decision 5 says so) are unpacked into .deps/cef/<NAME>. ship.json there
records each shipped file's SHA-256 and the CEF commit from include/cef_version.h; a missing name fails. Idempotent:
an unpacked tree whose ship.json matches the pin is left alone. A re-pin is a reviewed change (decision 10).

    tools/fetch-cef.py            fetch (or take from PS5_PROTON_CACHE) and unpack
    tools/fetch-cef.py --check    exit 0 when unpacked at the pin, 2 when not
    tools/fetch-cef.py --url      print the pinned archive's URL
"""
import hashlib
import json
import os
import re
import shutil
import sys
import tarfile
import urllib.parse
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CEF = ROOT / '.deps/cef'
NAME = 'cef_binary_150.0.22+gc1a097e+chromium-150.0.7871.256_windows64_minimal'
URL = 'https://cef-builds.spotifycdn.com/' + urllib.parse.quote(NAME) + '.tar.bz2'
SHA1 = '01011e3f5b05caacb3b724c7965b047ffd33fd18'  # index.json, 2026-10-09T04:53:14Z
SHA256 = 'a6afba8ee3d1cc9ef64318a734dfeb5ceafc0384a46df2af37b857e7d76c0544'
BYTES = 164266201
CHROMIUM = '150.0.7871.256'
SHIP = ['Release/libcef.dll', 'Release/chrome_elf.dll', 'Release/libEGL.dll', 'Release/libGLESv2.dll',
        'Release/v8_context_snapshot.bin', 'Resources/icudtl.dat', 'Resources/resources.pak',
        'Resources/chrome_100_percent.pak', 'Resources/locales/en-US.pak', 'Resources/locales/en-US_FEMININE.pak',
        'Resources/locales/en-US_MASCULINE.pak', 'Resources/locales/en-US_NEUTER.pak']
LICENCES = ['LICENSE.txt', 'CREDITS.html']
# Never shipped: Microsoft's compiler and Khronos's loader for the H3 graphics arms (decision 5), and SwiftShader, the
# software Vulkan the headless host oracle draws with (its Wine has no display driver, so no GPU path presents there).
CANDIDATES = ['Release/d3dcompiler_47.dll', 'Release/vulkan-1.dll', 'Release/vk_swiftshader.dll',
              'Release/vk_swiftshader_icd.json']
BUILD = ['Release/libcef.lib']


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''): digest.update(block)
    return digest.hexdigest()


def archive():
    path = CEF / (NAME + '.tar.bz2')
    cached = Path(os.environ.get('PS5_PROTON_CACHE', '/nonexistent')) / path.name
    if not path.exists() and cached.is_file() and sha256(cached) == SHA256:
        CEF.mkdir(parents=True, exist_ok=True); shutil.copyfile(cached, path)
    if not path.exists():
        CEF.mkdir(parents=True, exist_ok=True)
        print('fetching', URL)
        part = path.with_suffix('.part')
        request = urllib.request.Request(URL, headers={'User-Agent': 'PS5_Proton fetch-cef'})
        with urllib.request.urlopen(request, timeout=120) as response, open(part, 'wb') as out:
            shutil.copyfileobj(response, out, 1 << 20)
        part.rename(path)
    sha1 = hashlib.sha1(path.read_bytes()).hexdigest()
    if path.stat().st_size != BYTES or sha1 != SHA1 or sha256(path) != SHA256:
        sys.exit(f'{path.name}: size, SHA-1 or SHA-256 differs from the pin (delete it to fetch again)')
    return path


def main():
    if '--url' in sys.argv[1:]:
        print(URL); return 0
    out = CEF / NAME
    record = out / 'ship.json'
    if record.exists() and json.loads(record.read_text()).get('archive_sha256') == SHA256 and \
            json.loads(record.read_text()).get('candidates') == CANDIDATES:
        print('ok       ', record.relative_to(ROOT)); return 0
    if '--check' in sys.argv[1:]:
        print('absent   ', record.relative_to(ROOT)); return 2
    source = archive()
    partial = out.with_name(out.name + '.partial')
    shutil.rmtree(partial, ignore_errors=True)
    wanted = set(SHIP + LICENCES + CANDIDATES + BUILD)
    with tarfile.open(source, 'r:bz2') as tar:
        members = []
        for member in tar.getmembers():
            relative = member.name.split('/', 1)[1] if '/' in member.name else ''
            if not member.isfile(): continue
            if relative in wanted or (relative.startswith('include/') and relative.endswith('.h')):
                member.name = relative
                members.append(member)
        tar.extractall(partial, members=members, filter='data')
    missing = [name for name in sorted(wanted) if not (partial / name).is_file()]
    if missing: sys.exit('missing from the archive: ' + ', '.join(missing))
    version = (partial / 'include/cef_version.h').read_text()
    commit = re.search(r'#define CEF_COMMIT_HASH "([0-9a-f]{40})"', version).group(1)
    api = re.search(r'#define CEF_API_VERSION_LAST CEF_API_VERSION_(\d+)', (partial / 'include/cef_api_versions.h').read_text())
    files = {name: {'sha256': sha256(partial / name), 'bytes': (partial / name).stat().st_size}
             for name in SHIP + LICENCES + CANDIDATES + BUILD}
    (partial / 'ship.json').write_text(json.dumps({
        'name': NAME, 'url': URL, 'archive_sha1': SHA1, 'archive_sha256': SHA256, 'archive_bytes': BYTES,
        'cef_commit': commit, 'chromium': CHROMIUM, 'api_version_last': api and int(api.group(1)),
        'ship': SHIP, 'licences': LICENCES, 'candidates': CANDIDATES,
        'ship_bytes': sum(files[name]['bytes'] for name in SHIP), 'files': files}, indent=1) + '\n')
    shutil.rmtree(out, ignore_errors=True)
    partial.rename(out)
    print('unpacked ', out.relative_to(ROOT), 'cef', commit, 'ship', sum(files[n]['bytes'] for n in SHIP), 'bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
