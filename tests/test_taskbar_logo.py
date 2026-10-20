#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""src/pw_taskbar_logo.h, the Start button's Mihawk mark at every DPI, against themes/windows11/start.svg: librsvg's
drawing of the SVG at 22 px (96 DPI), 33 and 44 px agrees on average within 1.5 levels, and nine pixels in ten within 8
(measured 2026-10-18: means 1.36, 0.90, 0.91; Chromium's Skia, coarser, is 4.6 at 22 px). The mark is left-right
symmetric to the bit, whether or not librsvg is installed."""
import io
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
PROGRAM = r'''
#include <stdio.h>
#include <stdlib.h>
#include "pw_taskbar_logo.h"
int main(int argc, char **argv)
{
    (void)argc;
    const int extent = atoi(argv[1]);
    uint32_t *pixels = malloc((size_t)extent * extent * 4);
    pw_taskbar_logo(pixels, extent);
    fwrite(pixels, 4, (size_t)extent * extent, stdout);
    return 0;
}
'''


class TaskbarLogo(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        source = Path(cls.folder.name) / 'logo.c'
        source.write_text(PROGRAM)
        cls.binary = Path(cls.folder.name) / 'logo'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        f'-I{ROOT / "src"}', str(source), '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def logo(self, extent):
        """extent x extent x (R, G, B, A), premultiplied."""
        raw = subprocess.run([str(self.binary), str(extent)], capture_output=True, check=True).stdout
        words = np.frombuffer(raw, dtype='<u4').reshape(extent, extent)
        return np.stack([words >> 16 & 255, words >> 8 & 255, words & 255, words >> 24], -1).astype(int)

    def test_symmetric_and_placed(self):
        pixels = self.logo(22)
        self.assertTrue((pixels == pixels[:, ::-1]).all())
        self.assertEqual(pixels[0, 0, 3], 0)                          # outside the squircle's corner
        self.assertEqual(tuple(pixels[5, 11]), (255, 255, 255, 255))  # the white face above the M
        self.assertEqual(tuple(pixels[11, 5]), (14, 43, 70, 255))     # the M's left stem, navy

    @unittest.skipUnless(shutil.which('rsvg-convert'), 'librsvg (rsvg-convert) is not installed')
    def test_matches_the_svg(self):
        for extent in (22, 33, 44):
            png = subprocess.run(['rsvg-convert', '-w', str(extent), '-h', str(extent),
                                  str(ROOT / 'themes/windows11/start.svg')], capture_output=True, check=True).stdout
            straight = np.asarray(Image.open(io.BytesIO(png)).convert('RGBA')).astype(int)
            alpha = straight[..., 3:]
            want = np.concatenate([(straight[..., :3] * alpha + 127) // 255, alpha], -1)
            error = np.abs(self.logo(extent) - want)
            self.assertLessEqual(error.mean(), 1.5, extent)
            self.assertGreaterEqual((error.max(-1) <= 8).mean(), 0.9, extent)


if __name__ == '__main__':
    unittest.main()
