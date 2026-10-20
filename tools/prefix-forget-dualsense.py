#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make a finished prefix forget the DualSense, to test its first session again without wineboot's 4 minutes.

  tools/prefix-forget-dualsense.py [--prefix mihawk] [--restore]

On 2026-10-13 a game started in the first session of a new prefix never acquired the DualSense (DirectInput found no controller),
and in every later session of the same prefix it did. What differs is state the first session writes: the HID and USB device
instances of VID_054C&PID_0CE6 under Enum, and their device-interface keys under Control\\DeviceClasses. This removes those sections
from the prefix's system.reg (the original is kept beside it as system.reg.before-forget-dualsense) and the DirectInput calibration
key from user.reg, so the next session meets the DualSense for the first time. --restore puts the originals back.

The console must be idle (no title running): the wineserver writes the registry back when a session ends."""
import argparse
import ftplib
import io
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
sys.path.insert(0, str(ROOT / 'tools'))
import ps5_console  # noqa: E402
import console_guard  # noqa: E402

SYSTEM_SECTIONS = (r'Enum\\\\HID\\\\VID_054C', r'Enum\\\\USB\\\\VID_054C',
                   r'DeviceClasses\\\\\{4D1E55B2-F16F-11CF-88CB-001111000030\}\\\\##\?#HID#VID_054C')
USER_SECTIONS = (r'DirectInput\\\\VID_054C',)
KEPT = '.before-forget-dualsense'


def strip(text, patterns):
    """The registry text without the sections whose header matches one of the patterns, and how many were left out."""
    kept, removed = [], 0
    for block in re.split(r'(?m)^(?=\[)', text):
        head = block.split('\n', 1)[0]
        if head.startswith('[') and any(re.search(pattern, head, re.I) for pattern in patterns):
            removed += 1
        else:
            kept.append(block)
    return ''.join(kept), removed


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--prefix', default='mihawk')
    parser.add_argument('--restore', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', args.prefix):
        raise SystemExit('prefix must be a folder ID')
    settings = ps5_console.load_settings()
    if ' count=0 ' not in ps5_console.ps5vkctl_command(settings, 'procs', 15) + ' ':
        raise SystemExit('a title is running: the registry is written back when its session ends')
    base = f'/data/ps5proton/prefixes/{args.prefix}'
    with console_guard.writing('probe', settings), ftplib.FTP() as ftp:
        ftp.connect(settings['host'], settings['ftp_port'], timeout=30)
        ftp.login(settings['ftp_user'], settings['ftp_password'])
        for name, patterns in (('system.reg', SYSTEM_SECTIONS), ('user.reg', USER_SECTIONS)):
            current, kept = io.BytesIO(), io.BytesIO()
            ftp.retrbinary(f'RETR {base}/{name}', current.write)
            if args.restore:
                try:
                    ftp.retrbinary(f'RETR {base}/{name}{KEPT}', kept.write)
                except ftplib.error_perm:
                    print(f'{name}: no copy was kept, nothing to restore')
                    continue
                ftp.storbinary(f'STOR {base}/{name}', io.BytesIO(kept.getvalue()))
                print(f'{name}: the original is back ({len(kept.getvalue())} bytes)')
                continue
            text = current.getvalue().decode('utf-8', 'surrogateescape')
            stripped, removed = strip(text, patterns)
            if not removed:
                print(f'{name}: nothing to forget')
                continue
            try:  # never overwrite a copy kept from an earlier call: it is the true original
                ftp.size(f'{base}/{name}{KEPT}')
            except ftplib.error_perm:
                ftp.storbinary(f'STOR {base}/{name}{KEPT}', io.BytesIO(current.getvalue()))
            ftp.storbinary(f'STOR {base}/{name}', io.BytesIO(stripped.encode('utf-8', 'surrogateescape')))
            print(f'{name}: {removed} sections left out ({len(text)} -> {len(stripped)} bytes)')


if __name__ == '__main__':
    main()
