#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/make-mihawk-wallpaper.py, the desktop's Aero Meadow: the same pixels on every run (it is seeded, and the build
redraws it only when the script changes), the size asked for, sky above and grass below."""
import importlib.util
import unittest
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('mihawk_wallpaper', ROOT / 'tools/make-mihawk-wallpaper.py')
wallpaper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wallpaper)


class MihawkWallpaper(unittest.TestCase):
    def test_deterministic_sized_sky_over_grass(self):
        first, second = wallpaper.wallpaper(320, 180), wallpaper.wallpaper(320, 180)
        self.assertEqual(first.size, (320, 180))
        self.assertEqual(first.mode, 'RGB')
        a = np.asarray(first).astype(int)
        self.assertTrue((a == np.asarray(second)).all())
        top, bottom = a[2:10, 40:120].reshape(-1, 3).mean(0), a[-8:-2, 40:280].reshape(-1, 3).mean(0)
        self.assertGreater(top[2], top[0] + 60)        # blue sky
        self.assertGreater(bottom[1], bottom[2] + 60)  # green grass

    def test_dark_is_the_same_meadow_at_night(self):
        light, dark = np.asarray(wallpaper.wallpaper(320, 180)).astype(int), np.asarray(wallpaper.wallpaper(320, 180, 'dark')).astype(int)
        self.assertLess(dark.mean(), light.mean() / 2)
        top, bottom = dark[2:10, 40:120].reshape(-1, 3).mean(0), dark[-8:-2, 40:280].reshape(-1, 3).mean(0)
        self.assertGreater(top[2], top[0] + 20)        # night blue
        self.assertGreater(bottom[1], bottom[2] + 10)  # dark grass
        self.assertLess(top.max(), 80)


if __name__ == '__main__':
    unittest.main()
