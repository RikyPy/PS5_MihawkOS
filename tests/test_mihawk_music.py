#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/make-mihawk-music.py, MihawkOS's own music: rendered again, every piece is the same samples and the same OGG
bytes as the copy the title ships (ps5/assets/music, music.json's hashes), and it still loops at -18 LUFS."""
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHIPPED = ROOT / 'ps5/assets/music'


class MihawkMusic(unittest.TestCase):
    def test_rendered_again_byte_for_byte(self):
        shipped = json.loads((SHIPPED / 'music.json').read_text())
        with tempfile.TemporaryDirectory() as out:
            subprocess.run([sys.executable, str(ROOT / 'tools/make-mihawk-music.py'), out], check=True,
                           stdout=subprocess.DEVNULL)
            again = json.loads((Path(out) / 'music.json').read_text())
            self.assertEqual(sorted(again['pieces']), ['bubble-garden', 'glass-meadow', 'sky-lounge'])
            for name, piece in again['pieces'].items():
                self.assertEqual(piece, shipped['pieces'][name], name)
                ogg = (Path(out) / (name + '.ogg')).read_bytes()
                self.assertEqual(ogg, (SHIPPED / (name + '.ogg')).read_bytes(), name)
                self.assertEqual(hashlib.sha256(ogg).hexdigest(), piece['ogg_sha256'])
                self.assertAlmostEqual(piece['lufs'], -18.0, delta=0.1)
                self.assertLess(piece['peak_dbfs'], -1.0)
                self.assertTrue(60 <= piece['seconds'] <= 120)


if __name__ == '__main__':
    unittest.main()
