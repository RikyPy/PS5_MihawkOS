#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/package-install.py on a small title and runtime: the layout a user copies, its checksums, and the refusals
(a development leftover in the title folder, a piece that is not the one the title pins)."""
import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class PackageInstall(unittest.TestCase):
    def test_package(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            title, runtime, out = work / 'PPSA99170', work / 'runtime', work / 'package'
            runtime.mkdir()
            piece = b'(not really zstd)' * 100
            digest = hashlib.sha256(piece).hexdigest()
            (runtime / f'{digest}.zst').write_bytes(piece)
            for relative, data in {'eboot.bin': b'\x7fELF', 'sce_sys/param.json': b'{}', 'runtime-stamp': b'x\n',
                                   'runtime-pack.txt': f'mihawkos-runtime-pack 1\nchunk {digest} {len(piece)} 9 1\n'.encode()}.items():
                (title / relative).parent.mkdir(parents=True, exist_ok=True)
                (title / relative).write_bytes(data)
            run = [sys.executable, str(ROOT / 'tools/package-install.py'), '--title', str(title), '--runtime', str(runtime),
                   '--out', str(out)]
            made = subprocess.run(run, capture_output=True, text=True)
            self.assertEqual(made.returncode, 0, made.stderr)
            self.assertEqual((out / 'usb/mihawkos' / f'{digest}.zst').read_bytes(), piece)
            self.assertTrue((out / 'PPSA99170/eboot.bin').is_file() and (out / 'README.txt').is_file())
            checked = subprocess.run(['sha256sum', '-c', '--quiet', 'SHA256SUMS'], cwd=out, capture_output=True, text=True)
            self.assertEqual(checked.returncode, 0, checked.stdout)
            self.assertEqual(len((out / 'SHA256SUMS').read_text().splitlines()), 6)
            (title / 'install-source.txt').write_text('http://fixture/\n')
            self.assertNotEqual(subprocess.run(run, capture_output=True).returncode, 0)
            (title / 'install-source.txt').unlink()
            (runtime / f'{digest}.zst').write_bytes(piece + b'!')
            self.assertNotEqual(subprocess.run(run, capture_output=True).returncode, 0)


if __name__ == '__main__':
    unittest.main()
