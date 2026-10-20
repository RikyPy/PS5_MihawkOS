#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Install the D3D probe and pinned DXVK into our isolated probe prefix."""
import ftplib
import hashlib
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
sys.path.insert(0, str(ROOT / 'tools'))
import ps5_console
import console_guard
settings = ps5_console.load_settings()
try:
    if ' count=0 ' not in ps5_console.ps5vkctl_command(settings, 'procs') + ' ':
        raise RuntimeError('Console is not idle')
    uploads = {'d3d11-probe.exe': ROOT / '.deps/probes/d3d11-probe.exe'}
    uploads.update({f'windows/system32/{name}': ROOT / '.deps/dxvk-release' / name
                    for name in ('dxgi.dll', 'd3d11.dll', 'd3d10core.dll')})
    with console_guard.writing('probe', settings), ftplib.FTP() as ftp:
        ftp.connect(settings['host'], settings['ftp_port'], timeout=30)
        ftp.login(settings['ftp_user'], settings['ftp_password'])
        for relative, local in uploads.items():
            remote = '/data/ps5proton/prefixes/probe/drive_c/' + relative
            with local.open('rb') as data:
                ftp.storbinary('STOR ' + remote, data, blocksize=262144)
            digest = hashlib.sha256()
            ftp.retrbinary('RETR ' + remote, digest.update, blocksize=262144)
            if digest.hexdigest() != hashlib.sha256(local.read_bytes()).hexdigest():
                raise RuntimeError(f'Readback mismatch: {relative}')
    print('D3D probe and three DXVK DLLs uploaded and verified')
except BaseException as error:
    raise SystemExit(str(error).replace(settings['host'], '<console>')) from None
