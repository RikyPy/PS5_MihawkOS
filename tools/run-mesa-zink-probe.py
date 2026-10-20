#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Test upstream PE64 Mesa/Zink in the project-owned probe prefix, then restore it."""
import argparse
import hashlib
import importlib.util
import io
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('console_runner', ROOT / 'tools/run-console.py')
console = importlib.util.module_from_spec(spec)
spec.loader.exec_module(console)
REMOTE = '/data/ps5proton/prefixes/probe/drive_c/windows/system32/'


def download(ftp, name):
    data = io.BytesIO()
    ftp.retrbinary('RETR ' + REMOTE + name, data.write)
    return data.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--exe', choices=('gl-vertex-probe.exe', 'gl-present-probe.exe',
                                         'gl-exit-probe.exe', 'gl-sprite-probe.exe'), default='gl-vertex-probe.exe')
    parser.add_argument('--env', action='append', default=[], help='NAME=VALUE, added for this launch (MESA_DEBUG=1, ...)')
    parser.add_argument('--args', help="the probe's command line for this launch (gl-sprite-probe.exe: evict)")
    parser.add_argument('--mesa-work', type=Path, default=ROOT / '.deps/mesa-foundation-wgl',
                        help="a Mesa WGL build folder and its manifest.json: a candidate build (default: the pinned one)")
    args = parser.parse_args()
    if not args.name.replace('-', '').replace('_', '').isalnum():
        parser.error('use a plain run name')
    work = args.mesa_work
    manifest = json.loads((work / 'manifest.json').read_text())
    modules = {name: work / 'build/src/gallium/targets' / target / name for name, target in
               [('opengl32.dll', 'libgl-gdi'), ('libgallium_wgl.dll', 'wgl')]}
    for name, path in modules.items():
        if hashlib.sha256(path.read_bytes()).hexdigest() != manifest['files'][name]:
            raise SystemExit('Mesa module differs from its manifest')
    settings = console.ps5_console.load_settings()
    if not console.idle(settings):
        raise SystemExit('Console is not idle')
    backup = ROOT / 'build/zink-probe-backup' / args.name
    backup.mkdir(parents=True, exist_ok=False)
    previous, changed = {}, []
    record = {'run': args.name, 'mesa': manifest, 'restored': False, 'passed': False}
    evidence = ROOT / 'evidence' / (args.name + '-modules.json')
    try:
        with console.console_guard.writing('probe', settings), console.connect(settings) as ftp:
            for name in modules:
                try:
                    previous[name] = download(ftp, name)
                    (backup / name).write_bytes(previous[name])
                except console.ftplib.error_perm as error:
                    if not str(error).startswith('550'):
                        raise
                    previous[name] = None
            (backup / 'state.json').write_text(json.dumps({n: b is not None for n, b in previous.items()}))
            for name, path in modules.items():
                changed.append(name)
                with path.open('rb') as source:
                    ftp.storbinary('STOR ' + REMOTE + name, source)
                if hashlib.sha256(download(ftp, name)).hexdigest() != manifest['files'][name]:
                    raise RuntimeError('Mesa module readback mismatch')
        result = subprocess.run([sys.executable, str(ROOT / 'tools/run-console.py'),
            '--exe', args.exe, '--name', args.name, '--watchdog', '120',
            '--env', 'WINEDLLOVERRIDES=opengl32=n;d3d11,dxgi,d3d10core=n,b;bcrypt=b',
            '--env', 'GALLIUM_DRIVER=zink', '--env', 'WINE_PS5_HID=0',
            '--env', 'PROTON_LOADING_SCREEN=0', '--env', 'PROTON_GPU_CAPTURE=1',
            '--env', 'WINEDEBUG=-all,+seh,+debugstr,+loaddll'] +
            [word for extra in args.env for word in ('--env', extra)] +
            (['--args', args.args] if args.args else []), cwd=ROOT)
        record['runner_status'] = result.returncode
        record['passed'] = result.returncode == 0
    finally:
        try:
            if console.idle(settings):
                with console.console_guard.writing('probe', settings), console.connect(settings) as ftp:
                    for name in changed:
                        if previous[name] is None:
                            console.remove(ftp, REMOTE + name)
                        else:
                            ftp.storbinary('STOR ' + REMOTE + name, io.BytesIO(previous[name]))
                            if download(ftp, name) != previous[name]:
                                raise RuntimeError('Probe prefix restoration mismatch')
                record['restored'] = True
        except (Exception, SystemExit) as error:
            record['restore_error'] = str(error).replace(settings['host'], '<console>')
        evidence.write_text(json.dumps(record, indent=2) + '\n')
        print(json.dumps({k: record[k] for k in ('run', 'passed', 'restored')}))
    return 0 if record['passed'] and record['restored'] else 1


if __name__ == '__main__':
    main()
