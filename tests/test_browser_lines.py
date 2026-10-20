#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/browser_lines.py: MihawkBrowser's own lines (src/pw_browser.c) into a run's record and verdict. The fixture
lines are the formats the source prints, and the source is checked to still print them."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import browser_lines  # noqa: E402

SHA = '7d3654531c32d941b8cae81c4137fc542172bfa9635f169cb392f245a0a12bcb'
RUN = [
    'browser: process type=browser pid=32', 'browser: window ms=654', 'browser: created ms=666',
    'browser: process type=utility service=storage.mojom.StorageService pid=396',
    'browser: load scheme=https host=example.com path=af63a24c860189fe status=200 ms=45 at=1936',
    f'browser: download done scheme=https host=downloads.example.org path=073db0c6cdbbd144 bytes=2380800 ms=288 sha256={SHA}',
    'browser: handoff child=720 window_ms=146 exiting', 'browser: exit ms=4600',
]
SCRIPT = 'load https://example.com/; download https://x/ExampleSetup.exe; open; exit'


class Summary(unittest.TestCase):
    def test_source_prints_the_lines(self):
        source = (ROOT / 'src/pw_browser.c').read_text()
        for fragment in ('"load %s status=%d ms=%llu at=%llu"', '"download done %s bytes=%lld ms=%llu sha256=%s"',
                         '"handoff child=%lu window_ms=%llu exiting"', '"process type=%ls%s%ls pid=%lu"',
                         '"load-error code=%d %s ms=%llu"', '"step timeout wait=%d"', '"window ms=%llu"'):
            self.assertIn(fragment, source)

    def test_a_passing_run(self):
        record = browser_lines.summary(RUN, SCRIPT, SHA)
        self.assertTrue(record['passed'])
        self.assertEqual(record['startup'], {'window_ms': 654, 'browser_created_ms': 666, 'first_load_at_ms': 1936})
        self.assertEqual(record['download']['bytes'], 2380800)
        self.assertEqual(record['handoff'], {'window_ms': 146, 'action': 'exiting'})
        self.assertEqual(record['process_types'], ['browser', 'utility storage.mojom.StorageService'])

    def test_what_fails_a_run(self):
        self.assertFalse(browser_lines.summary(RUN, SCRIPT, '0' * 64)['passed'])  # another file
        self.assertFalse(browser_lines.summary([l for l in RUN if 'handoff' not in l], SCRIPT, SHA)['passed'])
        self.assertFalse(browser_lines.summary(RUN + ['browser: step timeout wait=1'], SCRIPT, SHA)['passed'])
        error = 'browser: load-error code=-105 scheme=https host=nowhere.example path=0 ms=3'
        self.assertFalse(browser_lines.summary(RUN + [error], SCRIPT, SHA)['passed'])
        self.assertFalse(browser_lines.summary([l.replace('status=200', 'status=404') for l in RUN], SCRIPT, SHA)['passed'])
        self.assertFalse(browser_lines.summary([], 'load https://example.com/', SHA)['passed'])

    def test_a_run_without_download_or_open(self):
        lines = [l for l in RUN if 'download' not in l and 'handoff' not in l]
        self.assertTrue(browser_lines.summary(lines, 'load https://example.com/; wait 1000; exit', SHA)['passed'])


if __name__ == '__main__':
    unittest.main()
