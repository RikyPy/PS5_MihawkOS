#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/pack-runtime.py on a small staged tree: what src/pw_pack.c reads, and an update that reuses chunks."""
import hashlib
import json
import os
import random
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STAMP = 'runtime/ge11-7/runtime-stamp'


def stage(directory, files):
    manifest = {}
    for relative, data in files.items():
        path = directory / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        manifest[relative] = hashlib.sha256(data).hexdigest()
    os.utime(directory / 'runtime/ge11-7/share/wine/wine.inf', (1712345678, 1712345678))
    (directory / 'manifest.json').write_text(json.dumps(manifest))


def pack(work, name, files):
    staged = work / name
    stage(staged, files)
    subprocess.run([sys.executable, str(ROOT / 'tools/pack-runtime.py'), '--stage', str(staged), '--out', str(work / 'out'),
                    '--title', str(work / ('title-' + name)), '--cache', str(work / 'cache'), '--jobs', '4'],
                   check=True, capture_output=True)
    return (work / ('title-' + name) / 'runtime-pack.txt').read_text()


def chunks(text):
    """[(sha256, packed, plain, [(digest, size, mtime, path)])]"""
    out = []
    for line in text.splitlines():
        if not line.startswith(('chunk ', 'file ')):
            continue
        words = line.split(' ', 4)
        if words[0] == 'chunk':
            out.append((words[1], int(words[2]), int(words[3]), []))
        else:
            out[-1][3].append((words[1], int(words[2]), int(words[3]), words[4]))
    return out


class PackRuntime(unittest.TestCase):
    def test_pack_and_update(self):
        noise = random.Random(20261018).randbytes   # the same bytes every run: the cuts, and so the reuse, are fixed
        files = {f'runtime/ge11-7/lib/wine/x86_64-unix/m{i:03d}.so': noise(300000 + i * 5000) for i in range(40)}
        files['runtime/ge11-7/share/wine/wine.inf'] = b'[Version]\n' * 100
        files['runtime/ge11-7/share/wine/mono/big.msi'] = noise(20 << 20)
        files['run/proton-payload-child.elf'] = b'\x7fELF' + noise(1000)
        files['prefixes/probe/drive_c/not-installed.exe'] = b'MZ'
        files[STAMP] = b'one\n'
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            text = pack(work, 'one', files)
            lines = text.splitlines()
            self.assertEqual(lines[:3], ['mihawkos-runtime-pack 1', lines[1], f'stamp {STAMP}'])
            self.assertRegex(lines[1], r'^zstd \d+\.\d+\.\d+ -19$')
            listed = [f for c in chunks(text) for f in c[3]]
            paths = [f[3] for f in listed]
            self.assertEqual(paths[-1], STAMP)
            self.assertEqual(paths[:-1], sorted(paths[:-1]))
            self.assertEqual(set(paths), {p for p in files if not p.startswith('prefixes/')})
            self.assertEqual(chunks(text)[-1][3], [(hashlib.sha256(b'one\n').hexdigest(), 4, 0, STAMP)])
            for digest, size, mtime, path in listed:
                self.assertEqual(mtime, 1712345678 if path.endswith('wine.inf') else 0)
            for sha, packed, plain, members in chunks(text):
                data = (work / 'out' / (sha + '.zst')).read_bytes()
                self.assertEqual((len(data), hashlib.sha256(data).hexdigest()), (packed, sha))
                body = subprocess.run(['zstd', '-d', '-c'], input=data, check=True, capture_output=True).stdout
                self.assertEqual(len(body), plain)
                self.assertLess(plain - members[-1][1], 16 << 20)   # cut once it holds 16 MiB
                at = 0
                for digest, size, _, _ in members:
                    self.assertEqual(hashlib.sha256(body[at:at + size]).hexdigest(), digest)
                    at += size
            self.assertEqual(pack(work, 'again', files), text)   # the same tree packs to the same bytes
            changed = dict(files)
            changed['runtime/ge11-7/lib/wine/x86_64-unix/m020.so'] = noise(310000)
            changed[STAMP] = b'two\n'
            update = pack(work, 'two', changed)
            before = {c[0] for c in chunks(text)}
            after = [c[0] for c in chunks(update)]
            self.assertLessEqual(sum(c not in before for c in after), 3)   # the changed file's chunk, the stamp's
            self.assertGreater(sum(c in before for c in after), len(after) // 2)
            self.assertEqual({p.name for p in (work / 'out').glob('*.zst')}, {c + '.zst' for c in after})

    def test_no_stamp_no_pack(self):
        files = {'runtime/ge11-7/share/wine/wine.inf': b'[Version]\n', 'runtime/ge11-7/lib/a.so': b'a'}
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            (work / 'title-old').mkdir()
            (work / 'title-old' / 'runtime-pack.txt').write_text('a pin from an earlier build\n')
            stage(work / 'old', files)
            subprocess.run([sys.executable, str(ROOT / 'tools/pack-runtime.py'), '--stage', str(work / 'old'), '--out',
                            str(work / 'out'), '--title', str(work / 'title-old'), '--cache', str(work / 'cache')],
                           check=True, capture_output=True)
            self.assertFalse((work / 'title-old' / 'runtime-pack.txt').exists())

    def test_title_pack(self):
        files = {'eboot.bin': b'\x7fELF' + bytes(5000), 'sce_sys/param.json': b'{}', 'assets/z.ktx': b'z' * 3000,
                 'runtime-pack.txt': b'mihawkos-runtime-pack 1\n', 'title-pack.txt': b'the record of an earlier install\n'}
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            for relative, data in files.items():
                (work / 'title' / relative).parent.mkdir(parents=True, exist_ok=True)
                (work / 'title' / relative).write_bytes(data)
            run = [sys.executable, str(ROOT / 'tools/pack-runtime.py'), '--title-pack', str(work / 'title'), '--out',
                   str(work / 'pack'), '--cache', str(work / 'cache')]
            subprocess.run(run, check=True, capture_output=True)
            text = (work / 'pack' / 'title-pack.txt').read_text()
            self.assertEqual(text.splitlines()[0], 'mihawkos-title-pack 1')
            self.assertEqual(text.splitlines()[2:4], ['stamp eboot.bin', 'stamp sce_sys/param.json'])
            paths = [f[3] for c in chunks(text) for f in c[3]]
            # every file but the record, in path order, eboot.bin then param.json last
            self.assertEqual(paths, ['assets/z.ktx', 'runtime-pack.txt', 'eboot.bin', 'sce_sys/param.json'])
            (work / 'title' / 'eboot.bin').unlink()
            self.assertNotEqual(subprocess.run(run, capture_output=True).returncode, 0)


if __name__ == '__main__':
    unittest.main()
