#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/run-carrier-image-probe.py's verdict: release passes only when every segment went back, 0x400000 was taken,
the mode ended without a fatal klog line and its after-checks said what the list control said."""
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('run_carrier', ROOT / 'tools/run-carrier-image-probe.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

AFTER = ['CARRIER thread status=0 value=42', 'CARRIER signal status=0 delivered=1', 'CARRIER load handle_ok=1 status=0']
LIST = ['CARRIER start mode=list pid=7 text=0x200000a10', 'CARRIER carrier before module=0', *AFTER,
        'CARRIER end mode=list']
RELEASE = ['CARRIER start mode=release pid=8 text=0x200000a10', 'CARRIER carrier before module=0',
           'CARRIER unmapped seg=0x400000 status=0 errno=0', 'CARRIER unmapped seg=0x454000 status=0 errno=0',
           *AFTER, 'CARRIER map after-load above start=0x200000000 end=0x20000c000', 'CARRIER released ok=1',
           'CARRIER end mode=release']


class Verdict(unittest.TestCase):
    def test_list(self):
        self.assertTrue(runner.verdict('list', LIST, [], None)['passed'])
        self.assertFalse(runner.verdict('list', LIST[:-1], [], None)['passed'])          # ended early
        self.assertFalse(runner.verdict('list', LIST, ['# signal: 11 (SIGSEGV)'], None)['passed'])
        self.assertFalse(runner.verdict('list', [l.replace('module=0', 'module=-1') for l in LIST], [], None)['passed'])

    def test_release(self):
        control = runner.verdict('list', LIST, [], None)['after']
        self.assertTrue(runner.verdict('release', RELEASE, [], control)['passed'])
        self.assertFalse(runner.verdict('release', RELEASE, [], None)['passed'])         # no control
        changed = [l.replace('delivered=1', 'delivered=0') for l in RELEASE]
        self.assertFalse(runner.verdict('release', changed, [], control)['after_matches_list'])
        refused = [l.replace('seg=0x454000 status=0 errno=0', 'seg=0x454000 status=-1 errno=22') for l in RELEASE]
        self.assertFalse(runner.verdict('release', refused, [], control)['passed'])
        self.assertFalse(runner.verdict('release', [l for l in RELEASE if 'released' not in l], [], control)['passed'])
        landed = RELEASE[:-2] + ['CARRIER map after-load low start=0x400000 end=0x480000'] + RELEASE[-2:]
        self.assertFalse(runner.verdict('release', landed, [], control)['passed'])   # a module in the freed range


class Run32(unittest.TestCase):
    CONTROL = 'CARRIER xo32 control returned cs=0x33 ss=0x3b value=42 want=42'
    LOW = 'CARRIER xo32 low returned cs=0x33 ss=0x3b value=42 want=42'
    END = 'CARRIER end mode=xo32'

    def test_both_return(self):
        lines = ['CARRIER xo32 released=1', self.CONTROL, self.LOW, self.END]
        self.assertTrue(runner.verdict('xo32', lines, [], None)['passed'])
        self.assertFalse(runner.verdict('xo32', lines[:-1], [], None)['passed'])          # ended early
        self.assertFalse(runner.verdict('xo32', [lines[0], self.CONTROL, self.END], [], None)['passed'])

    def test_stop_is_an_answer(self):
        stop = runner.verdict('xo32', ['CARRIER xo32 released=1', 'CARRIER xo32 low enter code=0x400000'],
                              ['mDBG: Sending signal(pid: 9, tid: 1, signo: 0xa0020328)'], None)
        self.assertTrue(stop['stopped'] and not stop['ended'])
        self.assertIn('low enter', stop['last_step'])
        wrong = self.CONTROL.replace('value=42', 'value=41')
        self.assertFalse(runner.verdict('xo32', [wrong, self.LOW, self.END], [], None)['passed'])


class ExecuteOnly(unittest.TestCase):
    def test_stop_is_an_answer(self):
        ran = ['CARRIER xo released=1', 'CARRIER xo anon-read at=0x400000']
        stopped = runner.verdict('xo-text', ran, ['mDBG: Sending signal(pid: 9, tid: 1, signo: 0xa0020328)'], None)
        self.assertTrue(stopped['stopped'] and not stopped['ended'])
        self.assertEqual(stopped['last_step'], 'CARRIER xo anon-read at=0x400000')
        control = ran[:1] + ['CARRIER xo anon-read ok value=0', 'CARRIER end mode=xo-control']
        self.assertTrue(runner.verdict('xo-control', control, [], None)['passed'])
        faulted = control[:1] + ['CARRIER xo anon-read fault signo=11 code=2 addr=0x590000', 'CARRIER end mode=xo-control']
        self.assertFalse(runner.verdict('xo-control', faulted, [], None)['passed'])


if __name__ == '__main__':
    unittest.main()
