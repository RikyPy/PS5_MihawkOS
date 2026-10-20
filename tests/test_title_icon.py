#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The title's icon is the desktop's Mihawk mark: ps5/tools/icon0.svg draws themes/windows11/start.svg's three shapes
unchanged (on a white tile), and ps5/sce_sys/icon0.png is that SVG rendered at 512 x 512 as 8-bit RGB, not a stale
picture of an older mark (checked against librsvg when it is installed)."""
import io
import re
import shutil
import subprocess
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SHAPE = re.compile(r'<(?:rect|polyline)\b[^>]*/>')


class TitleIcon(unittest.TestCase):
    def test_svg_draws_the_shell_mark(self):
        mark = SHAPE.findall((ROOT / 'themes/windows11/start.svg').read_text())
        icon = SHAPE.findall((ROOT / 'ps5/tools/icon0.svg').read_text())
        self.assertEqual(len(mark), 3)
        self.assertEqual(icon[1:], mark)  # the first is the white tile
        self.assertIn('fill="#FFFFFF"', icon[0])

    def test_png_is_512_rgb(self):
        image = Image.open(ROOT / 'ps5/sce_sys/icon0.png')
        self.assertEqual((image.size, image.mode), ((512, 512), 'RGB'))

    @unittest.skipUnless(shutil.which('rsvg-convert'), 'librsvg (rsvg-convert) is not installed')
    def test_png_is_the_svg(self):
        png = subprocess.run(['rsvg-convert', '-w', '512', '-h', '512', str(ROOT / 'ps5/tools/icon0.svg')],
                             capture_output=True, check=True).stdout
        want = np.asarray(Image.open(io.BytesIO(png)).convert('RGB')).astype(int)
        have = np.asarray(Image.open(ROOT / 'ps5/sce_sys/icon0.png').convert('RGB')).astype(int)
        error = np.abs(want - have)
        self.assertLessEqual(error.mean(), 0.5)  # another librsvg may antialias an edge a level or two apart
        self.assertLessEqual(int(error.max()), 16)


if __name__ == '__main__':
    unittest.main()
