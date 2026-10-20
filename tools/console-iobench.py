#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Native network and disk speeds on the console (tools/console-iobench.c), for comparing with the Wine probe.

  tools/console-iobench.py [--port 1749] [--dir /data/ps5proton]

Serves zeros on --port from this machine, sends the payload through websrv's launcher (as tools/console-memory.py
does) and prints what it measured. Only while no title runs: a payload during a title's run jams elfldr.
"""
import argparse
import socket
import subprocess
import sys
import threading
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402

SOURCE = ROOT / 'tools/console-iobench.c'
OUTPUT = ROOT / 'build/console-iobench.elf'
SDK = ROOT / '.deps/native/ps5-payload-sdk'


def serve(listener, stop):
    chunk = bytes(1 << 20)

    def send(connection):
        try:
            while not stop.is_set():
                connection.sendall(chunk)
        except OSError:
            pass
        finally:
            connection.close()

    while not stop.is_set():
        try:
            connection, _ = listener.accept()
        except OSError:
            break
        threading.Thread(target=send, args=(connection,), daemon=True).start()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', type=int, default=1749)
    parser.add_argument('--dir', default='/data/ps5proton')
    parser.add_argument('--only', default='', help='comma list of fs, flags, sizes, mapped, parallel (default: all)')
    args = parser.parse_args()
    if not OUTPUT.exists() or OUTPUT.stat().st_mtime < SOURCE.stat().st_mtime:
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(SDK / 'bin/prospero-clang'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-o', str(OUTPUT),
                        str(SOURCE)], check=True)
    settings = ps5_console.load_settings()
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.connect((settings['host'], 9))
    here = probe.getsockname()[0]
    probe.close()
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(('0.0.0.0', args.port))
    listener.listen(16)
    stop = threading.Event()
    threading.Thread(target=serve, args=(listener, stop), daemon=True).start()
    boundary = 'ConsoleIoBench'
    body = b''
    for name, data in (('args', (f'console-iobench {here} {args.port} {args.dir}' + (f' only={args.only}' if args.only else '')).encode()), ('pipe', b'1'),
                       ('elf', OUTPUT.read_bytes())):
        header = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
        if name == 'elf':
            header += '; filename="console-iobench.elf"'
        body += (header + '\r\n\r\n').encode() + data + b'\r\n'
    body += f'--{boundary}--\r\n'.encode()
    request = urllib.request.Request(f'http://{settings["host"]}:8080/elfldr', data=body,
                                     headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
    try:
        with urllib.request.urlopen(request, timeout=180) as response:
            print(response.read().decode(errors='replace').replace(settings['host'], '<console>').replace(here, '<host>'), end='')
    except OSError as error:
        raise SystemExit(f'websrv: {str(error).replace(settings["host"], "<console>")}') from None
    finally:
        stop.set()
        listener.close()


if __name__ == '__main__':
    main()
