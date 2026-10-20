#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The frame-pacing runner's budget is one of the display's frames at the rate the broker kept (tools/run-frame-pacing.py):
8.33 ms at 120 Hz, 16.7 ms at 60 Hz, read from graphics patch 0029's line, else 0023's, else 60 Hz."""
import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('frame_pacing', Path(__file__).resolve().parents[1] / 'tools/run-frame-pacing.py')
frame_pacing = importlib.util.module_from_spec(spec)
spec.loader.exec_module(frame_pacing)


def phase_line(number, compose_max_us, compose_avg_us=2000, count=100):
    return (f'gpu-windows phase capture=/x/game-frame-{number:02d}.ppm compose_count={count} compose_avg_us={compose_avg_us} '
            f'compose_max_us={compose_max_us} compose_hist=1,2,3,4,5,6,7 pacing seconds=2.0 frames=200 hidden=0 fps=100.0 '
            f'mean=10.0 p50=8.5 p95=9.0 p99=12.0 max=20.0 ms over20=0 over34=0 present_avg=1.0 present_max=2.0 ms')


def log_with(rate_lines, slowest_us):
    names = '\n'.join(f'frame-pacing phase number={i + 1} name={name}' for i, name in enumerate(frame_pacing.STEADY))
    phases = '\n'.join(phase_line(i + 1, slowest_us if name == 'drag' else 3000)
                       for i, name in enumerate(frame_pacing.STEADY))
    return '\n'.join([rate_lines, names, phases])


class FramePacingBudget(unittest.TestCase):
    def judge(self, log):
        return frame_pacing.verdict(frame_pacing.phases(log), *frame_pacing.display_rate(log))

    def test_120hz_budget_is_8_33_ms(self):
        log = log_with('gpu-display early hz=120 status=0', 9000)   # under 16.7 ms, over 8.33 ms
        result = self.judge(log)
        self.assertEqual((result['display_hz'], result['display_hz_source'], result['frame_budget_us']), (120, 'early', 8333))
        self.assertEqual(result['phases_over_frame'], ['drag'])
        self.assertFalse(result['passed'])
        self.assertTrue(self.judge(log_with('gpu-display early hz=120 status=0', 8000))['passed'])

    def test_60hz_budget_is_16_7_ms(self):
        result = self.judge(log_with('gpu-display early hz=60 status=0', 9000))
        self.assertEqual((result['display_hz'], result['frame_budget_us']), (60, 16667))
        self.assertTrue(result['passed'])
        self.assertFalse(self.judge(log_with('gpu-display early hz=60 status=0', 16667))['passed'])

    def test_rate_line_and_default(self):
        rate120 = ('gpu-display rate found_us=16682 restored=-1 asked_120hz=1 supported=1 configured=0 period_us=8341 '
                   'high_frame_rate=1')
        self.assertEqual(frame_pacing.display_rate(rate120), (120, 'rate'))
        self.assertEqual(frame_pacing.display_rate(rate120.replace('high_frame_rate=1', 'high_frame_rate=0')), (60, 'rate'))
        self.assertEqual(frame_pacing.display_rate('nothing here'), (60, 'assumed'))
        # 0029's line wins over 0023's, and a display that did not open (hz=0) is not a rate.
        self.assertEqual(frame_pacing.display_rate(rate120.replace('=1', '=0') + '\ngpu-display early hz=120 status=0'),
                         (120, 'early'))
        self.assertEqual(frame_pacing.display_rate('gpu-display early hz=0 status=0x5'), (60, 'assumed'))


if __name__ == '__main__':
    unittest.main()
