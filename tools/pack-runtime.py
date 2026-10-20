#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pack the staged runtime into the chunks the title installs from (docs/SELF_INSTALL.md, phase 5).

  tools/pack-runtime.py [--stage .deps/staged] [--out dist/runtime] [--title dist/PPSA99170]
  tools/pack-runtime.py --title-pack dist/PPSA99170 --out DIR      the title folder itself, for an update

What a user install holds (runtime/ge11-7/ and run/proton-payload-child.elf, as tools/deploy-wine.py uploads), in
path order with the runtime's stamp last, cut into chunks at file boundaries. A chunk is one zstd -19 frame of its
files' bytes, one after another, with the default 8 MiB window and zstd's checksum; it is named by the SHA-256 of
those compressed bytes. Where a chunk ends is chosen by the files themselves (a cut after a file whose path and
digest hash to 0 mod 8, once the chunk holds 2 MiB, or once it holds 16 MiB), so a new runtime reuses every chunk
whose files did not change, and an update downloads only the rest. A file larger than 16 MiB is a chunk of its own.

The manifest, runtime-pack.txt, is written into the title folder (the copy the title trusts: it names every path,
size and SHA-256 it may install) and beside the chunks. src/pw_pack.h reads it:

  mihawkos-runtime-pack 1
  zstd <version> -19
  stamp <path>                                      removed first, renamed into place last
  chunk <sha256> <packed bytes> <plain bytes> <files>
  file <sha256> <bytes> <mtime> <path>              <files> lines after each chunk; mtime 0: the install's time

A title pack (--title-pack, "mihawkos-title-pack 1", title-pack.txt in DIR) holds every file of a title folder in
path order, with eboot.bin and then sce_sys/param.json last: ShadowMountPlus registers no title until both are in
place. The release's update installs it into a staging folder (src/pw_update.h); title-pack.txt itself, the
installed record, is not one of its files.

Only wine.inf keeps its time (a prefix updates when it changes, tools/stage-wine.py). Compressed chunks are cached
by the digest of their plain bytes in .deps/runtime-pack, so a rebuild compresses only what changed.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MIN_CHUNK = 2 << 20
MAX_CHUNK = 16 << 20
CUT_DIVISOR = 8
STAMP = 'runtime/ge11-7/runtime-stamp'
KEEP_TIME = 'runtime/ge11-7/share/wine/wine.inf'


def installed(relative):
    return relative.startswith('runtime/ge11-7/') or relative == 'run/proton-payload-child.elf'


TITLE_LAST = ('eboot.bin', 'sce_sys/param.json')


def chunks(files, stamps=(STAMP,)):
    """[(relative, digest, size)] in install order -> lists of them, cut by content; the stamps begin a chunk."""
    out, current, size = [], [], 0
    for relative, digest, length in files:
        if relative == stamps[0] and current:
            out.append(current)
            current, size = [], 0
        current.append((relative, digest, length))
        size += length
        cut = int(hashlib.sha256(f'{relative}\0{digest}'.encode()).hexdigest()[:8], 16) % CUT_DIVISOR == 0
        if size >= MAX_CHUNK or (size >= MIN_CHUNK and cut):
            out.append(current)
            current, size = [], 0
    if current:
        out.append(current)
    return out


def compress(stage, members, cache):
    # The files' digests name the plain bytes: nothing is read for a chunk already compressed.
    plain = hashlib.sha256(''.join(f'{relative}\0{digest}\n' for relative, digest, _ in members).encode())
    cached = cache / (plain.hexdigest() + '.zst')
    if not cached.exists():
        data = b''.join((stage / relative).read_bytes() for relative, _, _ in members)
        packed = subprocess.run(['zstd', '-19', '-T1', '-q', '-c', '--no-progress'], input=data, check=True,
                                stdout=subprocess.PIPE).stdout
        temporary = cached.with_suffix('.part')
        temporary.write_bytes(packed)
        os.replace(temporary, cached)
    return cached


