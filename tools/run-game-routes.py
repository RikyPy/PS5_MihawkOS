#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The same installed game in both presentation routes, compared (docs/PLAN.md, priority 1).

  tools/run-game-routes.py --name RUN --profile 1 [--seconds 90]

Runs the catalog game twice through tools/run-console.py, idle at whatever it shows first (no input):

  launcher  its catalog entry, as the launcher starts it: the broker flips the game's own buffer (direct)
  desktop   inside the Mihawk OS session (the `mihawk` prefix every game lives in), started through its own desktop
            shortcut, C:\\users\\Public\\Desktop\\<name>.lnk, which the title wrote (src/pw_shell_link.h), with
            nothing added to the session: the title gives the game its catalog settings when it starts it
            (src/pw_app_environment.h). The broker composes the desktop (composed), or, full-screen in game mode,
            flips the game's frames itself (independent, src/pw_window_direct.h). --start-exe starts the
            executable directly instead (cmd /c start /d), with the entry's environment as one-shot overrides,
            as before the shortcuts

and compares the broker's pacing record of the game's GPU client in each (src/pw_frame_pacing.h). The desktop run
creates the game's own first-run settings in the `mihawk` prefix if it has none; it never touches the game's own
prefix or its saves. Writes evidence/RUN.json with both summaries and the verdict:

  desktop_slower  the composed route lost more than 5 % of the launcher's frames per second, or its worst-window
                  p95 interval is more than 2 ms longer, or its share of intervals over 20 ms is more than one
                  percentage point higher. These thresholds are this tool's, chosen before the first run.
"""
import argparse
import importlib.util
import io
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402

spec = importlib.util.spec_from_file_location('run_console', ROOT / 'tools/run-console.py')
console = importlib.util.module_from_spec(spec)
spec.loader.exec_module(console)


def catalog_entry(profile):
    """The entry's game.json, read from the console (never stored)."""
    with console.connect(ps5_console.load_settings()) as ftp:
        raw = io.BytesIO()
        ftp.retrbinary(f'RETR /data/ps5proton/games/{profile}/game.json', raw.write)
    return json.loads(raw.getvalue())


def words_text(words):
    """Words as the title splits them back (src/pw_wine_launch.h): quoted when empty or holding a space."""
    return ' '.join(f'"{w}"' if not w or re.search(r'\s', w) else w for w in words)


def shortcut_name(name):
    """The file name the title gives a game's shortcut (src/pw_shell_link.c pw_shell_link_file_name)."""
    kept = ''.join(c for c in name if ord(c) >= 0x20 and c not in '<>:"/\\|?*').rstrip(' .')
    base, dot, rest = kept.partition('.')
    if re.fullmatch(r'(?i)(con|prn|aux|nul|com[1-9]|lpt[1-9]|conin\$|conout\$)', base.rstrip(' ')):
        kept = base.rstrip(' ') + ' (game)' + dot + rest
    return kept + '.lnk'


def shortcut_arguments(game):
    link = 'C:\\users\\Public\\Desktop\\' + shortcut_name(game.get('name') or game['executable'].rsplit('\\', 1)[-1])
    return words_text(['C:\\windows\\system32\\cmd.exe', '/c', 'start', f"{game.get('name', 'game')} desktop route", link])


def desktop_arguments(game):
    executable = game['executable']
    directory = game.get('directory') or executable.rsplit('\\', 1)[0]
    title = f"{game.get('name', 'game')} desktop route"
    words = ['C:\\windows\\system32\\cmd.exe', '/c', 'start', title, '/d', directory, executable]
    arguments = game.get('arguments', '')
    return words_text(words) + (' ' + arguments if arguments else '')


def game_client(record, routes):
    """The client that presented the most frames in these routes: the game."""
    clients = [c for c in ((record.get('pacing') or {}).get('clients') or {}).values() if c['route'] in routes]
    return max(clients, key=lambda c: c['frames'], default=None)


