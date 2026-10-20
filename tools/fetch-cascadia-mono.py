#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch Cascadia Mono's static TTFs, unmodified, from Microsoft's pinned release into themes/windows11/fonts/cascadia.

Only the named members of the release zip are read, with HTTP range requests (its central directory, then each
member's local header and data); each is inflated, checked against the zip's CRC-32 and the SHA-256 pinned below,
and written beside the release's licence. The files are committed: the build never fetches them. Run it only to
re-pin (a new release changes PINS).
"""
import hashlib
import json
import struct
import sys
import urllib.request
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / 'themes/windows11/fonts/cascadia'
RELEASE = 'v2407.24'
ZIP = f'https://github.com/microsoft/cascadia-code/releases/download/{RELEASE}/CascadiaCode-2407.24.zip'
LICENSE = f'https://raw.githubusercontent.com/microsoft/cascadia-code/{RELEASE}/LICENSE'
PINS = {  # member -> SHA-256 of the inflated file
    'ttf/static/CascadiaMono-Regular.ttf': '06520d032ec274fa5040b22c6f4a1d829081b24ba40b2da56dae89bf10c7b481',
    'ttf/static/CascadiaMono-Bold.ttf': 'b22cb603ed23cac36e8444846e1841caca21719a5e852780a8d37ae7a49b0a36',
}
LICENSE_SHA256 = '51882cd3cdba4e16f220f44ddb08a635c38c44ea6e0975db2574f4be6f958238'  # as the prototype pinned it


def get(url, start=None, end=None):
    request = urllib.request.Request(url, headers={'User-Agent': 'PS5_Proton fetch-cascadia-mono'})
    if start is not None: request.add_header('Range', f'bytes={start}-{end}')
    with urllib.request.urlopen(request, timeout=60) as response:
        return response.read(), response.headers


def main():
    _, headers = get(ZIP, 0, 0)
    size = int(headers['Content-Range'].split('/')[1])
    tail, _ = get(ZIP, size - 65557, size - 1)
    eocd = tail.rindex(b'PK\x05\x06')
    entries, cd_size, cd_offset = struct.unpack_from('<HII', tail, eocd + 10)
    directory, _ = get(ZIP, cd_offset, cd_offset + cd_size - 1)
    members, position = {}, 0
    for _ in range(entries):
        (signature, _, _, _, method, _, _, crc, compressed, _, name_length, extra_length, comment_length,
         _, _, _, offset) = struct.unpack_from('<IHHHHHHIIIHHHHHII', directory, position)
        assert signature == 0x02014b50, 'central directory'
        name = directory[position + 46:position + 46 + name_length].decode()
        members[name] = (method, crc, compressed, offset)
        position += 46 + name_length + extra_length + comment_length
    OUTPUT.mkdir(parents=True, exist_ok=True)
    record = {'name': 'Cascadia Mono (static Regular and Bold)', 'repository': 'https://github.com/microsoft/cascadia-code',
              'release': RELEASE, 'asset': ZIP, 'how': 'only these members were read from the release zip with HTTP range '
              'requests; CRC-32 verified on extraction; unmodified', 'license': 'OFL-1.1 (LICENSE, from the repository at '
              f'{RELEASE}; Reserved Font Name Cascadia Code)', 'files': {}}
    for member, pin in PINS.items():
        method, crc, compressed, offset = members[member]
        header, _ = get(ZIP, offset, offset + 29)
        name_length, extra_length = struct.unpack_from('<HH', header, 26)
        start = offset + 30 + name_length + extra_length
        data, _ = get(ZIP, start, start + compressed - 1)
        data = zlib.decompress(data, -15) if method == 8 else data
        if zlib.crc32(data) != crc: sys.exit(f'{member}: CRC-32 differs from the zip')
        digest = hashlib.sha256(data).hexdigest()
        if pin and digest != pin: sys.exit(f'{member}: SHA-256 {digest} differs from the pin {pin}')
        (OUTPUT / Path(member).name).write_bytes(data)
        record['files'][Path(member).name] = {'member': member, 'sha256': digest, 'crc32': '%08x' % crc, 'bytes': len(data)}
    licence, _ = get(LICENSE)
    if hashlib.sha256(licence).hexdigest() != LICENSE_SHA256: sys.exit('LICENSE differs from the pinned one')
    (OUTPUT / 'LICENSE').write_bytes(licence)
    record['files']['LICENSE'] = {'sha256': LICENSE_SHA256}
    (OUTPUT / 'upstream.json').write_text(json.dumps(record, indent=1) + '\n')
    print(json.dumps(record['files'], indent=1))


if __name__ == '__main__':
    main()
