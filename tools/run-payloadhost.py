#!/usr/bin/env python3
"""Run the payload-host capacity probe: how many, how long, how many files.

    tools/run-payloadhost.py build [--step linger|fd]
    tools/run-payloadhost.py send  [--step linger|fd]      one send, output echoed
    tools/run-payloadhost.py count --count 8 [--settle 12] how many stay alive at once
    tools/run-payloadhost.py life                          close the stream, see if it exits

Why: a process the payload loader starts is outside the title's seven helpers,
which is what stopped a game launcher's browser (CEF). Before relying on that, three things are
unmeasured - how many such processes the console runs, whether one ends when its
starter goes away, and whether it has its own open-file budget. Each send uses
websrv's payload launcher (POST /elfldr, pipe=1) exactly as the other probes do,
one step per send.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import io
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan' / 'tools'))
import ps5_console  # noqa: E402

ELF = ROOT / 'build' / 'payloadhost.elf'


def mask(text):
    return re.sub(r'\b\d{1,3}(\.\d{1,3}){3}\b', '<console>', str(text))


def build(step=None):
    subprocess.run([str(TOOLS / 'build-payloadhost.sh')], check=True)
    return ELF


def send(step, out_path, keep_open=False):
    """POST the payload to websrv's launcher; the response streams its stdout."""
    settings = ps5_console.load_settings()
    command = ['curl', '-sS', '-N', '-m', '3600', '-F', f'elf=@{ELF}', '-F', f'args={step}', '-F',
               'pipe=1', f'http://{settings["host"]}:8080/elfldr']
    output = open(out_path, 'wb')
    # start_new_session: a held stream must outlive the command that opened it.
    return subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                            start_new_session=True), output


def read(path):
    try:
        return io.open(path, 'rb').read().decode('utf-8', 'replace')
    except FileNotFoundError:
        return ''


def command_send(args):
    process, output = send(args.step, args.output)
    if args.keep_open:
        print(f'sent {args.step!r}, stream held open (pid {process.pid})')
        return 0
    try:
        process.wait()
    finally:
        output.close()
    print(mask(read(args.output)))
    return 0


def ftp_settings():
    return ps5_console.load_settings()


def list_data(ftp):
    """This FTP server answers LIST but not NLST."""
    lines = []
    ftp.retrlines('LIST /data', lines.append)
    return [line.split()[-1] for line in lines if line.split()]


def markers():
    """Which pids wrote an alive/gone marker under /data."""
    import ftplib
    settings = ftp_settings()
    alive, gone = set(), set()
    ftp = ftplib.FTP()
    ftp.connect(settings['host'], settings['ftp_port'], timeout=20)
    ftp.login(settings['ftp_user'], settings['ftp_password'])
    try:
        names = list_data(ftp)
    finally:
        try:
            ftp.quit()
        except (EOFError, OSError):
            pass
    for name in names:
        match = re.fullmatch(r'payloadhost-(\d+)\.(alive|gone)', name.rsplit('/', 1)[-1])
        if match:
            (alive if match.group(2) == 'alive' else gone).add(int(match.group(1)))
    return alive, gone


def clear_markers():
    """Markers from an earlier send must not be read as this one's evidence."""
    import ftplib
    settings = ftp_settings()
    ftp = ftplib.FTP()
    ftp.connect(settings['host'], settings['ftp_port'], timeout=20)
    ftp.login(settings['ftp_user'], settings['ftp_password'])
    try:
        for name in list_data(ftp):
            base = name.rsplit('/', 1)[-1]
            if re.fullmatch(r'payloadhost-\d+\.(alive|gone)', base):
                try:
                    ftp.delete('/data/' + base)
                except (ftplib.error_perm, ftplib.error_reply):
                    pass  # this server answers 226 File deleted, which ftplib calls unexpected
    finally:
        try:
            ftp.quit()
        except (EOFError, OSError):
            pass


def pids_in(paths):
    found = []
    for path in paths:
        match = re.search(r'payloadhost pid=(\d+)', read(path))
        if match:
            found.append(int(match.group(1)))
    return found


def command_count(args):
    """Start N at once and see how many are still alive after the settle."""
    clear_markers()
    logs = []
    processes = []
    for index in range(args.count):
        path = ROOT / 'klog' / f'payloadhost-count-{index:02d}.log'
        process, output = send('linger', path)
        processes.append((index, process, output, path))
        print(f'started instance {index} (pid {process.pid})', flush=True)
        time.sleep(args.gap)

    print(f'settling {args.settle:.0f} s', flush=True)
    time.sleep(args.settle)

    alive = 0
    for index, process, _, path in processes:
        running = process.poll() is None
        alive += running
        first = (read(path).splitlines() or [''])[0]
        print(f'  instance {index}: {"alive" if running else "gone"}  {mask(first)[:90]}')
    print(f'RESULT: {alive} of {args.count} payload processes alive at once')

    print('closing every stream; a payload should end when its starter does')
    for index, process, output, path in processes:
        process.terminate()
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        output.close()
    time.sleep(args.settle)
    alive_markers, gone_markers = markers()
    wanted = pids_in([path for _, _, _, path in processes])
    print(f'RESULT: {len(wanted)} instances reported their pid; '
          f'{len(gone_markers & set(wanted))} wrote a gone marker after the stream closed; '
          f'{len(alive_markers & set(wanted))} still marked alive')
    clear_markers()
    return 0