def overhead(name, executable):
    """What the run's processes held and did at its end, from its logs: the title's census of system memory, and for
    each helper its program, resident memory and Wine server calls per 10 s (median of its last three reports).
    The game's own helper is marked."""
    record = {'system_mib': None, 'helpers_mib': None, 'helpers': None, 'processes': []}
    log = ROOT / 'klog' / (name + '.log')
    if log.exists():
        census = re.findall(r'system memory (\d+) MiB \(helpers (\d+) MiB in (\d+),', log.read_text(errors='replace'))
        if census:
            record['system_mib'], record['helpers_mib'], record['helpers'] = map(int, census[-1])
    game = executable.rsplit('\\', 1)[-1].lower()
    for child in sorted((ROOT / 'klog').glob(name + '-wine-child-*.log')):
        text = child.read_text(errors='replace')
        program = re.search(r'Loaded L"[^"]*\\([^"\\]+\.exe)"', text)
        calls = [int(c) for c in re.findall(r'wine-child: server calls=(\d+)', text)][-3:]
        resident = re.findall(r'memory resident=(\d+) MiB', text)
        if not program or not resident:
            continue
        record['processes'].append({'program': program.group(1), 'game': program.group(1).lower() == game,
                                    'resident_mib': int(resident[-1]),
                                    'server_calls_per_10s': sorted(calls)[len(calls) // 2] if calls else None})
    return record


def run(name, arguments):
    command = [sys.executable, str(ROOT / 'tools/run-console.py'), '--name', name] + arguments
    print('$ ' + ' '.join(command), flush=True)
    subprocess.run(command, cwd=ROOT)
    path = ROOT / 'evidence' / (name + '.json')
    return json.loads(path.read_text()) if path.exists() else {}


def compare(launcher, desktop):
    reasons = []
    if not launcher or not desktop:
        return None, ['a route recorded no frames of the game']
    if desktop['fps'] < 0.95 * launcher['fps']:
        reasons.append(f"fps {desktop['fps']} against {launcher['fps']}")
    if (desktop['p95_worst_ms'] or 0) > (launcher['p95_worst_ms'] or 0) + 2:
        reasons.append(f"worst p95 {desktop['p95_worst_ms']} ms against {launcher['p95_worst_ms']} ms")
    share = lambda c: 100.0 * c['over20'] / max(1, c['frames'] - c['windows'])
    if share(desktop) > share(launcher) + 1:
        reasons.append(f"{share(desktop):.1f} % of intervals over 20 ms against {share(launcher):.1f} %")
    return bool(reasons), reasons


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--profile', required=True, help='catalog id under /data/ps5proton/games')
    parser.add_argument('--seconds', type=int, default=90)
    parser.add_argument('--start-exe', action='store_true',
                        help='start the executable directly, with the entry environment as overrides (no shortcut)')
    parser.add_argument('--desktop-only', action='store_true', help='skip the launcher route')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name) or not re.fullmatch(r'[a-zA-Z0-9_-]+', args.profile):
        parser.error('Invalid name or profile')
    game = catalog_entry(args.profile)
    seconds = ['--seconds', str(args.seconds), '--watchdog', str(args.seconds + 120)]
    launcher = {} if args.desktop_only else run(args.name + '-launcher', ['--profile', args.profile] + seconds)
    started = desktop_arguments(game) if args.start_exe else shortcut_arguments(game)
    desktop = run(args.name + '-desktop', ['--profile', 'mihawk-os', f'--args={started}'] +
                  ([f'--env={item}' for item in game.get('env', [])] if args.start_exe else []) + seconds)
    direct, composed = game_client(launcher, ('direct',)), game_client(desktop, ('composed', 'independent'))
    log = ROOT / 'klog' / (args.name + '-desktop.log')
    text = log.read_text(errors='replace') if log.exists() else ''
    # The title's own records: the shortcut it wrote, and the settings it gave the game (src/pw_app_environment.h).
    settings = re.findall(r'Mihawk: "([^"]+)" \(([^)]*)\) gets its library settings: (\d+)', text)
    shortcuts = re.findall(r'Mihawk: desktop shortcuts for (\d+) library games: written (\d+), unchanged (\d+), '
                           r'removed (\d+), failed (\d+)', text)
    slower, reasons = compare(direct, composed)
    record = {'run': args.name, 'profile': args.profile, 'game': game.get('name'), 'seconds': args.seconds,
              'question': 'Is the game slower or less even in the Mihawk desktop (composed) than from the launcher '
                          '(direct)?',
              'launcher': {'run': args.name + '-launcher', 'passed': launcher.get('passed'), 'client': direct,
                           'overhead': overhead(args.name + '-launcher', game['executable'])},
              'desktop': {'run': args.name + '-desktop', 'passed': desktop.get('passed'), 'client': composed,
                          'overhead': overhead(args.name + '-desktop', game['executable']),
                          'arguments': started, 'route': 'executable' if args.start_exe else 'shortcut',
                          'env': game.get('env', []) if args.start_exe else [],
                          'library_settings': [{'name': n, 'image': i, 'settings': int(c)} for n, i, c in settings],
                          'shortcuts': [dict(zip(('games', 'written', 'unchanged', 'removed', 'failed'), map(int, m)))
                                        for m in shortcuts]},
              'desktop_slower': slower, 'reasons': reasons,
              'thresholds': {'fps_loss': 0.05, 'p95_ms': 2, 'over20_points': 1}}
    (ROOT / 'evidence' / (args.name + '.json')).write_text(json.dumps(record, indent=1) + '\n')
    print(json.dumps(record, indent=1), flush=True)
    return 0 if slower is not None else 1


if __name__ == '__main__':
    sys.exit(main())
