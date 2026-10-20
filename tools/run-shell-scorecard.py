#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The shell scorecard: how much of the approved prototype the native shell is, state by state, in one console run.

tests/shell-scorecard-probe.c puts the native shell in every state of the prototype's reference set it can reach, in
light then dark, and captures the display in each. Each capture is compared with its counterpart in the reference set
(.impeccable/reference-1920 of the design worktree, made from bbbf3c6 at brightness 100; manifest.json names each
state and how it was made; it holds the Bloom wallpaper, so it is never committed) over the state's footprint: every
pixel where the state differs from its theme's desktop, in the prototype or in the native shell, so a surface drawn
in the wrong place or at the wrong size counts both places. The desktop itself is compared whole.

Left out of every comparison: the pointer's 32 x 32 sprite (the prototype's captures have none), and live data,
which the console cannot show as the prototype's fixture does: the status and clock buttons (network, sound, battery,
the time and date, the notification count), the notification centre's list and the calendar, File Explorer's Recent
rows and item count. Task Manager's lists and graphs are compared as they are.

Per state: the share of compared pixels within one level (max channel difference), within two, and the worst
difference; or why it has no score: "no native surface" (the shell has no such state), "not driven" (the probe does
not reach it yet) or "did not open" (the probe tried; the record says why). "Like the prototype" is the mean share
within one level over all the reference set's states, a state without a score counting 0; the mean over scored states
says how close what exists is.

  tools/run-shell-scorecard.py --name <run> [--reference DIR]       one console run, then the score
  tools/run-shell-scorecard.py --score <run> [--reference DIR]      score that run's captures again
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_REFERENCE = os.environ.get('PW_REFERENCE_1920', str(ROOT.parent / 'PS5_Proton-design/.impeccable/reference-1920'))

# Every state of the reference set by the surface it belongs to.
GROUPS = {
    'desktop': ['desktop', 'desktop-icon-hover', 'desktop-icon-infotip', 'desktop-icon-selected', 'desktop-rubber-band',
                'desktop-multi-selected', 'desktop-icon-drag', 'desktop-icon-dropped'],
    'taskbar': ['taskbar-start-hover', 'taskbar-start-pressed', 'taskbar-app-hover', 'taskbar-thumbs', 'taskbar-tray',
                'taskbar-left', 'taskbar-progress', 'taskbar-flash', 'jumplist', 'taskbar-menu', 'winx'],
    'start': ['start', 'start-opened-by-click', 'start-pin-hover', 'search'],
    'flyouts': ['quicksettings', 'quicksettings-wifi', 'notifications', 'toast', 'osd', 'osd-brightness', 'emoji', 'clipboard'],
    'menus': ['menu-desktop', 'menu-desktop-item', 'menu-desktop-view-submenu'],
    'window chrome': ['caption-close-hover', 'caption-close-pressed', 'caption-minimize-hover', 'caption-maximize-hover',
                      'windows-active-inactive', 'window-maximized', 'hang'],
    'file explorer': ['explorer'],
    'dialogs': ['run', 'msgbox', 'msgbox-error', 'msgbox-info', 'taskdialog', 'properties', 'properties-drive',
                'properties-folder', 'properties-recycle', 'properties-shortcut', 'openwith', 'fileopen', 'filesave', 'uac',
                'copyprogress', 'copyprogress-details', 'conflict', 'notepad-save-prompt'],
    'apps': ['settings', 'settings-taskbar', 'settings-clipboard', 'notepad', 'calculator', 'terminal', 'terminal-help',
             'taskmgr', 'taskmgr-perf'],
    'controls': ['controls', 'controls-buttons', 'controls-text', 'controls-lists', 'controls-ranges', 'controls-bars',
                 'controls-dates', 'controls-dialog'],
    'snap and switching': ['snaplayouts', 'snap-preview-left', 'snap-assist', 'alttab', 'taskview'],
    'session': ['lock', 'signin', 'restarting', 'shutdown-dialog'],
}
GROUP = {state: group for group, states in GROUPS.items() for state in states}
NO_NATIVE = {
    'quicksettings-wifi': 'Quick Settings has no Wi-Fi page', 'toast': 'no notification toasts',
    'osd': 'no volume indicator', 'osd-brightness': 'no brightness indicator', 'emoji': 'no emoji panel',
    'clipboard': 'no clipboard history', 'taskbar-left': 'no left-aligned taskbar', 'taskbar-progress': 'no button progress',
    'taskbar-flash': 'no button flashing', 'jumplist': 'no taskbar button menu', 'taskbar-menu': 'no taskbar menu',
    'winx': 'no Start button menu', 'hang': 'no "not responding" window', 'uac': 'no elevation prompt',
    'settings-taskbar': 'Settings has no Taskbar page', 'settings-clipboard': 'Settings has no Clipboard page',
    'calculator': 'no Calculator', 'snaplayouts': 'no Snap Layouts', 'snap-preview-left': 'no Snap',
    'snap-assist': 'no Snap Assist', 'alttab': 'no Alt+Tab switcher', 'taskview': 'no Task View', 'lock': 'no lock screen',
    'signin': 'no sign-in screen', 'restarting': 'no restart screen', 'shutdown-dialog': 'no shut down dialog',
    **{state: 'no control gallery' for state in GROUPS['controls']},
}
NOT_DRIVEN = {
    'desktop-icon-drag': 'a drag would move and persist a desktop icon', 'desktop-icon-dropped': 'as desktop-icon-drag',
    'properties-recycle': "the Recycle Bin's properties are not opened yet", 'openwith': 'Open with is not opened yet',
    'copyprogress': 'needs a copy long enough to show progress', 'copyprogress-details': 'as copyprogress',
    'conflict': 'needs a copy onto an existing file', 'terminal-help': "the prototype's terminal is PowerShell's help",
}
# x0, y0, x1, y1 (exclusive) on the 1920 x 1080 screen, from the prototype's own layout at 4e52ee1.
LIVE = [(1713, 1032, 1912, 1080)]  # .tb-status and .tb-clock with its badge
MASKS = {
    'notifications': [(1549, 305, 1907, 561), (1549, 575, 1907, 1019)],  # .nc-list, then .cal-header and .cal-body
    'explorer': [(576, 599, 1509, 839), (423, 844, 489, 860)],            # Home's Recent section, the item count
}
for _state in ('caption-close-hover', 'caption-close-pressed', 'caption-minimize-hover', 'caption-maximize-hover'):
    MASKS[_state] = MASKS['explorer']
