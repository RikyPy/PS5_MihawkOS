#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Start the owned-root service by hand; logs stay on the console.

PS5 Proton starts this daemon itself when no daemon answers its /data request (proton_data_grant), so
an ordinary launch needs nothing from this host. Two daemons must never run: both would take the
same request. Use this tool only before the title has been opened in the current boot, and say so
with --no-launch-this-boot.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import sys
import time
import urllib.request
from ftplib import FTP, error_perm

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console
import console_guard
from console_log import redact


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--no-launch-this-boot', action='store_true',
                        help='PS5 Proton has not been opened since the console started (it would have started the daemon)')
    if not parser.parse_args().no_launch_this_boot:
        raise RuntimeError('PS5 Proton starts the daemon itself when it opens; start one by hand only before the '
                           'title has been opened this boot, with --no-launch-this-boot')
    settings = ps5_console.load_settings()
    state_path = ROOT / '.deps/lapy-service.json'
    guard = console_guard.load()
    if state_path.exists():
        prior = json.loads(state_path.read_text())
        if prior['started'] >= guard['boot_started']:
            raise RuntimeError('Service already started or attempted this boot; inspect its log before starting another')
    if ' count=0 ' not in ps5_console.ps5vkctl_command(settings, 'procs', timeout=15) + ' ':
        raise RuntimeError('Console is not idle; close the title before starting the service')
    build = ROOT / '.deps/lapy-validation/source/build/owned_root_daemon-service-verified-client'
    manifest = json.loads((build / 'manifest.json').read_text())
    elf = (build / 'lapy-root-daemon.elf').read_bytes()
    if (manifest['elf_sha256'] != hashlib.sha256(elf).hexdigest() or manifest['mode'] != 'service'
            or manifest['max_requests'] is not None or manifest['target_title'] != 'PPSA99170'):
        raise RuntimeError('Resident payload manifest mismatch')
    folder = ROOT / 'klog' / time.strftime('resident-service-%Y%m%d-%H%M%S')
    folder.mkdir(parents=True, exist_ok=False)
    (folder / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (folder / 'lapy-root-daemon.elf').write_bytes(elf)
    def read_log():
        with FTP() as ftp:
            ftp.connect(settings['host'], settings['ftp_port'], timeout=15)
            ftp.login(settings['ftp_user'], settings['ftp_password'] or 'codex')
            sink = io.BytesIO()
            try:
                ftp.retrbinary('RETR /data/ps5proton/logs/lapy-service.log', sink.write)
            except error_perm as error:
                if not str(error).startswith('550'):
                    raise
            return sink.getvalue()
    previous = read_log()
    (folder / 'previous.log').write_bytes(previous)
    # Persist uncertain delivery too: a retry must never create two daemons.
    state = {'started': time.time(), 'status': 'delivery-attempted', 'build_id': manifest['build_id'],
             'elf_sha256': manifest['elf_sha256'], 'capture': str(folder.relative_to(ROOT))}
    state_path.write_text(json.dumps(state, indent=2) + '\n')
    boundary = 'ProtonResidentPayload'
    body = b''
    for name, data in [('args', b''), ('pipe', b'0'), ('elf', elf)]:
        header = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
        if name == 'elf':
            header += '; filename="lapy-root-daemon.elf"'
        body += (header + '\r\n\r\n').encode() + data + b'\r\n'
    body += f'--{boundary}--\r\n'.encode()
    request = urllib.request.Request('http://' + settings['host'] + ':8080/elfldr', data=body,
              headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            response.read(4096)
    except Exception as error:
        # Delivery may have succeeded despite an HTTP timeout; verify the console log, never resend.
        state['delivery_note'] = redact(error)
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        current = read_log()
        if not current.startswith(previous):
            raise RuntimeError('Service log changed unexpectedly; no retry')
        text = current[len(previous):].decode(errors='replace')
        (folder / 'startup.log').write_text(redact(text))
        if 'daemon_result' in text or 'daemon_held' in text or '\tERR\t' in text:
            raise RuntimeError('Resident preflight failed; inspect captured log')
        if manifest['build_id'] in text and 'phase=service_baseline' in text:
            state.update(status='ready', log='/data/ps5proton/logs/lapy-service.log',
                         logging='console file; host connection not required', max_requests=None)
            state_path.write_text(json.dumps(state, indent=2) + '\n')
            print('Resident owned-root service ready for this boot; diagnostics persist on the console.')
            print('Record: ' + str(state_path))
            return 0
        time.sleep(.25)
    raise RuntimeError('No resident readiness proof; inspect captured log before retrying')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except BaseException as error:
        if isinstance(error, SystemExit) and isinstance(error.code, int):
            raise
        print('Service start failed: ' + redact(error), file=sys.stderr)
        sys.exit(1)
