#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What the console's kernel says about memory: tools/console-memory.c, built with the payload SDK and sent through
websrv's launcher (POST /elfldr, pipe=1), which streams its output back. It only reads.

  tools/console-memory.py [map=PID] [census]

map=PID  PID's mappings, summed by kind, with the files that have pages in memory
census   the memory helper's census (src/pw_memory_helper.c), taken in this payload and read back from its page
"""
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402

SOURCE = ROOT / 'tools/console-memory.c'
OUTPUT = ROOT / 'build/console-memory.elf'
SDK = ROOT / '.deps/native/ps5-payload-sdk'


def main():
    words = sys.argv[1:]
    if any(not word.startswith('map=') and word != 'census' for word in words):
        raise SystemExit(__doc__)
    helper = ROOT / 'src/pw_memory_helper.c'
    if not OUTPUT.exists() or OUTPUT.stat().st_mtime < max(SOURCE.stat().st_mtime, helper.stat().st_mtime):
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(SDK / 'bin/prospero-clang'), '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-DPW_MEMORY_HELPER_CENSUS', '-o', str(OUTPUT), str(SOURCE), str(helper), str(ROOT / 'src/pw_memory_file.c')], check=True)
    settings = ps5_console.load_settings()
    boundary = 'ConsoleMemory'
    body = b''
    for name, data in (('args', ' '.join(['console-memory'] + words).encode()), ('pipe', b'1'),
                       ('elf', OUTPUT.read_bytes())):
        header = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
        if name == 'elf':
            header += '; filename="console-memory.elf"'
        body += (header + '\r\n\r\n').encode() + data + b'\r\n'
    body += f'--{boundary}--\r\n'.encode()
    request = urllib.request.Request(f'http://{settings["host"]}:8080/elfldr', data=body,
                                     headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            print(response.read().decode(errors='replace').replace(settings['host'], '<console>'), end='')
    except OSError as error:
        raise SystemExit(f'websrv: {str(error).replace(settings["host"], "<console>")}') from None


if __name__ == '__main__':
    main()
