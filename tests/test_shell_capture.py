#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A semantic PASS must not hide missing or visually incorrect shell frames."""
import importlib.util
import math
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from PIL import Image

spec = importlib.util.spec_from_file_location('shell', Path(__file__).resolve().parents[1] / 'tools/run-shell.py')
shell = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shell)
START = (660, 454, 1260, 1020)


class ShellCaptureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.explorer = {theme: shell.pw_explorer_expected.home(theme)[0] for theme in ('light', 'dark')}

    def verdict(self, *, square=False, no_glass=False, preview=(200, 60, 140), no_preview=False, explorer_wrong=False,
                misplaced=False, inactive=False, no_frame=False):
        with tempfile.TemporaryDirectory() as directory:
            root, names, lines = Path(directory), [], []
            beneath = Image.new('RGB', (1920, 1080), (70, 80, 90))
            for x in range(0, 1920, 7):
                for y in range(300, 460):
                    beneath.putpixel((x, y), (200, 40, 40))
            materials = shell.effects.tokens()
            for n, phase in enumerate(shell.PHASES, 1):
                name = f'{n}.png'; names.append(name)
                image = beneath.copy() if phase == 'desktop-light' else Image.new('RGB', (1920, 1080), (32, 32, 32))
                if phase.startswith('start-'):
                    point = (START[0] + 10, START[1] + 10)
                    for role, at, value, _ in shell.start_samples(beneath, START, point, phase.endswith('dark'), materials):
                        colour = tuple(int(math.floor(c + 0.5)) for c in value)
                        if square and role == 'start-corner': colour = (243, 243, 243)
                        image.putpixel(at, colour)
                    if not no_glass:
                        lines.append(f'shell-glass number={n} window={",".join(map(str, START))}')
                    lines.append(f'shell-capture number={n} phase={phase} point={point[0]},{point[1]} expected=0,0,0 result=HOST')
                elif phase == 'preview':
                    image.putpixel((150, 120), preview)
                    if not no_preview:
                        lines.append(f'shell-preview number={n} thumb=100,100,200,140 expected=200,60,140')
                    lines.append(f'shell-capture number={n} phase=preview point=5,5 expected=200,60,140 result=HOST')
                elif phase == 'desktop-light':
                    lines.append(f'shell-capture number={n} phase={phase} point=5,5 expected=0,0,0 result=HOST')
                else:
                    # the prototype's File Explorer on Home, its client inside the 1 px border of the frame at 410,166
                    window = Image.fromarray(self.explorer[phase[9:]].copy())
                    if explorer_wrong and phase == 'explorer-dark':
                        window.putpixel((60, 70), tuple(min(255, c + 9) for c in window.getpixel((60, 70))))  # the Forward button
                    image.paste(window, (411, 167))
                    frame = '400,166,1500,866' if misplaced else '410,166,1510,866'
                    if not (no_frame and phase == 'explorer-light'):
                        lines.append(f'shell-explorer number={n} frame={frame} active={0 if inactive else 1} pointer=1800,100')
                    lines.append(f'shell-capture number={n} phase={phase} point=421,177 expected=0,0,0 result=HOST')
                image.save(root / name)
            with patch.object(shell, 'ROOT', root):
                return shell.display_verdict(names, '\n'.join(lines * 2))[1]

    def test_matching_frames_and_duplicate_log_sinks_pass(self): self.assertTrue(self.verdict())
    def test_explorer_pixel_off_the_prototype_fails(self): self.assertFalse(self.verdict(explorer_wrong=True))
    def test_explorer_away_from_the_prototype_s_place_fails(self): self.assertFalse(self.verdict(misplaced=True))
    def test_inactive_explorer_fails(self): self.assertFalse(self.verdict(inactive=True))
    def test_explorer_without_frame_record_fails(self): self.assertFalse(self.verdict(no_frame=True))
    def test_square_start_corner_fails(self): self.assertFalse(self.verdict(square=True))
    def test_start_without_window_geometry_fails(self): self.assertFalse(self.verdict(no_glass=True))
    def test_wrong_preview_thumbnail_fails(self): self.assertFalse(self.verdict(preview=(10, 10, 10)))
    def test_missing_preview_record_fails(self): self.assertFalse(self.verdict(no_preview=True))


if __name__ == '__main__': unittest.main()
