#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify approved SVG pins, build alpha icons and export Wine shell resources.

Original coloured objects retain their paths. Fluent command glyphs use the
approved light/dark foreground colours. No preview HTML, font or wallpaper is
an input. The resource-only DLL is shared by PE32/PE64 as a data file; shell32's
existing resources retain their IDs, eligibility and actual filesystem state.
"""
import argparse
import hashlib
import io
import json
import re
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path
from PIL import Image
import pefile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'themes/windows11/icons'
OUTPUT = ROOT / '.deps/mihawk-icons'
# The pinned renderer: tools/fetch-all.sh puts it here when PATH has no ImageMagick 7.1.2-32 Q16-HDRI.
MAGICK = ROOT / '.deps/magick-7.1.2-32/bin/magick'
SIZES = (16, 20, 24, 32, 48, 64)
# An app's icon is also a window's, a shortcut's and a large view's: up to 256 px.
APP_SIZES = SIZES + (128, 256)
# Pinned shell32 resource IDs; app-provided icons are never replaced.
SHELL_ICONS = {1: 'document', 2: 'document', 4: 'folder', 5: 'folder-open',
               9: 'drive', 16: 'this-pc', 20: 'folder-open', 29: 'folder-open',
               31: 'folder-open', 32: 'recycle-empty', 33: 'recycle-full',
               35: 'desktop', 191: 'recycle-empty', 192: 'recycle-full',
               231: 'downloads', 235: 'documents', 236: 'pictures',
               237: 'music', 238: 'videos', 254: 'recycle-empty'}


def write_changed(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def verified_sources():
    manifest = json.loads((SOURCE / 'manifest.json').read_text())
    for category, metadata, field in [('original', 'provenance.json', 'original_icons'),
                                      ('fluent', 'upstream.json', 'fluent_icons')]:
        record = json.loads((SOURCE / category / metadata).read_text())
        if category == 'fluent' and (record['revision'] != manifest['fluent_revision'] or
                                    record['license'] != 'MIT'):
            raise ValueError('Fluent revision/licence differs from manifest')
        if set(record['icons']) != set(manifest[field]):
            raise ValueError('Icon inventory differs from provenance')
        for name in manifest[field]:
            path = SOURCE / category / (name + '.svg')
            expected = record['icons'][name]
            if category == 'fluent': expected = expected['sha256']
            data = path.read_bytes()
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError('SVG hash differs: ' + str(path.relative_to(ROOT)))
            yield category, name, data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wine-tree', type=Path)
    args = parser.parse_args()
    inputs = list(verified_sources())  # Refuse bad pins before generating anything.
    palette = json.loads((ROOT / 'themes/windows11/appearance.json').read_text())
    magick = str(MAGICK) if MAGICK.is_file() else 'magick'
    info = subprocess.check_output([magick, '-version'], text=True)
    version = info.splitlines()[0]
    # Without librsvg, ImageMagick's own SVG renderer draws the gradient-filled objects black (its 7.1.2-32 AppImage).
    if 'ImageMagick 7.1.2-32 Q16-HDRI' not in version or not re.search(r'^Delegates.* rsvg', info, re.M):
        raise ValueError('Use the pinned ImageMagick 7.1.2-32 Q16-HDRI renderer with its librsvg delegate')
    OUTPUT.mkdir(parents=True, exist_ok=True)
    resources, inventory = [], []
    for category, name, svg in inputs:
        variants = ['original'] if category == 'original' else ['light', 'dark']
        for variant in variants:
            artwork = svg if category == 'original' else svg.replace(b'#212121', palette[variant]['text'].encode())
            folder = OUTPUT / variant
            folder.mkdir(exist_ok=True)
            root = ET.fromstring(artwork)
            root.set('width', '256'); root.set('height', '256')
            rendered = subprocess.check_output([magick, '-background', 'none', 'svg:-',
                'png32:-'], input=ET.tostring(root))
            with Image.open(io.BytesIO(rendered)) as source:
                source = source.convert('RGBA')
                if source.getextrema()[3][0] != 0 or source.getextrema()[3][1] == 0:
                    raise ValueError('Icon lost transparency or visible ink: ' + name)
                sizes = APP_SIZES if name.startswith('app-') else SIZES
                for size in sizes:
                    image = source.resize((size, size), Image.Resampling.LANCZOS)
                    png = io.BytesIO(); image.save(png, format='PNG')
                    write_changed(folder / f'{name}-{size}.png', png.getvalue())
                ico = io.BytesIO()
                source.save(ico, format='ICO', sizes=[(s, s) for s in sizes])
                path = folder / (name + '.ico'); write_changed(path, ico.getvalue())
            with Image.open(path) as icon:
                if icon.ico.sizes() != {(s, s) for s in sizes}:
                    raise ValueError('ICO size inventory differs: ' + name)
            resource_id = (1000 if variant == 'original' else 2000 if variant == 'light' else 3000) + sum(
                1 for item in inventory if item['variant'] == variant)
            resources.append(f'{resource_id} ICON {json.dumps(str(path))}')
            inventory.append({'name': name, 'variant': variant, 'id': resource_id,
                              'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    for resource_id, name in SHELL_ICONS.items():
        resources.append(f'{resource_id} ICON {json.dumps(str(OUTPUT / "original" / (name + ".ico")))}')
    rc = OUTPUT / 'icons.rc'; rc.write_text('LANGUAGE 0, 0\n' + '\n'.join(resources) + '\n')
    compiler = ROOT / '.deps/llvm-mingw/bin'
    subprocess.run([str(compiler / 'x86_64-w64-mingw32-windres'), str(rc), str(OUTPUT / 'icons.o')], check=True)
    dll = OUTPUT / 'icons.dll'
    subprocess.run([str(compiler / 'lld-link'), '/dll', '/noentry', '/machine:x64', '/timestamp:0',
                    '/out:' + str(dll), str(OUTPUT / 'icons.o')], check=True)
    pe = pefile.PE(str(dll))
    if pe.OPTIONAL_HEADER.AddressOfEntryPoint or getattr(pe, 'DIRECTORY_ENTRY_IMPORT', None):
        raise ValueError('Icon DLL must contain only resources')
    groups = next(item for item in pe.DIRECTORY_ENTRY_RESOURCE.entries if item.id == 14)
    if {item.id for item in groups.directory.entries} != {x['id'] for x in inventory} | set(SHELL_ICONS):
        raise ValueError('Compiled group-icon IDs differ')
    if args.wine_tree:
        folder = args.wine_tree / 'dlls/shell32/resources/mihawk'
        for name in set(SHELL_ICONS.values()):
            write_changed(folder / (name + '.ico'), (OUTPUT / 'original' / (name + '.ico')).read_bytes())
    record = {'passed': True, 'runtime_qualified': False, 'renderer': version, 'sizes': SIZES,
              'original_sources': sum(c == 'original' for c, _, _ in inputs),
              'fluent_sources': sum(c == 'fluent' for c, _, _ in inputs), 'resources': inventory,
              'shell_resource_ids': sorted(SHELL_ICONS), 'dll_sha256': hashlib.sha256(dll.read_bytes()).hexdigest()}
    (OUTPUT / 'build.json').write_text(json.dumps(record, indent=1) + '\n')
    header = '/* Generated from the verified icon manifest. */\n'
    for item in inventory:
        symbol = ('PW_ICON_' + item['variant'] + '_' + item['name']).upper().replace('-', '_')
        header += f'#define {symbol} {item["id"]}\n'
    write_changed(OUTPUT / 'icons.h', header.encode())
    print(json.dumps({'passed': True, 'icons': len(inventory), 'sizes': SIZES,
                      'shell_resources': len(SHELL_ICONS), 'dll_sha256': record['dll_sha256']}))


if __name__ == '__main__': main()
