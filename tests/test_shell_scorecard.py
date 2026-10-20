#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/run-shell-scorecard.py: a state's score counts its footprint in both shells, leaves out live data and the
pointer, and every state the probe reaches belongs to a surface and is not also listed as unreachable."""
import importlib.util
import json
import re
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('scorecard', ROOT / 'tools/run-shell-scorecard.py')
card = importlib.util.module_from_spec(spec)
spec.loader.exec_module(card)


def desktop():
    image = np.zeros((1080, 1920, 3), np.uint8)
    image[..., 0] = np.arange(1920) % 200
    image[..., 2] = (np.arange(1080) % 150)[:, None]
    image[1032:] = (238, 238, 240)
    return image


def with_box(image, x, y, w, h, colour):
    image = image.copy()
    image[y:y + h, x:x + w] = colour
    return image


class Scorecard(unittest.TestCase):
    def run_card(self, native_start, log_extra='', clock=False):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            reference, captures = root / 'reference', {}
            states = []
            for theme in ('light', 'dark'):
                (reference / theme).mkdir(parents=True)
                Image.fromarray(desktop()).save(reference / theme / 'desktop.png')
                Image.fromarray(with_box(desktop(), 660, 454, 600, 566, (243, 243, 243))).save(reference / theme / 'start.png')
                Image.fromarray(with_box(desktop(), 100, 900, 300, 100, (40, 40, 40))).save(reference / theme / 'toast.png')
                states += [{'theme': theme, 'name': name, 'file': f'{theme}/{name}.png'} for name in ('desktop', 'start', 'toast')]
            (reference / 'manifest.json').write_text(json.dumps({'commit': 'test', 'states': states}))
            native_desktop = desktop()
            if clock:
                native_desktop = with_box(native_desktop, 1800, 1040, 60, 30, (0, 0, 0))  # another time of day
            lines = []
            for number, (state, theme, image) in enumerate([('desktop', 'light', native_desktop), ('start', 'light', native_start),
                                                            ('desktop', 'dark', native_desktop)], 1):
                image = with_box(image, 1910, 8, 10, 20, (255, 255, 255))  # the pointer, parked
                captures[number] = root / f'capture-{number}.png'
                Image.fromarray(image).save(captures[number])
                lines.append(f'shell-scorecard capture={number} state={state} theme={theme} pointer=1910,8')
            return card.scorecard('\n'.join(lines) + log_extra, captures, reference)

    def record(self, result, state, theme):
        return next(s for s in result['states'] if s['state'] == state and s['theme'] == theme)

    def test_the_same_pixels_score_100(self):
        result = self.run_card(with_box(desktop(), 660, 454, 600, 566, (243, 243, 243)), clock=True)
        start, top = self.record(result, 'start', 'light'), self.record(result, 'desktop', 'light')
        self.assertEqual((start['status'], start['within_1'], start['worst']), ('scored', 100.0, 0))
        self.assertEqual(start['compared'], 600 * 566)
        self.assertEqual((top['within_1'], top['compared']), (100.0, 1920 * 1080 - 199 * 48 - 11 * 32))  # clock and pointer (clipped at the edge) left out

    def test_a_misplaced_surface_counts_both_places(self):
        result = self.run_card(with_box(desktop(), 700, 454, 600, 566, (243, 243, 243)))
        start = self.record(result, 'start', 'light')
        self.assertEqual(start['compared'], 640 * 566)
        self.assertAlmostEqual(start['within_1'], round(560 / 640 * 100, 1))

    def test_unreached_states_score_0_and_say_why(self):
        result = self.run_card(desktop(), '\nshell-scorecard missing state=start theme=dark reason=Start did not open')
        self.assertEqual(self.record(result, 'start', 'dark')['status'], 'did not open')
        self.assertEqual(self.record(result, 'toast', 'light')['status'], 'no native surface')
        self.assertEqual(result['scored'], 3)
        self.assertEqual(result['total'], 6)
        native_start = self.record(result, 'start', 'light')['within_1']
        self.assertAlmostEqual(result['like_prototype'], round((100 + native_start + 100) / 6, 1))

    def test_every_probe_state_has_a_surface_and_a_route(self):
        probe = (ROOT / 'tests/shell-scorecard-probe.c').read_text()
        reached = set(re.findall(r'(?:shot|absent|flyout|dialog)\("([\w-]+)"', probe)) | \
            set(re.findall(r'\{"(caption-[\w-]+)", \d+\}', probe))
        reached.discard('all')
        self.assertGreater(len(reached), 40)
        for state in reached:
            self.assertIn(state, card.GROUP, state)
            self.assertNotIn(state, card.NO_NATIVE, state)
            self.assertNotIn(state, card.NOT_DRIVEN, state)
        self.assertEqual(len(card.GROUP), 86)


if __name__ == '__main__':
    unittest.main()
