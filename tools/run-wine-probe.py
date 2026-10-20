#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run a PE64 console probe with the title's own restart and exit path."""
import argparse
import importlib.util
import ftplib
import io
from pathlib import Path
import sys
import threading

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT.parent / 'PS5_Vulkan/tools'
sys.path.insert(0, str(TOOLS))
import ps5_console

spec = importlib.util.spec_from_file_location('run_title', TOOLS / 'run-title.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

parser = argparse.ArgumentParser(description=__doc__)
mode = parser.add_mutually_exclusive_group()
mode.add_argument('--d3d', action='store_true')
mode.add_argument('--input', action='store_true')
mode.add_argument('--hid', action='store_true')
mode.add_argument('--profile', choices=('1',), help='run the installed game instead of a probe')
parser.add_argument('--seconds', type=int, default=90, help='game run budget before requesting normal exit')
parser.add_argument('--output', default=str(ROOT / 'klog/wine-probe.log'))
args = parser.parse_args()
settings = ps5_console.load_settings()
sys.path.insert(0, str(ROOT / 'tools'))
import console_guard  # noqa: E402
console_guard.before_launch()  # refuses after a drop and paces launches
timer = None
try:
    if ' count=0 ' not in ps5_console.ps5vkctl_command(settings, 'procs') + ' ':
        raise RuntimeError('Console is not idle; probe refused')
    name = 'hid-probe.exe' if args.hid else 'd3d11-probe.exe' if args.d3d else 'xinput-probe.exe' if args.input else 'wine-probe.exe'
    with ftplib.FTP() as ftp:
        ftp.connect(settings['host'], settings['ftp_port'], timeout=30)
        ftp.login(settings['ftp_user'], settings['ftp_password'])
        try:
            ftp.delete('/data/ps5proton/stop-game')
        except ftplib.error_perm as error:
            if not str(error).startswith('550'):
                raise
        base = '/data/homebrew/PPSA99170'
        request = f'profile={args.profile}\n' if args.profile else f'path=C:\\{name}\n'
        ftp.storbinary(f'STOR {base}/wine-run.txt', io.BytesIO(request.encode()))
        ftp.storbinary(f'STOR {base}/test-run.txt', io.BytesIO(b'frames 10\nsamples all\n'))
    if args.profile:
        def request_stop():
            try:
                with ftplib.FTP() as ftp:
                    ftp.connect(settings['host'], settings['ftp_port'], timeout=15)
                    ftp.login(settings['ftp_user'], settings['ftp_password'])
                    ftp.storbinary('STOR /data/ps5proton/stop-game', io.BytesIO(b'run budget reached\n'))
                print('Requested the title to return to the launcher', flush=True)
            except Exception as error:
                print(str(error).replace(settings['host'], '<console>'), flush=True)
        timer = threading.Timer(args.seconds, request_stop)
        timer.start()
    console_guard.after_launch()
    result, log, _ = runner.run_title('PPSA99170', r'\[MihawkOS\].*ends: status', args.seconds + 30,
        args.output, echo_pattern=r'\[MihawkOS\]|PS5_PROTON_|wine startup|fatal signal', exit_grace=30)
    text = log.read_text()
    # The synchronous Wine sink survives LoadExec even when klog loses its tail.
    wine_log = Path(args.output).with_name(Path(args.output).stem + '-wine.log')
    with ftplib.FTP() as ftp:
        ftp.connect(settings['host'], settings['ftp_port'], timeout=20)
        ftp.login(settings['ftp_user'], settings['ftp_password'])
        with wine_log.open('wb') as output:
            ftp.retrbinary('RETR /data/ps5proton/logs/wine-last.log', output.write)
    text += wine_log.read_text(errors='replace')
    marker = ('PS5_PROTON_HID_PROBE PASS' if args.hid else 'PS5_PROTON_D3D11_PROBE PASS' if args.d3d else
              'PS5_PROTON_XINPUT_PROBE PASS' if args.input else 'PS5_PROTON_PE64_PROBE PASS')
    if (not args.profile and marker not in text) or result != 'finished':
        raise RuntimeError(f'Probe not proven: {result}; inspect {log}')
    print(f'Game run ended; inspect its output in {log}' if args.profile else f'Console probe passed; log: {log}')
except BaseException as error:
    if isinstance(error, SystemExit) and error.code == 0:
        raise
    raise SystemExit(str(error).replace(settings['host'], '<console>')) from None
finally:
    if timer:
        timer.cancel()
    # A shell-closed game never reaches the launcher's self-test consumer.
    # Remove one-shot requests once the console is idle, including failures.
    try:
        if ' count=0 ' in ps5_console.ps5vkctl_command(settings, 'procs') + ' ':
            with ftplib.FTP() as ftp:
                ftp.connect(settings['host'], settings['ftp_port'], timeout=15)
                ftp.login(settings['ftp_user'], settings['ftp_password'])
                for path in ('/data/homebrew/PPSA99170/test-run.txt',
                             '/data/homebrew/PPSA99170/wine-run.txt', '/data/ps5proton/stop-game'):
                    try:
                        ftp.delete(path)
                    except ftplib.error_perm as error:
                        if not str(error).startswith('550'):
                            raise
    except Exception as error:
        print('Test request cleanup: ' + str(error).replace(settings['host'], '<console>'), flush=True)
