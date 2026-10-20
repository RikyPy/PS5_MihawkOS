#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject false shutdown passes: entry, actual close, deadline and every native PID matter."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('console_run', ROOT / 'tools/run-console.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class TelemetryTeardown(unittest.TestCase):
    def setUp(self):
        self.text = ('wine-child: telemetry hold begin pid=11 duration_ms=60000\n'
                     'PS5_PROTON_WINDOW_PROBE: WM_CLOSE received\n'
                     'PS5_PROTON_WINDOW_PROBE: leaving after the window closed\n'
                     'wine-child: payload terminal reason=control-ended\n')
        self.states = [{'pid': 11, 'absent': 1}, {'pid': 12, 'absent': 1}]

    def passed(self, **changes):
        args = dict(pid=11, text=self.text, native_states=self.states, expected_pids=[11, 12], elapsed=20)
        args.update(changes)
        return runner.telemetry_hold_passed(**args)

    def test_observed_shutdown(self):
        self.assertTrue(self.passed())
        self.assertTrue(self.passed(native_states=list(reversed(self.states))))

    def test_entry_and_shutdown_are_required(self):
        self.assertFalse(self.passed(pid=None))
        self.assertFalse(self.passed(pid=12))
        for line in self.text.splitlines(keepends=True):
            with self.subTest(missing=line):
                self.assertFalse(self.passed(text=self.text.replace(line, '')))

    def test_deadline_cannot_be_qualified_by_late_success(self):
        for elapsed in (0, -1, 60, 61, float('nan')):
            self.assertFalse(self.passed(elapsed=elapsed))
        self.assertFalse(self.passed(text=self.text + 'wine-child: telemetry hold expired\n'))

    def test_no_missing_duplicate_or_surviving_helpers(self):
        for states in ([], self.states[:1], self.states + self.states[:1],
                       [{'pid': 11, 'absent': 1}, {'pid': 12, 'absent': 0}],
                       [{'pid': 11, 'absent': 1}, {'pid': 13, 'absent': 1}]):
            self.assertFalse(self.passed(native_states=states))
        self.assertFalse(self.passed(expected_pids=[]))


if __name__ == '__main__':
    unittest.main()
