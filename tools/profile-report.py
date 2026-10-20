#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarise a helper's profile (docs/PROFILING.md) from a run's logs: where the threads were, what they waited in, what the
file and section counters cost, where the telemetry thread's own time went, and how long the game went without a frame.

  tools/profile-report.py <run> [--pid N]       reads klog/<run>-wine-child-<pid>.log and klog/<run>-gpu-broker.log

Without --pid it takes the helper log with the most profile lines. Nothing is written; the verdict lines are plain text so a
run's report can be pasted into docs/CONSOLE_QUEUE.md.
"""
import argparse
import collections
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
THREAD = re.compile(r'wine-child: (?:thread|cpu) tid=(\d+)(?: delta_ms=\d+ total_ms=\d+)? samples=(\d+) in_call_stub=(\d+) signal_blocked=(\d+)')
HOT = re.compile(r'wine-child: hot rip=(0x[0-9a-f]+) n=(\d+) tid=(\d+)')
NT = re.compile(r'wine-child: nt tid=(\d+) call=(0x[0-9a-f]+) n=(\d+)')
CENSUS = re.compile(r'wine-child: census seconds=(\d+) elapsed_ms=(\d+) files=(\d+) dirs=\d+ slept_ms=(\d+) sampler_ms=(\d+) other_ms=(\d+)')
CENSUS_MS = re.compile(r'wine-child: census_ms fds=(\d+) image_stats=(\d+) vm_census=(\d+)')
FILE = re.compile(r'wine-child: file (\w+) calls=(\d+) ms=(\d+) slowest_ms=(\d+) KiB=(\d+)')
SECTION = re.compile(r'wine-child: section views=(\d+) fault_calls=(\d+) fault_ms=(\d+) mapped=(\d+) evicted=(\d+) failures=(\d+) '
                     r'peak=(\d+) cached=(\d+) map_calls=(\d+) map_ms=(\d+) flush_calls=(\d+) flush_ms=(\d+) unmap_calls=(\d+) '
                     r'unmap_ms=(\d+) reserve_calls=(\d+) reserve_ms=(\d+)')
PACING = re.compile(r'gpu-pacing .*?seconds=([\d.]+) frames=(\d+) .*?fps=([\d.]+).*?max=([\d.]+) ms')


def syscall_names():
    """Windows system call number -> name, from the Wine tree's table (ALL_SYSCALLS64), when it is there."""
    names = {}
    for tree in (ROOT / '.deps/wine-ps5/source', ROOT / '.deps/wine-proton'):
        table = tree / 'dlls/ntdll/ntsyscalls.h'
        if not table.exists():
            continue
        text = table.read_text()
        start = text.find('ALL_SYSCALLS64')
        for match in re.finditer(r'SYSCALL_ENTRY\w*\(\s*(0x[0-9a-f]+),\s*(\w+),', text[start:]):
            names[int(match.group(1), 16)] = match.group(2)
        break
    return names


def module_map(lines):
    """Native module text bases printed by the loader: 'payload module=ntdll.prx text=800478000 bytes=606208'."""
    found = []
    for line in lines:
        match = re.search(r'payload module=(\S+) text=([0-9a-f]+) bytes=(\d+)', line)
        if match and int(match.group(3)) > 0:
            found.append((int(match.group(2), 16), int(match.group(3)), match.group(1)))
    return sorted(found)


def where(address, modules):
    for base, size, name in modules:
        if base <= address < base + size:
            return '%s+%#x' % (name, address - base)
    if 0x910000000 <= address < 0x91fffffff:
        return 'guest eboot'
    if 0x900000000 <= address < 0x90fffffff:
        return 'guest libs'
    if address >= 0xdfff000000:
        return 'wine pe dlls'
    if 0x140000000 <= address < 0x150000000:
        return 'game exe'
    return 'other'


