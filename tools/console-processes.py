#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every process on the console and what it waits on, without the control payload.

Builds tools/console-processes.c with the payload SDK and sends it through websrv's launcher (POST /elfldr, pipe=1),
which streams its output back. It only reads. For when ps5vkctl accepts a connection and answers nothing: it told a
title stuck in its own exit, with SceSysCore stopped, from a hung payload (docs/WINDOWS_PROCESSES.md).

  tools/console-processes.py
"""
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402

SOURCE = ROOT / 'tools/console-processes.c'
OUTPUT = ROOT / 'build/console-processes.elf'
SDK = ROOT / '.deps/native/ps5-payload-sdk'


def main():
    if sys.argv[1:]:
        raise SystemExit(__doc__)
    if not OUTPUT.exists() or OUTPUT.stat().st_mtime < SOURCE.stat().st_mtime:
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(SDK / 'bin/prospero-clang'), '-std=c11', '-O2', '-Wall', '-Wextra', '-o', str(OUTPUT),
                        str(SOURCE)], check=True)
    settings = ps5_console.load_settings()
    boundary = 'ConsoleProcesses'
    body = b''
    for name, data in (('args', b'console-processes'), ('pipe', b'1'),
                       ('elf', OUTPUT.read_bytes())):
        header = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
        if name == 'elf':
            header += '; filename="console-processes.elf"'
        body += (header + '\r\n\r\n').encode() + data + b'\r\n'
    body += f'--{boundary}--\r\n'.encode()
    request = urllib.request.Request(f'http://{settings["host"]}:8080/elfldr', data=body,
                                     headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            print(response.read().decode(errors='replace').replace(settings['host'], '<console>'), end='')
    except OSError as error:
        raise SystemExit(f'websrv: {str(error).replace(settings["host"], "<console>")}') from None


if __name__ == '__main__':
    main()