CAPTURE = re.compile(r'shell-scorecard capture=(\d+) state=([\w-]+) theme=(\w+) pointer=(-?\d+),(-?\d+)')
# Wine's debug channel quotes the line: OutputDebugStringA "shell-scorecard missing ... reason=R\n"
MISSING = re.compile(r'shell-scorecard missing state=([\w-]+) theme=(\w+) reason=(.*?)(?:\\n"|\n|$)', re.M)


def masked(state, pointers):
    """The pixels a state's comparison leaves out: live data and every pointer sprite in its captures."""
    mask = np.zeros((1080, 1920), bool)
    for x0, y0, x1, y1 in LIVE + MASKS.get(state, []):
        mask[y0:y1, x0:x1] = True
    for x, y in pointers:
        mask[max(0, y - 1):max(0, y + 31), max(0, x - 1):max(0, x + 31)] = True
    return mask


def score(native, reference, native_desktop, reference_desktop, mask, whole=False):
    """Agreement of a native capture with the prototype's (RGB uint8 arrays, 1080 x 1920) over the state's footprint."""
    n, r = native.astype(np.int16), reference.astype(np.int16)
    error = np.abs(n - r).max(-1)
    if whole:
        region = ~mask
    else:
        region = (np.abs(r - reference_desktop.astype(np.int16)).max(-1) > 1) | \
                 (np.abs(n - native_desktop.astype(np.int16)).max(-1) > 1)
        region &= ~mask
    compared = int(region.sum())
    if not compared:
        return {'compared': 0, 'within_1': 0.0, 'within_2': 0.0, 'worst': None}
    picked = error[region]
    return {'compared': compared, 'within_1': round(float((picked <= 1).mean()) * 100, 1),
            'within_2': round(float((picked <= 2).mean()) * 100, 1), 'worst': int(picked.max())}


def relative(path):
    path = Path(path).resolve()
    return str(path.relative_to(ROOT)) if path.is_relative_to(ROOT) else str(path)


def rgb(path):
    with Image.open(path) as image:
        return np.asarray(image.convert('RGB'))


def scorecard(log, captures, reference):
    """Every reference state's record and the summary, from the probe's log and the captures (number: path)."""
    manifest = json.loads((reference / 'manifest.json').read_text())
    shots = {(m[1], m[2]): (int(m[0]), (int(m[3]), int(m[4]))) for m in CAPTURE.findall(log)}
    absent = {(m[0], m[1]): m[2].strip() for m in MISSING.findall(log)}
    desktops, references = {}, {}
    for theme in ('light', 'dark'):
        number, pointer = shots.get(('desktop', theme), (None, None))
        if number in captures:
            desktops[theme] = (rgb(captures[number]), pointer)
            references[theme] = rgb(reference / theme / 'desktop.png')
    states = []
    for entry in manifest['states']:
        state, theme = entry['name'], entry['theme']
        record = {'state': state, 'theme': theme, 'group': GROUP.get(state, 'other')}
        if (state, theme) in shots and shots[(state, theme)][0] in captures and theme in desktops:
            number, pointer = shots[(state, theme)]
            native_desktop, desktop_pointer = desktops[theme]
            result = score(rgb(captures[number]), rgb(reference / entry['file']), native_desktop, references[theme],
                           masked(state, [pointer, desktop_pointer]), whole=state == 'desktop')
            record.update(status='scored', capture=relative(captures[number]), pointer=list(pointer), **result)
        elif (state, theme) in absent:
            record.update(status='did not open', reason=absent[(state, theme)])
        elif state in NO_NATIVE:
            record.update(status='no native surface', reason=NO_NATIVE[state])
        elif state in NOT_DRIVEN:
            record.update(status='not driven', reason=NOT_DRIVEN[state])
        else:
            record.update(status='not captured')
        states.append(record)
    return {'reference': manifest.get('commit'), 'states': states, **summary(states)}