def write_pack(kind, root, files, stamps, out, cache, jobs, mtime_of):
    """Compress `files` ([(relative, digest, size)], the stamps last) into out/<sha256>.zst; the manifest's text."""
    version = subprocess.run(['zstd', '-V'], check=True, capture_output=True, text=True).stdout
    version = version.split(' v', 1)[1].split(',', 1)[0]
    cache.mkdir(parents=True, exist_ok=True)
    groups = chunks(files, stamps)
    with ThreadPoolExecutor(jobs) as pool:
        packed = list(pool.map(lambda members: compress(root, members, cache), groups))
    out.mkdir(parents=True, exist_ok=True)
    lines = [f'mihawkos-{kind}-pack 1', f'zstd {version} -19'] + [f'stamp {s}' for s in stamps]
    names = set()
    for members, cached in zip(groups, packed):
        data = cached.read_bytes()
        name = hashlib.sha256(data).hexdigest()
        names.add(name + '.zst')
        target = out / (name + '.zst')
        if not target.exists():
            target.write_bytes(data)
        lines.append(f'chunk {name} {len(data)} {sum(m[2] for m in members)} {len(members)}')
        for relative, digest, length in members:
            lines.append(f'file {digest} {length} {mtime_of(relative)} {relative}')
    for stale in out.glob('*.zst'):
        if stale.name not in names:
            stale.unlink()
    plain = sum(f[2] for f in files)
    compressed = sum(p.stat().st_size for p in packed)
    print(f'{kind} pack: {len(files)} files, {plain} bytes in {len(groups)} chunks of {compressed} bytes '
          f'({100 * compressed / plain:.1f}%); {out}')
    return '\n'.join(lines) + '\n'


def plain_file(root, relative):
    path = root / relative
    if path.is_symlink() or not path.is_file() or any(c in relative for c in '\n\r\\ ') or '..' in relative.split('/'):
        raise SystemExit(f'{relative}: not a plain file with a plain path')
    return path


def title_pack(args):
    folder = args.title_pack
    names = sorted(str(p.relative_to(folder)) for p in folder.rglob('*')
                   if (p.is_file() or p.is_symlink()) and str(p.relative_to(folder)) != 'title-pack.txt')
    if any(last not in names for last in TITLE_LAST):
        raise SystemExit(f'{folder}: not a title folder (no {" or ".join(TITLE_LAST)})')
    files = []
    for relative in [n for n in names if n not in TITLE_LAST] + list(TITLE_LAST):
        data = plain_file(folder, relative).read_bytes()
        files.append((relative, hashlib.sha256(data).hexdigest(), len(data)))
    text = write_pack('title', folder, files, TITLE_LAST, args.out, args.cache, args.jobs, lambda relative: 0)
    (args.out / 'title-pack.txt').write_text(text)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--stage', type=Path, default=ROOT / '.deps/staged')
    parser.add_argument('--out', type=Path, default=ROOT / 'dist/runtime')
    parser.add_argument('--title', type=Path, default=ROOT / 'dist/PPSA99170')
    parser.add_argument('--cache', type=Path, default=ROOT / '.deps/runtime-pack')
    parser.add_argument('--jobs', type=int, default=os.cpu_count())
    parser.add_argument('--title-pack', type=Path, help='pack this title folder instead (for an update)')
    args = parser.parse_args()
    if args.title_pack:
        return title_pack(args)
    staged = json.loads((args.stage / 'manifest.json').read_text())
    if STAMP not in staged:
        # The stamp is what an install writes last (the launcher compares it): no stamp, no pack. A pin left from an
        # earlier build would make the title trust pieces of another runtime, so it goes too.
        (args.title / 'runtime-pack.txt').unlink(missing_ok=True)
        print(f'runtime pack: skipped, {args.stage} has no {STAMP} (tools/stage-wine.py writes it)')
        return 0
    files = []
    for relative in sorted(r for r in staged if installed(r) and r != STAMP) + [STAMP]:
        data = plain_file(args.stage, relative).read_bytes()
        if hashlib.sha256(data).hexdigest() != staged[relative]:
            raise SystemExit(f'{relative}: changed since it was staged')
        files.append((relative, staged[relative], len(data)))
    text = write_pack('runtime', args.stage, files, (STAMP,), args.out, args.cache, args.jobs,
                      lambda relative: int((args.stage / relative).stat().st_mtime) if relative == KEEP_TIME else 0)
    for folder in (args.out, args.title):
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'runtime-pack.txt').write_text(text)
    print(f'runtime pack: manifest in {args.title}')
    return 0

if __name__ == '__main__':
    sys.exit(main())
