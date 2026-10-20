#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The composed desktop's cost and pacing on the console, scene by scene (docs/WINDOWS11_PLAN.md 5c).

Runs tests/frame-pacing-probe.c in the desktop session (one launch). Each of its display captures ends a phase in the
GPU broker (graphics patch 0019), which writes the phase's compositions (count, mean, maximum, a histogram under 2, 4,
8, 12, 16.7 and 33.3 ms and longer) and the pacing of the composed desktop's presents (src/pw_frame_pacing.h).

Budget, checked on the steady phases (the idle desktop, Start and Quick Settings held open, a window dragged across
the taskbar at 60 Hz) and on the desktop's menu (its opening, and its rows swept by the pointer at 60 Hz): no
composition as long as one of the display's frames, at the rate the broker kept (graphics patch 0029's
"gpu-display early hz=": 8.33 ms at 120 Hz, 16.7 ms at 60 Hz; 0023's rate line when 0029's is missing; 60 Hz when
neither says). The record keeps every phase's numbers; the mean over the steady phases is reported
against the 3 ms of the desktop before the prototype's materials (gate-2026-10-15-01).

Each phase also says why its intervals over 20 ms came (graphics patch 0020, 'slow'): content (the broker looked and
found nothing new: the windows changed no sooner), or the longest of compose, copy (the frame into the display's
buffer), wait (for a free buffer, the display behind) and other. Each sweep's own cadence ('cadence': the periods
between the probe's moves and what each move call took) is set beside it.

  tools/run-frame-pacing.py --name <run>
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STEADY = ('idle', 'start-held', 'quicksettings-held', 'drag', 'menu-opening', 'menu-hover')
PHASE = re.compile(r'gpu-windows phase capture=\S*?-(\d+)\.ppm compose_count=(\d+) compose_avg_us=(\d+) compose_max_us=(\d+) '
                   r'compose_hist=([\d,]+) pacing seconds=([\d.]+) frames=(\d+) hidden=(\d+) fps=([\d.]+) mean=([\d.]+) '
                   r'p50=([\d.]+) p95=([\d.]+) p99=([\d.]+) max=([\d.]+) ms over20=(\d+) over34=(\d+) '
                   r'present_avg=([\d.]+) present_max=([\d.]+) ms')
SLOW = re.compile(r'gpu-windows phase capture=\S*?-(\d+)\.ppm .*? slow content=(\d+) compose=(\d+) copy=(\d+) wait=(\d+) '
                  r'other=(\d+) mean interval=([\d.]+) compose_ms=([\d.]+) copy_ms=([\d.]+) wait_ms=([\d.]+) looks=([\d.]+)')
EARLY_HZ = re.compile(r'gpu-display early hz=(\d+)')
RATE = re.compile(r'gpu-display rate .*?high_frame_rate=(\d)')
CADENCE = re.compile(r'frame-pacing note: ([\w-]+) moves=(\d+) in (\d+) ms period_avg_us=(\d+) period_max_us=(\d+) '
                     r'periods_over20=(\d+) call_avg_us=(\d+) call_max_us=(\d+)')


def phases(log):
    """Each phase the probe ended, named by the probe and measured by the broker, in capture order."""
    names = {int(number): name for number, name in re.findall(r'frame-pacing phase number=(\d+) name=([\w-]+)', log)}
    found = {}
    for m in PHASE.finditer(log):
        number = int(m.group(1))
        found[number] = {
            'phase': names.get(number, str(number)), 'compositions': int(m.group(2)), 'compose_avg_us': int(m.group(3)),
            'compose_max_us': int(m.group(4)), 'compose_hist': [int(v) for v in m.group(5).split(',')],
            'seconds': float(m.group(6)), 'frames': int(m.group(7)), 'fps': float(m.group(9)),
            'interval_ms': {'mean': float(m.group(10)), 'p50': float(m.group(11)), 'p95': float(m.group(12)),
                            'p99': float(m.group(13)), 'max': float(m.group(14))},
            'over_20ms': int(m.group(15)), 'over_34ms': int(m.group(16)),
            'present_ms': {'mean': float(m.group(17)), 'max': float(m.group(18))}}
    for m in SLOW.finditer(log):
        if int(m.group(1)) in found:
            found[int(m.group(1))]['slow'] = {
                **dict(zip(('content', 'compose', 'copy', 'wait', 'other'), (int(v) for v in m.groups()[1:6]))),
                'mean_ms': dict(zip(('interval', 'compose', 'copy', 'wait'), (float(v) for v in m.groups()[6:10]))),
                'mean_idle_looks': float(m.group(11))}
    cadence = {}
    for m in CADENCE.finditer(log):
        cadence.setdefault(m.group(1), {
            'moves': int(m.group(2)), 'ms': int(m.group(3)), 'period_avg_us': int(m.group(4)), 'period_max_us': int(m.group(5)),
            'periods_over_20ms': int(m.group(6)), 'call_avg_us': int(m.group(7)), 'call_max_us': int(m.group(8))})
    for phase in found.values():
        if phase['phase'] in cadence:
            phase['cadence'] = cadence[phase['phase']]
    return [found[k] for k in sorted(found)]


def display_rate(log):
    """The rate the broker's display kept and where it was read: 0029's early line, else 0023's rate line, else 60."""
    early = [int(hz) for hz in EARLY_HZ.findall(log) if int(hz) in (60, 120)]
    if early:
        return early[-1], 'early'
    rate = RATE.findall(log)
    if rate:
        return (120 if rate[-1] == '1' else 60), 'rate'
    return 60, 'assumed'


def frame_budget_us(hz):
    """One of the display's frames: 8,333 us at 120 Hz, 16,667 us at 60 Hz."""
    return round(1000000 / hz)


def verdict(measured, hz=60, source='assumed'):
    steady = [p for p in measured if p['phase'] in STEADY]
    budget = frame_budget_us(hz)
    over = [p['phase'] for p in steady if p['compose_max_us'] >= budget]
    count = sum(p['compositions'] for p in steady)
    mean = round(sum(p['compose_avg_us'] * p['compositions'] for p in steady) / count) if count else 0
    return {'steady_phases': [p['phase'] for p in steady], 'steady_compositions': count, 'steady_compose_avg_us': mean,
            'steady_compose_max_us': max((p['compose_max_us'] for p in steady), default=0),
            'display_hz': hz, 'display_hz_source': source, 'frame_budget_us': budget,
            'phases_over_frame': over, 'baseline_compose_avg_us': 3000,
            'passed': len(steady) == len(STEADY) and not over}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    args = parser.parse_args()
    result = subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--name', args.name,
        '--profile', 'mihawk-os',
        '--args=Z:\\data\\ps5proton\\prefixes\\probe\\drive_c\\frame-pacing64-probe.exe --capture=Z:\\data\\ps5proton\\logs\\game-frame.ppm',
        '--marker', 'PS5_PROTON_FRAME_PACING_PROBE', '--seconds', '240', '--watchdog', '300'], cwd=ROOT)
    path = ROOT / 'evidence' / (args.name + '.json')
    if not path.exists():
        return result.returncode or 1
    record = json.loads(path.read_text())
    log = '\n'.join(log.read_text(errors='replace') for log in (ROOT / 'klog').glob(args.name + '*.log'))
    record['frame_pacing'] = {'phases': phases(log), **verdict(phases(log), *display_rate(log))}
    record['session_passed'] = record['passed']
    record['passed'] = record['passed'] and record['frame_pacing']['passed']
    path.write_text(json.dumps(record, indent=1) + '\n')
    print(json.dumps(record['frame_pacing'], indent=1))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
