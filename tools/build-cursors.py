#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the Mihawk pointer scheme (.cur and .ani) from its pinned SVGs.

Inputs are themes/windows11/cursors/*.svg, checked against manifest.json, and
the icon builder's pinned renderer (ImageMagick 7.1.2-32, .deps/magick-7.1.2-32).
Outputs go to .deps/windows11-cursors, one file per scheme role, with
build.json and the initializer's table pw_cursor_scheme.h; stage-wine.py installs them in C:\\windows\\Cursors and the session
root names them in HKCU\\Control Panel\\Cursors (wine/ps5/pw_shell_cursors.h).
Every file is parsed back before it is kept, so a malformed cursor fails here.
"""
import hashlib
import io
import json
import struct
import subprocess
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'themes/windows11/cursors'
OUTPUT = ROOT / '.deps/windows11-cursors'
# The pinned renderer kept beside the build, else one on PATH (checked below either way).
MAGICK = str(ROOT / '.deps/magick-7.1.2-32/bin/magick') if (ROOT / '.deps/magick-7.1.2-32/bin/magick').exists() else 'magick'
SIZE = 32  # shortcut: 96 DPI only (SM_CXCURSOR 32); add 48/64 entries with phase 8's scaling


def render(svg):
    png = subprocess.check_output([MAGICK, '-background', 'none', 'svg:-', 'png32:-'], input=svg)
    with Image.open(io.BytesIO(png)) as image:
        image = image.convert('RGBA')
    if image.size != (SIZE, SIZE):
        raise ValueError(f'rendered {image.size}, expected {SIZE}x{SIZE}')
    return image


def cur(image, hotspot):
    """One-entry .cur: 32 bpp straight alpha, bottom-up, with an AND mask where alpha is 0."""
    width, height = image.size
    pixels = image.load()
    xor = b''.join(bytes((b, g, r, a)) for y in reversed(range(height)) for x in range(width)
                   for r, g, b, a in (pixels[x, y],))
    stride = (width + 31) // 32 * 4
    mask = bytearray()
    for y in reversed(range(height)):
        row = bytearray(stride)
        for x in range(width):
            if pixels[x, y][3] == 0: row[x // 8] |= 0x80 >> (x % 8)
        mask += row
    dib = struct.pack('<IiiHHIIiiII', 40, width, height * 2, 1, 32, 0, len(xor) + len(mask), 0, 0, 0, 0) + xor + mask
    return struct.pack('<HHH', 0, 2, 1) + struct.pack('<BBBBHHII', width, height, 0, 0, *hotspot, len(dib), 22) + dib


def chunk(tag, data):
    return tag + struct.pack('<I', len(data)) + data + (b'\0' if len(data) % 2 else b'')


def ani(frames, jiffies):
    """RIFF ACON with AF_ICON frames, each a complete .cur, shown in order."""
    header = struct.pack('<9I', 36, len(frames), len(frames), 0, 0, 0, 0, jiffies, 1)
    fram = b'fram' + b''.join(chunk(b'icon', frame) for frame in frames)
    body = b'ACON' + chunk(b'anih', header) + chunk(b'LIST', fram)
    return b'RIFF' + struct.pack('<I', len(body)) + body


def check_cur(data, hotspot):
    reserved, kind, count = struct.unpack_from('<HHH', data)
    width, height, _, _, hx, hy, size, offset = struct.unpack_from('<BBBBHHII', data, 6)
    bi_size, bi_width, bi_height, planes, bits = struct.unpack_from('<IiiHH', data, offset)
    assert (reserved, kind, count) == (0, 2, 1) and (width, height) == (SIZE, SIZE), 'cursor header'
    assert [hx, hy] == hotspot and offset + size == len(data), 'cursor hotspot or extent'
    assert (bi_size, bi_width, bi_height, planes, bits) == (40, SIZE, SIZE * 2, 1, 32), 'cursor bitmap'


def check_ani(data, count, hotspot):
    assert data[:4] == b'RIFF' and data[8:12] == b'ACON' and struct.unpack_from('<I', data, 4)[0] == len(data) - 8
    frames, position = [], 12
    while position < len(data):
        tag, size = data[position:position + 4], struct.unpack_from('<I', data, position + 4)[0]
        if tag == b'LIST':
            inner = position + 12
            while inner < position + 8 + size:
                length = struct.unpack_from('<I', data, inner + 4)[0]
                frames.append(data[inner + 8:inner + 8 + length])
                inner += 8 + length + length % 2
        position += 8 + size + size % 2
    assert len(frames) == count, 'animated cursor frame count'
    for frame in frames: check_cur(frame, hotspot)


def main():
    manifest = json.loads((SOURCE / 'manifest.json').read_text())
    named = {svg for role in manifest['scheme'].values() for svg in role['svg']}
    if named != set(manifest['sha256']) or named != {p.name for p in SOURCE.glob('*.svg')}:
        raise ValueError('cursor SVG inventory differs from manifest.json')
    sources = {}
    for name, digest in manifest['sha256'].items():
        data = (SOURCE / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError('SVG hash differs: ' + name)
        sources[name] = data
    version = subprocess.check_output([MAGICK, '-version'], text=True).splitlines()[0]
    if 'ImageMagick 7.1.2-32 Q16-HDRI' not in version:
        raise ValueError('Use the pinned ImageMagick 7.1.2-32 Q16-HDRI renderer (.deps/magick-7.1.2-32)')
    OUTPUT.mkdir(parents=True, exist_ok=True)
    record = {'scheme_name': manifest['scheme_name'], 'renderer': version, 'files': {}, 'scheme': {}}
    for role, item in sorted(manifest['scheme'].items()):
        frames = [cur(render(sources[svg]), item['hotspot']) for svg in item['svg']]
        if item['file'].endswith('.ani'):
            data = ani(frames, manifest['frame_jiffies'])
            check_ani(data, len(item['svg']), item['hotspot'])
        else:
            data = frames[0]
            check_cur(data, item['hotspot'])
        path = OUTPUT / item['file']
        if not path.exists() or path.read_bytes() != data: path.write_bytes(data)
        record['files'][item['file']] = hashlib.sha256(data).hexdigest()
        record['scheme'][role] = item['file']
    (OUTPUT / 'build.json').write_text(json.dumps(record, indent=1) + '\n')
    # The session root's initializer (wine/ps5/pw_shell_cursors.h) names exactly these files.
    header = ('/* Generated by tools/build-cursors.py from themes/windows11/cursors/manifest.json. */\n'
              f'#define PW_CURSOR_SCHEME_NAME L"{manifest["scheme_name"]}"\n#define PW_CURSOR_SCHEME {{ \\\n' +
              ''.join(f'    {{ L"{role}", L"{name}" }}, \\\n' for role, name in record['scheme'].items()) + '}\n')
    path = OUTPUT / 'pw_cursor_scheme.h'
    if not path.exists() or path.read_text() != header: path.write_text(header)
    print(json.dumps({'files': len(record['files']), 'scheme': record['scheme_name']}))


if __name__ == '__main__':
    main()