def command_life(args):
    """One send, close the stream, and read the marker the payload leaves behind."""
    path = ROOT / 'klog' / 'payloadhost-life.log'
    clear_markers()
    process, output = send('linger', path)
    time.sleep(args.settle)
    print(mask(read(path))[:500])
    wanted = pids_in([path])
    print(f'payload pid: {wanted}', flush=True)
    print('closing the stream now', flush=True)
    process.terminate()
    try:
        process.wait(timeout=20)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    output.close()
    time.sleep(6)
    alive_markers, gone_markers = markers()
    print(f'markers: alive={sorted(alive_markers)} gone={sorted(gone_markers)}')
    if set(wanted) & gone_markers:
        print('RESULT: it saw the stream close and ended itself - no orphan')
    elif set(wanted) & alive_markers:
        print('RESULT: it is still running after its starter went away - an orphan')
    else:
        print('RESULT: no marker for its pid: it may have been killed before it could write one')
    clear_markers()
    return 0


STATE = ROOT / 'klog' / 'payloadhost-instances.json'


def load_state():
    try:
        return json.loads(STATE.read_text())
    except (FileNotFoundError, ValueError):
        return []


def save_state(instances):
    STATE.write_text(json.dumps(instances, indent=1))


def command_start(args):
    """Start instances whose streams stay open, detached, so they outlive this command."""
    clear_markers()
    instances = []
    for index in range(args.count):
        path = ROOT / 'klog' / f'payloadhost-hold-{index:02d}.log'
        process, output = send('linger', path)
        # Detached: the stream must outlive this runner, or the payload would end
        # the moment the command returns.
        instances.append({'log': str(path), 'curl_pid': process.pid})
        print(f'started instance {index} (curl pid {process.pid})', flush=True)
        time.sleep(args.gap)
    time.sleep(6)  # let each payload print its identity line
    for instance in instances:
        pids = pids_in([instance['log']])
        instance['payload_pid'] = pids[0] if pids else None
    save_state(instances)
    alive_markers, gone_markers = markers()
    print(f'payloads alive: {sorted(alive_markers)}  gone: {sorted(gone_markers)}')
    print(f'state: {STATE}')
    return 0


def command_probe(args):
    """What is true of every held instance right now."""
    instances = load_state()
    if not instances:
        print('no instances recorded; run start first')
        return 1
    alive_markers, gone_markers = markers()
    live_curl = 0
    for instance in instances:
        try:
            os.kill(instance['curl_pid'], 0)
            stream_open = True
            live_curl += 1
        except (ProcessLookupError, PermissionError):
            stream_open = False
        pid = instance.get('payload_pid')
        lines = [line for line in read(instance['log']).splitlines() if 'linger: alive' in line]
        state = ('gone marker' if pid in gone_markers else
                 'alive marker' if pid in alive_markers else 'no marker')
        print(f"  instance pid={pid}: stream {'open' if stream_open else 'CLOSED'}, {state}, "
              f"last heartbeat {lines[-1].strip() if lines else 'none'}")
    print(f'RESULT: {live_curl} of {len(instances)} streams still open '
          f'({len(alive_markers)} alive markers, {len(gone_markers)} gone)')
    return 0


def command_stop(args):
    """Close every held stream; the payloads should end themselves."""
    instances = load_state()
    for instance in instances:
        try:
            os.kill(instance['curl_pid'], 15)
        except ProcessLookupError:
            pass
    time.sleep(8)
    _, gone_markers = markers()
    wanted = {instance.get('payload_pid') for instance in instances}
    print(f'RESULT: {len(gone_markers & wanted)} of {len(instances)} wrote a gone marker')
    clear_markers()
    save_state([])
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)

    build_parser = commands.add_parser('build')
    build_parser.set_defaults(handler=lambda args: (build(), 0)[1])

    send_parser = commands.add_parser('send')
    send_parser.add_argument('--step', default='linger')
    send_parser.add_argument('--output', default=str(ROOT / 'klog' / 'payloadhost-send.log'))
    send_parser.add_argument('--keep-open', action='store_true')
    send_parser.set_defaults(handler=command_send)

    count_parser = commands.add_parser('count')
    count_parser.add_argument('--count', type=int, default=8)
    count_parser.add_argument('--gap', type=float, default=1.0)
    count_parser.add_argument('--settle', type=float, default=12.0)
    count_parser.set_defaults(handler=command_count)

    start_parser = commands.add_parser('start')
    start_parser.add_argument('--count', type=int, default=4)
    start_parser.add_argument('--gap', type=float, default=1.0)
    start_parser.set_defaults(handler=command_start)

    commands.add_parser('probe').set_defaults(handler=command_probe)
    commands.add_parser('stop').set_defaults(handler=command_stop)

    life_parser = commands.add_parser('life')
    life_parser.add_argument('--settle', type=float, default=12.0)
    life_parser.set_defaults(handler=command_life)

    args = parser.parse_args()
    (ROOT / 'klog').mkdir(exist_ok=True)
    if args.command != 'build' and not ELF.is_file():
        build()
    return args.handler(args)


if __name__ == '__main__':
    sys.exit(main())