def pick(klog, run, pid):
    logs = sorted(Path(klog).glob('%s-wine-child-*.log' % run))
    if pid:
        logs = [log for log in logs if log.name.endswith('-%s.log' % pid)]
    best, best_count = None, -1
    for log in logs:
        count = sum(1 for line in log.open(errors='replace') if THREAD.match(line))
        if count > best_count:
            best, best_count = log, count
    return best


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('run')
    parser.add_argument('--pid')
    parser.add_argument('--klog', default=str(ROOT / 'klog'), help='the folder of run logs')
    args = parser.parse_args()
    log = pick(args.klog, args.run, args.pid)
    if not log:
        print('no helper log for %s' % args.run, file=sys.stderr)
        return 2
    lines = log.read_text(errors='replace').splitlines()
    print('helper log: %s (%d lines)' % (log.name, len(lines)))

    threads = collections.defaultdict(lambda: [0, 0, 0])
    hot = collections.Counter()
    hot_pages = collections.Counter()
    nt = collections.Counter()
    census = []
    census_ms = []
    files = {}
    section = None
    modules = module_map(lines)
    names = syscall_names()
    for line in lines:
        if match := THREAD.match(line):
            t = threads[match.group(1)]
            t[0] += int(match.group(2)); t[1] += int(match.group(3)); t[2] += int(match.group(4))
        elif match := HOT.match(line):
            address = int(match.group(1), 16)
            hot[(match.group(3), where(address, modules))] += int(match.group(2))
            hot_pages[address >> 12] += int(match.group(2))
        elif match := NT.match(line):
            nt[(match.group(1), int(match.group(2), 16))] += int(match.group(3))
        elif match := CENSUS.match(line):
            census.append(tuple(int(v) for v in match.groups()))
        elif match := CENSUS_MS.match(line):
            census_ms.append(tuple(int(v) for v in match.groups()))
        elif match := FILE.match(line):
            files[match.group(1)] = tuple(int(v) for v in match.groups()[1:])
        elif match := SECTION.match(line):
            section = tuple(int(v) for v in match.groups())

    total = sum(t[0] for t in threads.values())
    waiting = sum(t[1] for t in threads.values())
    if total:
        print('\nthreads: %d, samples %d, in call stubs %d (%.1f %%), signal-blocked %d' %
              (len(threads), total, waiting, 100.0 * waiting / total, sum(t[2] for t in threads.values())))
        busiest = sorted(threads.items(), key=lambda item: item[1][0] - item[1][1], reverse=True)[:5]
        for tid, (samples, stub, blocked) in busiest:
            print('  busiest tid %s: %d of %d samples running (%.0f %%)' % (tid, samples - stub, samples,
                                                                       100.0 * (samples - stub) / max(samples, 1)))
        print('  code the threads ran (64-byte buckets, top 6 by place):')
        for (tid, place), count in hot.most_common(6):
            print('    %6d  tid %s  %s' % (count, tid, place))
    else:
        print('\nno thread samples (run with --env WINE_PS5_SAMPLE=1)')
    if nt:
        print('\nWindows calls the threads were inside (top 8 by samples, all threads):')
        by_call = collections.Counter()
        for (tid, call), count in nt.items():
            by_call[call & 0xfff] += count
        for call, count in by_call.most_common(8):
            print('    %6d  %s (%#x)' % (count, names.get(call, '?'), call))
    if census:
        slow = [c for c in census if c[5] > 1000]
        print('\ntelemetry thread: %d periods, longest other_ms %d, %d periods over 1 s of work%s' %
              (len(census), max(c[5] for c in census), len(slow),
               ' (the census itself costs the game: see WINE_PS5_CENSUS_MS)' if slow else ''))
    if census_ms:
        print('  census phases: fds %d ms, image stats %d ms, vm walk %d ms (largest)' %
              tuple(max(c[i] for c in census_ms) for i in range(3)))
    if files:
        print('\nfile calls (totals):')
        for kind, (calls, ms, slowest, kib) in files.items():
            print('  %-12s %8d calls %8d ms (slowest %d ms) %9d KiB' % (kind, calls, ms, slowest, kib))
    if section:
        (views, fault_calls, fault_ms, mapped, evicted, failures, peak, cached, map_calls, map_ms, flush_calls, flush_ms,
         unmap_calls, unmap_ms, reserve_calls, reserve_ms) = section
        print('\nsection cache: %d views, %d fault calls (%d ms) mapped %d pages, evicted %d, failures %d, peak %d, cached %d' %
              (views, fault_calls, fault_ms, mapped, evicted, failures, peak, cached))
        print('  native: map %d calls %d ms, flush %d calls %d ms, unmap %d calls %d ms, reserve %d calls %d ms' %
              (map_calls, map_ms, flush_calls, flush_ms, unmap_calls, unmap_ms, reserve_calls, reserve_ms))
        if mapped and evicted * 2 > mapped:
            print('  verdict: more than half of the mapped pages were evicted again: the cache is too small for the working set')
    broker = Path(args.klog) / ('%s-gpu-broker.log' % args.run)
    if broker.exists():
        windows = [match.groups() for match in map(PACING.search, broker.read_text(errors='replace').splitlines()) if match]
        if windows:
            print('\nframes per pacing window (seconds, frames, fps, longest gap ms):')
            gap_seconds = 0.0
            for seconds, frames, fps, longest in windows:
                print('  %6s s %6s frames %6s fps  longest gap %s ms' % (seconds, frames, fps, longest))
                if float(longest) > 1000:
                    gap_seconds += float(longest) / 1000
            print('  gaps over 1 s add up to about %.0f s' % gap_seconds)
    return 0


if __name__ == '__main__':
    sys.exit(main())
