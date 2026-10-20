#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Helper scheduling verdicts (tools/helper_sched.py): the lines the loader prints match its formats, every helper got
what it asked for or the run says why, and a census run ends in a C2 row (klog/salvage-2026-10-14/review/plan.md)."""
import importlib.util
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('helper_sched', ROOT / 'tools/helper_sched.py')
sched = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sched)

ENTRY = ('wine-child: sched kind=program mode=rr main_tid=100190 entry=1/700 entry_kernel=10/700 entry_cpus=0xffff '
         'plain=1/700 plain_kernel=10/700 plain_cpus=0xffff service=0 program=700 program_cpus=0 census=0 error=0 '
         'service_status=0 now=1/700 now_kernel=10/700')
MEMORY = ENTRY.replace('kind=program', 'kind=memory').replace('census=0', 'census=1')
WORKER = ('wine-child: sched worker=3/700 worker_kernel=2/700 worker_cpus=0xffff status=0 inherit=3/700 '
          'inherit_kernel=2/700 inherit_cpus=0xffff')
TALLY = 'wine-child: sched program threads set=12 failed=0 last_error=0'
HEADER = ('sched-census: ms=5012 self_cpu=3 self_tid=100200 main_tid=100190 inc_thread_records=4 inc_thread_errno=0 '
          'tids=100190,100200,100201,100202')


def process(pid, comm, prio, cpus='0x1fff', lastcpu=2, ppid=88, policy=1):
    return (f'sched-census: ms=5013 pid={pid} ppid={ppid} comm={comm} stat=3 wmesg=select threads=1 '
            f'first_policy={policy} first_prio={prio} best_rtprio=10/{prio} cpus={cpus} oncpu=-1 lastcpu={lastcpu} '
            f'thread_records=1 thread_errno=0 groups=1 dropped=0 raw600=a186010000000000bc02000000000000')


def group(pid, rtprio='10/700', cpus='0x1fff', threads=1):
    return f'sched-census: ms=5013 pid={pid} group rtprio={rtprio} cpus={cpus} threads={threads}'


LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"\s*')
SPEC = re.compile(r'%(#?)l{0,2}([dsux])')


def c_format(start):
    """The printf format in native/proton_wine_child.c that begins with start, its adjacent literals joined."""
    source = (ROOT / 'native/proton_wine_child.c').read_text()
    at = source.index('"' + start)
    text = ''
    while (match := LITERAL.match(source, at)):
        text += match.group(1)
        at = match.end()
    return text.replace('\\n', '')


def pattern(fmt):
    out, at = '', 0
    for match in SPEC.finditer(fmt):
        out += re.escape(fmt[at:match.start()])
        out += {'d': r'-?\d+', 's': r'\S+', 'u': r'\d+',
                'x': r'(?:0|0x[0-9a-f]+)' if match.group(1) else r'[0-9a-f]+'}[match.group(2)]
        at = match.end()
    return re.compile(out + re.escape(fmt[at:]))


class Formats(unittest.TestCase):
    def test_fixtures_are_what_the_loader_prints(self):
        for start, line in (('wine-child: sched kind=', ENTRY), ('wine-child: sched worker=', WORKER),
                            ('wine-child: sched program threads', TALLY), ('sched-census: ms=%lld self_cpu=', HEADER),
                            ('sched-census: ms=%lld pid=%d ppid=', process(97, 'klogsrv.elf', 700)),
                            ('sched-census: ms=%lld pid=%d group', group(97))):
            with self.subTest(start=start):
                self.assertRegex(line, '^' + pattern(c_format(start)).pattern + '$')


class Scheduled(unittest.TestCase):
    def verdict(self, *lines):
        return sched.scheduled(sched.digest(lines))

    def test_rr_taken(self):
        self.assertEqual(self.verdict(MEMORY, ENTRY, WORKER, TALLY), (True, []))

    def test_nothing_to_check(self):
        self.assertEqual(self.verdict(), (True, []))
        keep = ENTRY.replace('mode=rr', 'mode=keep')
        self.assertEqual(self.verdict(keep, WORKER.replace('worker=3/700', 'worker=1/700'), TALLY), (True, []))

    def test_failures_say_why(self):
        cases = {'refused setting': (ENTRY.replace('error=0', 'error=22'), WORKER),
                 'worker refused': (ENTRY, WORKER.replace('status=0', 'status=1').replace('worker=3/', 'worker=1/')),
                 'kernel kept FIFO': (ENTRY, WORKER.replace('worker_kernel=2/700', 'worker_kernel=10/700')),
                 'kernel view refused': (ENTRY, WORKER.replace('worker_kernel=2/700', 'worker_kernel=-78/-78')),
                 'thread kept it': (ENTRY, WORKER, TALLY.replace('failed=0', 'failed=2'))}
        for name, lines in cases.items():
            with self.subTest(name):
                ok, reasons = self.verdict(*lines)
                self.assertFalse(ok)
                self.assertEqual(len(reasons), 1, reasons)

    def test_each_worker_is_judged_by_its_own_helper(self):
        # A helper whose plain threads are already RR: no class change to see.
        rr_plain = ENTRY.replace('plain=1/700 plain_kernel=10/700', 'plain=3/700 plain_kernel=2/700')
        self.assertEqual(self.verdict(rr_plain, WORKER, ENTRY, WORKER), (True, []))
        kept = WORKER.replace('worker_kernel=2/700', 'worker_kernel=10/700')
        self.assertFalse(self.verdict(rr_plain, WORKER, ENTRY, kept)[0])


class Row(unittest.TestCase):
    def row(self, *lines):
        return sched.c2_row(sched.digest((MEMORY, ENTRY, WORKER, HEADER) + lines))

    def test_no_census(self):
        self.assertEqual(sched.c2_row(sched.digest([ENTRY])), {'row': 'no-census'})
        self.assertEqual(sched.c2_row(sched.digest([process(97, 'klogsrv.elf', 700)])), {'row': 'no-helper'})

    def test_daemons_better(self):
        row = self.row(process(97, 'klogsrv.elf', 600), group(97, '10/600'), process(58, 'SceShellCore', 500, ppid=55))
        self.assertEqual((row['row'], row['env'], row['D']), ('daemons-better', [], 700))

    def test_daemons_equal(self):
        row = self.row(process(97, 'klogsrv.elf', 700), process(58, 'SceShellCore', 500, ppid=55))
        self.assertEqual((row['row'], row['env']), ('daemons-equal', ['WINE_PS5_HELPER_PRIORITY=700,701']))

    def test_daemons_worse(self):
        row = self.row(process(97, 'klogsrv.elf', 700), process(99, 'ftpsrv.elf', 720))
        self.assertEqual((row['row'], row['env'], row['worst']), ('daemons-worse', ['WINE_PS5_HELPER_PRIORITY=700,721'], 720))

    def test_stop_when_nothing_is_worse(self):
        row = self.row(process(94, 'kstuff.elf', 767))
        self.assertEqual((row['row'], row['env']), ('stop', []))

    def test_helpers_are_not_daemons(self):
        row = self.row(process(113, 'SceSpZeroConf', 767, ppid=96), process(97, 'klogsrv.elf', 600))
        self.assertEqual(row['row'], 'daemons-better')

    def test_system_cpus(self):
        # A daemon on CPU 14, helpers on all 16: they keep to 0-12, where only better daemons run.
        row = self.row(process(97, 'klogsrv.elf', 720, cpus='0xc000', lastcpu=14), process(99, 'ftpsrv.elf', 600))
        self.assertEqual((row['row'], row['env'], row['cpu_lever']), ('daemons-better', ['WINE_PS5_HELPER_CPUS=0x1fff'], True))
        # Its last CPU alone puts it there.
        row = self.row(process(97, 'klogsrv.elf', 720, cpus='0', lastcpu=15))
        self.assertEqual(row['env'], ['WINE_PS5_HELPER_CPUS=0x1fff'])
        # Helpers only on the system CPUs: no CPU lever, the priority is the only one.
        system = ENTRY.replace('entry_cpus=0xffff', 'entry_cpus=0xe000')
        row = sched.c2_row(sched.digest([system, process(97, 'klogsrv.elf', 720, cpus='0xc000', lastcpu=14)]))
        self.assertEqual((row['row'], row['env'], row['cpu_lever']),
                         ('daemons-worse', ['WINE_PS5_HELPER_PRIORITY=700,721'], False))
        # A daemon that shares no CPU with the helpers does not count.
        row = sched.c2_row(sched.digest([system, process(97, 'klogsrv.elf', 720, cpus='0x1', lastcpu=0)]))
        self.assertEqual(row['row'], 'daemons-better')

    def test_unknown_and_mixed(self):
        row = self.row(process(59, 'SceShellUI', -3, policy=-3, ppid=55),
                       process(58, 'SceShellCore', 500, ppid=55), group(58, '10/500'), group(58, '10/760'))
        self.assertEqual((row['row'], row['unknown'], row['mixed']), ('undecided', ['SceShellUI'], ['SceShellCore']))
        # A census that read no shared daemon's priority gives no verdict either.
        row = self.row(process(59, 'SceShellUI', -3, policy=-3, ppid=55), process(58, 'SceShellCore', -3, policy=-3, ppid=55))
        self.assertEqual((row['row'], row['unknown']), ('undecided', ['SceShellCore', 'SceShellUI']))
        # A daemon in one kernel class, its other threads worse than D, alone: undecided, not 'the rr default is enough'.
        row = self.row(process(58, 'SceShellCore', 500, ppid=55), group(58, '10/500'), group(58, '10/760'))
        self.assertEqual((row['row'], row['mixed']), ('undecided', ['SceShellCore']))

    def test_the_last_census_counts(self):
        later = process(97, 'klogsrv.elf', 600).replace('ms=5013', 'ms=60013')
        row = self.row(process(97, 'klogsrv.elf', 720), group(97, '10/720'), later)
        self.assertEqual((row['row'], row['daemons'][0]['kernel_views']), ('daemons-better', 0))


class Summary(unittest.TestCase):
    def test_summary(self):
        self.assertIsNone(sched.summary(['wine-child: entry']))
        record = sched.summary([MEMORY, HEADER, process(97, 'klogsrv.elf', 700), group(97), ENTRY, WORKER, TALLY,
                                TALLY.replace('set=12', 'set=30')])
        self.assertEqual(record['program_threads'], {'set': 30, 'failed': 0})
        self.assertEqual((record['census_processes'], record['row']['row'], record['scheduled']['ok']),
                         (1, 'daemons-equal', True))
        self.assertNotIn('helper', record['workers'][0])


if __name__ == '__main__':
    unittest.main()
