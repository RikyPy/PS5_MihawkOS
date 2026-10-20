#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Close a hung title with the control payload's kill, after checking that it is hung.

A game that cannot leave (its exit waits for a render thread stuck inside the GPU
driver, or the title's own stop gave up) holds the console until something ends it.
My standing instruction is that a hung title is killed by this tool, not
left for me to close from the home screen. It is a kill: the process ends with
its display live, which has preceded console power-offs, so it only acts on a title
that has clearly stopped making progress:

  - the exit says it has waited MIN_WAIT seconds or more for a thread to release its
    OpenGL context ("waiting for N other thread(s) ... (S s)"), or
  - the title's stop gave up ("leaving it running").

--force kills a title that none of those describe (a hang I judged from the klog),
and says so. A title that is not running is left alone; a healthy one is refused.
tools/run-console.py never kills by itself.

  tools/kill-hung-title.py [--min-wait 120] [--force]
"""
import argparse
import re
import socket
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console


def klog_lines(settings, seconds):
    lines = []
    try:
        connection = socket.create_connection((settings['host'], settings['klog_port']), timeout=5)
    except OSError:
        return lines
    connection.settimeout(2)
    data = b''
    end = time.time() + seconds
    try:
        while time.time() < end:
            chunk = connection.recv(65536)
            if not chunk:
                break
            data += chunk
    except OSError:
        pass
    connection.close()
    return data.decode(errors='replace').splitlines()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--min-wait', type=int, default=120, help='seconds the exit must have waited (default 120)')
    parser.add_argument('--force', action='store_true', help='kill a title the klog does not show as stuck in its exit')
    args = parser.parse_args()
    settings = ps5_console.load_settings()
    state = ps5_console.ps5vkctl_command(settings, 'procs')
    if 'count=0' in state:
        print('nothing is running; nothing to kill')
        return 0
    lines = klog_lines(settings, 8)
    # The live stream only repeats the exit wait; the title's "leaving it running" is printed once, so
    # also read what the last run's capture (klog/<run>.log) holds, if it is recent.
    captures = sorted((path for path in (ROOT / 'klog').glob('*.log') if not path.name.endswith('-wine.log')),
                      key=lambda path: path.stat().st_mtime)
    if captures and time.time() - captures[-1].stat().st_mtime < 3600:
        lines = captures[-1].read_text(errors='replace').splitlines() + lines
    waits = [int(m.group(2)) for line in lines for m in [re.search(r'waiting for (\d+) other thread\(s\) to release their OpenGL context \((\d+) s\)', line)] if m]
    gave_up = any('leaving it running' in line for line in lines)
    reason = None
    if waits and max(waits) >= args.min_wait:
        reason = f'its exit has waited {max(waits)} s for the render thread to release its OpenGL context'
    elif gave_up:
        reason = "the title's stop gave up on it"
    elif args.force:
        reason = 'forced: judged hung from the klog'
    if not reason:
        longest = f'; the longest exit wait seen is {max(waits)} s' if waits else ''
        print(f'not killing: no sign that the title is stuck in its exit{longest}. Use --force for another kind of hang.')
        return 1
    print(f'killing the title: {reason}', flush=True)
    match = re.search(r'title=(\w{9})', state)
    if not match:
        print('the control payload does not name the running title; not killing')
        return 1
    print('payload:', ps5_console.ps5vkctl_command(settings, f'kill {match.group(1)}'))
    for _ in range(20):
        time.sleep(1)
        if 'count=0' in ps5_console.ps5vkctl_command(settings, 'procs'):
            print('the console is idle again')
            return 0
    print('the title is still listed after 20 s')
    return 2


if __name__ == '__main__':
    sys.exit(main())
