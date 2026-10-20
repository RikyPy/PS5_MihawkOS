#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The first-install package: what a user copies to install MihawkOS (docs/SELF_INSTALL.md, phase 7's route; phase 9
publishes it beside the signed update, tools/sign-release.py).

  tools/package-install.py [--title dist/PPSA99170] [--runtime dist/runtime] [--out dist/package]

  OUT/PPSA99170/       the title folder: copy it into /data/homebrew (FTP from a phone or a PC, or a file manager)
  OUT/usb/mihawkos/    the runtime's pieces: copy usb/'s contents to the root of a USB stick (MBR, exFAT or FAT32)
  OUT/SHA256SUMS       every file's SHA-256, to check a copy (sha256sum -c, from OUT)
  OUT/README.txt       those steps

From a production build (tools/build-production.sh). Every piece the title pins (its runtime-pack.txt) must be in
--runtime at its size and SHA-256 (its name), and the title folder must hold nothing a development run leaves behind.
"""
import argparse
import hashlib
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# What a console run or a PC tool writes into a title folder, never part of a release.
LEFTOVERS = {'install-source.txt', 'update-source.txt', 'test-run.txt', 'wine-run.txt', 'elevation-retry',
             'session-running', 'session-notices', 'install-probe.txt', 'install-probe-results.txt', 'title-pack.txt'}
NEEDED = ('eboot.bin', 'sce_sys/param.json', 'runtime-pack.txt', 'runtime-stamp')
README = """MihawkOS: install it on a jailbroken PS5 (tested on firmware 10.01 and 13.40; supported up to 13.60)

1. Copy PPSA99170 into /data/homebrew on the console: over FTP from a phone or a PC, or with a file manager from a USB
   stick. ShadowMountPlus shows its tile on the home screen within about 45 seconds.
2. Copy the contents of usb/ (the mihawkos folder) to the root of a USB stick formatted exFAT or FAT32 with an MBR
   partition table (the PS5 does not mount a GPT stick), and plug it into the console.
3. Open MihawkOS. Its guided setup installs the Windows runtime from the stick and starts Mihawk OS once.

Each boot: load kstuff, elfldr and websrv, and ShadowMountPlus first. If another homebrew will not start after MihawkOS
closes, restart the PS5 (a known issue with ShadowMountPlus 1.7beta4).
SHA256SUMS lists every file here; from this folder, `sha256sum -c SHA256SUMS` checks a copy.
"""


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as file:
        for block in iter(lambda: file.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--title', type=Path, default=ROOT / 'dist/PPSA99170')
    parser.add_argument('--runtime', type=Path, default=ROOT / 'dist/runtime')
    parser.add_argument('--out', type=Path, default=ROOT / 'dist/package')
    args = parser.parse_args()
    missing = [name for name in NEEDED if not (args.title / name).is_file()]
    if missing:
        raise SystemExit(f'{args.title}: no {", ".join(missing)} (run tools/build-production.sh)')
    files = sorted(p for p in args.title.rglob('*') if not p.is_dir())
    stray = [str(p.relative_to(args.title)) for p in files if p.is_symlink() or p.name in LEFTOVERS]
    if stray:
        raise SystemExit(f'{args.title} holds what no release carries: {", ".join(stray)}')
    pieces = []
    for line in (args.title / 'runtime-pack.txt').read_text().splitlines():
        if line.startswith('chunk '):
            _, digest, packed = line.split()[:3]
            piece = args.runtime / f'{digest}.zst'
            if not piece.is_file() or piece.stat().st_size != int(packed) or sha256(piece) != digest:
                raise SystemExit(f'{piece}: missing, or not the piece runtime-pack.txt pins')
            pieces.append(piece)
    if not pieces:
        raise SystemExit('runtime-pack.txt pins no pieces')
    if args.out.exists():
        shutil.rmtree(args.out)
    title = args.out / args.title.name
    shutil.copytree(args.title, title)
    usb = args.out / 'usb/mihawkos'
    usb.mkdir(parents=True)
    for piece in pieces:
        shutil.copyfile(piece, usb / piece.name)
    (args.out / 'README.txt').write_text(README)
    sums = [f'{sha256(p)}  {p.relative_to(args.out)}' for p in sorted(args.out.rglob('*')) if p.is_file()]
    (args.out / 'SHA256SUMS').write_text('\n'.join(sums) + '\n')
    size = lambda folder: sum(p.stat().st_size for p in folder.rglob('*') if p.is_file())
    print(f'package: {args.out}\n  {title.name}/: {len(files)} files, {size(title)} bytes\n'
          f'  usb/mihawkos/: {len(set(pieces))} files for {len(pieces)} pieces (identical pieces share one), '
          f'{size(usb)} bytes\n  SHA256SUMS: {len(sums)} files')
    return 0


if __name__ == '__main__':
    sys.exit(main())
