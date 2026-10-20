#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The title launches every program with the native graphics DLLs the conformance runners verified (Proton's default
set). They once drifted: the title kept d3d12 builtin, so Wine's own d3d12 met DXVK's DXGI and no Direct3D 12 program
could create a device, while the runners' Direct3D 12 suites passed."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def literal(path, pattern):
    match = re.search(pattern, (ROOT / path).read_text(), re.MULTILINE)
    if not match:
        raise AssertionError(f'{path}: no match for {pattern}')
    return match.group(1)


class DllOverrides(unittest.TestCase):
    def test_title_loads_what_the_runners_verified(self):
        vkd3d = literal('tools/run-vkd3d-tests.py', r"^OVERRIDES = '([^']+)'")
        wine = literal('tools/run-wine-tests.py', r"^DXVK_OVERRIDES = '([^']+)'")
        title = literal('native/proton_wine.cpp', r'default_overrides\[\] = "([^"]+)"')
        self.assertEqual(vkd3d, wine)
        self.assertEqual(title, vkd3d + ';bcrypt=b')
        self.assertIn('d3d12', title.split('=')[0].split(','))


if __name__ == '__main__':
    unittest.main()
