#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Saved click assignments must not silently turn into the old touchpad mapping."""
import importlib.util
from pathlib import Path
import unittest
spec = importlib.util.spec_from_file_location('desktop_input', Path(__file__).resolve().parents[1] / 'tools/run-desktop-input.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

class MappingTests(unittest.TestCase):
    def test_button_mapping_preserves_pointer_and_timing(self):
        self.assertEqual(runner.mapped_script('anchor frame\n1 touch 100 200 0.2\n3 touchpad\n', 'l3'),
                         'anchor frame\n1 touch 100 200 0.2\n3 l3\n')
    def test_trigger_mapping_uses_real_axis(self):
        self.assertEqual(runner.mapped_script('3 touchpad\n4 touchpad\n', 'r2'), '3 r2 1 0.2\n4 r2 1 0.2\n')
    def test_unassigned_click_cannot_fake_a_pass(self):
        with self.assertRaises(ValueError): runner.mapped_script('3 touchpad\n', 'none')

if __name__ == '__main__': unittest.main()