def summary(states):
    def mean(values):
        values = list(values)
        return round(sum(values) / len(values), 1) if values else 0.0
    scored = [s for s in states if s['status'] == 'scored']
    groups = {}
    for group in GROUPS:
        members = [s for s in states if s['group'] == group]
        groups[group] = {'states': len(members), 'scored': sum(s['status'] == 'scored' for s in members),
                         'like_prototype': mean(s.get('within_1', 0.0) if s['status'] == 'scored' else 0.0 for s in members),
                         'scored_mean': mean(s['within_1'] for s in members if s['status'] == 'scored')}
    # What is furthest from the prototype, by how many states' worth of pixels it lacks.
    backlog = sorted(groups, key=lambda g: -(groups[g]['states'] * (100 - groups[g]['like_prototype'])))
    return {'like_prototype': mean(s.get('within_1', 0.0) if s['status'] == 'scored' else 0.0 for s in states),
            'scored_mean': mean(s['within_1'] for s in scored), 'scored': len(scored), 'total': len(states),
            'by_status': {status: sum(s['status'] == status for s in states) for status in
                          ('scored', 'did not open', 'not driven', 'no native surface', 'not captured')},
            'groups': groups, 'backlog': backlog}


def markdown(card):
    lines = [f"Like the prototype: **{card['like_prototype']}%** of pixels within one level over all {card['total']} "
             f"reference states ({card['scored']} scored, mean {card['scored_mean']}% over those).", '',
             '| Surface | States scored | Like the prototype | Mean of scored |', '| --- | --- | --- | --- |']
    for group in card['backlog']:
        g = card['groups'][group]
        lines.append(f"| {group} | {g['scored']}/{g['states']} | {g['like_prototype']}% | {g['scored_mean']}% |")
    lines += ['', '| State | Light within 1 / worst | Dark within 1 / worst |', '| --- | --- | --- |']
    by = {(s['state'], s['theme']): s for s in card['states']}
    for state in dict.fromkeys(s['state'] for s in card['states']):
        cells = []
        for theme in ('light', 'dark'):
            s = by.get((state, theme), {'status': 'not captured'})
            cells.append(f"{s['within_1']}% / {s['worst']}" if s['status'] == 'scored' else s['status'])
        lines.append(f'| {state} | {cells[0]} | {cells[1]} |')
    return '\n'.join(lines)


def run_files(name):
    log = '\n'.join(path.read_text(errors='replace') for path in sorted((ROOT / 'klog').glob(name + '*.log')))
    captures = {int(m.group(1)): path for path in (ROOT / 'klog').glob(name + '-capture-*.png')
                if (m := re.search(r'-capture-(\d+)\.png$', path.name))}
    return log, captures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    which = parser.add_mutually_exclusive_group(required=True)
    which.add_argument('--name', help='run the probe on the console under this run name, then score it')
    which.add_argument('--score', help="score this earlier run's captures again")
    parser.add_argument('--reference', type=Path, default=Path(DEFAULT_REFERENCE))
    args = parser.parse_args()
    if not (args.reference / 'manifest.json').is_file():
        parser.error(f'no reference set at {args.reference} (PW_REFERENCE_1920 or --reference)')
    name = args.name or args.score
    if args.name:
        subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'), '--name', name, '--profile', 'mihawk-os',
            '--args=Z:\\data\\ps5proton\\prefixes\\probe\\drive_c\\shell-scorecard64-probe.exe --capture=Z:\\data\\ps5proton\\logs\\game-frame.ppm',
            '--marker', 'PS5_PROTON_SHELL_SCORECARD', '--seconds', '900', '--watchdog', '1000'], cwd=ROOT)
    path = ROOT / 'evidence' / (name + '.json')
    if not path.exists():
        print(f'no record {path}', file=sys.stderr)
        return 1
    record = json.loads(path.read_text())
    log, captures = run_files(name)
    card = scorecard(log, captures, args.reference)
    record['scorecard'] = card
    path.write_text(json.dumps(record, indent=1) + '\n')
    print(markdown(card))
    return 0 if card['scored'] else 1


if __name__ == '__main__':
    sys.exit(main())
