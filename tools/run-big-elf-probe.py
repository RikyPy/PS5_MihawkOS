#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""docs/SELF_INSTALL.md phase 0, probe 4: does a loader start an ELF the size of the setup program?

  tools/run-big-elf-probe.py --name RUN --route elfldr|websrv|pldmgr [--mib 30]

Builds tools/big-elf-probe.c with the pinned payload SDK around a pseudo-random blob of --mib MiB (the same bytes for
the same size), sends it the route's way, and waits for its klog line, which says how many bytes it found and their
FNV-1a hash:
  elfldr  the raw ELF to the console's elfldr port (9021)
  websrv  websrv's POST /elfldr, the multipart form the title sends its daemon with (src/pw_elfldr.c), port 8080
  pldmgr  serves a payload list and the ELF from this PC on --serve-port; the owner adds it as a source in pldmgr on
          the console and starts it from there (the run waits --wait seconds for the line); pldmgr's version is read
          from its own page first (GET :8084/version)
Run --mib 1 first on a route: it says whether the route works at all, so a refusal at 30 is the size's.
The console must be idle (no title: payload tools never run beside one). Writes evidence/<RUN>.json and keeps the
klog capture in klog/<RUN>.log.
"""
import argparse
import hashlib
import http.server
import json
import os
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import ftplib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402
sys.path.insert(0, str(ROOT / 'tools'))
import console_guard  # noqa: E402
from console_log import LineLog, redact  # noqa: E402


def blob(mib):
    out = bytearray()
    counter = 0
    while len(out) < mib << 20:
        out += hashlib.sha256(b'mihawkos-big-elf %d' % counter).digest()
        counter += 1
    return bytes(out[:mib << 20])


def fnv(data):
    value = 1469598103934665603
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def build(mib):
    sdk = Path(subprocess.check_output(['bash', str(ROOT / 'ps5/tools/setup-sdk.sh')], text=True).strip())
    work = ROOT / '.deps/big-elf-probe' / str(mib)
    elf = work / 'big-elf-probe.elf'
    source = ROOT / 'tools/big-elf-probe.c'
    stamp = hashlib.sha256(source.read_bytes() + (sdk / '.ps5-sdk-revision').read_bytes()).hexdigest()
    if elf.exists() and (work / 'stamp').read_text() == stamp:
        return elf, json.loads((work / 'build.json').read_text())
    work.mkdir(parents=True, exist_ok=True)
    data = blob(mib)
    (work / 'blob.bin').write_bytes(data)
    (work / 'blob.S').write_text('.section .rodata\n.balign 64\n.global big_elf_blob\nbig_elf_blob:\n'
                                 '.incbin "blob.bin"\n.global big_elf_blob_end\nbig_elf_blob_end:\n')
    expect = fnv(data)
    subprocess.run([str(sdk / 'bin/prospero-clang'), '-O2', '-Wall', '-Werror', f'-DBIG_ELF_EXPECT="{expect:016x}"',
                    f'-DBIG_ELF_EXPECT_VALUE=0x{expect:016x}ull', '-o', str(elf), str(source), 'blob.S'],
                   check=True, cwd=work)
    info = {'mib': mib, 'blob_fnv': f'{expect:016x}', 'elf_bytes': elf.stat().st_size,
            'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(), 'sdk_revision': (sdk / '.ps5-sdk-revision').read_text().strip(),
            'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest()}
    (work / 'build.json').write_text(json.dumps(info, indent=2) + '\n')
    (work / 'stamp').write_text(stamp)
    return elf, info


def build_space():
    """tools/space-probe.c: what a payload learns of free space (statfs, statvfs), no blob."""
    sdk = Path(subprocess.check_output(['bash', str(ROOT / 'ps5/tools/setup-sdk.sh')], text=True).strip())
    work = ROOT / '.deps/space-probe'
    work.mkdir(parents=True, exist_ok=True)
    elf = work / 'space-probe.elf'
    subprocess.run([str(sdk / 'bin/prospero-clang'), '-O2', '-Wall', '-Werror', '-o', str(elf),
                    str(ROOT / 'tools/space-probe.c')], check=True)
    return elf, {'program': 'space', 'elf_bytes': elf.stat().st_size, 'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest()}


def send_elfldr(host, data):
    with socket.create_connection((host, 9021), timeout=30) as connection:
        connection.sendall(data)
    return {'sent': len(data)}


def send_websrv(host, data, name):
    boundary = 'ProtonPayload'
    fields = (f'--{boundary}\r\nContent-Disposition: form-data; name="args"\r\n\r\nwebsrv\r\n'
              f'--{boundary}\r\nContent-Disposition: form-data; name="pipe"\r\n\r\n0\r\n'
              f'--{boundary}\r\nContent-Disposition: form-data; name="elf"; filename="{name}"\r\n\r\n').encode()
    tail = f'\r\n--{boundary}--\r\n'.encode()
    head = (f'POST /elfldr HTTP/1.1\r\nHost: {host}\r\nContent-Type: multipart/form-data; boundary={boundary}\r\n'
            f'Content-Length: {len(fields) + len(data) + len(tail)}\r\nConnection: close\r\n\r\n').encode()
    with socket.create_connection((host, 8080), timeout=60) as connection:
        connection.sendall(head + fields + data + tail)
        reply = connection.recv(512).decode('latin-1')
    return {'sent': len(data), 'reply': redact(reply.split('\r\n', 1)[0])}


def serve_pldmgr(directory, port, elf, console):
    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(directory), **kwargs)

        def log_message(self, fmt, *args):
            served.append(redact(fmt % args))
    served = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:   # this PC's address as the console reaches it
        route.connect((console, 9))
        here = route.getsockname()[0]
    # A bare list: pldmgr's parser takes the documented {"name", "payloads"} form's name for its first payload
    # (repository.c, json_helpers.c); it downloads with libcurl, then streams the file to elfldr (ps5_launcher.c).
    (directory / 'payloads.json').write_text(json.dumps([
        {'name': 'MihawkOS big ELF probe', 'filename': elf.name, 'url': f'http://{here}:{port}/{elf.name}',
         'version': '1', 'category': 'Tools', 'description': 'A harmless size probe: it prints one klog line',
         'checksum': hashlib.sha256(elf.read_bytes()).hexdigest()}], indent=2) + '\n')
    server = http.server.ThreadingHTTPServer(('0.0.0.0', port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, served


class Pldmgr:
    """Drive pldmgr 0.5.2 from this PC (http_server.c, port 8084), then put it back: the source list and pldmgr's own
    files must read the same after as before. Its delete call is not used: it also rewrites the payload's autoload
    entry; the files this run made are removed over FTP instead."""

    def __init__(self, settings, url, record):
        self.settings, self.url, self.record = settings, url, record

    def get(self, path):
        try:
            with urllib.request.urlopen(f'http://{self.settings["host"]}:8084{path}', timeout=120) as page:
                return page.read().decode('utf-8', 'replace')
        except urllib.error.HTTPError as error:   # pldmgr says why in the body of a 400
            raise RuntimeError(f'{path.split("?")[0]}: {error.code} {error.read(300).decode("utf-8", "replace")}') from None

    def sources(self):
        return json.loads(self.get('/sources_list'))['sources']

    def ftp(self):
        ftp = ftplib.FTP()
        ftp.connect(self.settings['host'], self.settings['ftp_port'], timeout=30)
        ftp.login(self.settings['ftp_user'], self.settings['ftp_password'] or 'codex')
        return ftp

    def files(self):
        with self.ftp() as ftp:
            found = []
            def walk(directory, depth=0):
                lines = []
                try:
                    ftp.retrlines(f'LIST {directory}', lines.append)
                except ftplib.error_perm:
                    return
                for fields in (line.split(None, 8) for line in lines):
                    if len(fields) == 9 and fields[8] not in ('.', '..'):
                        path = f'{directory}/{fields[8]}'
                        if fields[0].startswith('d') and depth < 3:
                            walk(path, depth + 1)
                        elif not fields[0].startswith('d'):
                            found.append(path)
            walk('/data/pldmgr')
            return sorted(found)

    def start(self, elf):
        self.before = self.sources()
        self.files_before = self.files()
        self.had = any(s['url'] == self.url for s in self.before)
        self.record['pldmgr'] = {'sources_before': [redact(s['name']) for s in self.before], 'ours_already_listed': self.had,
                                 'files_before': len(self.files_before)}
        if not self.had:
            self.record['pldmgr']['add'] = redact(self.get('/sources_add?url=' + urllib.parse.quote(self.url, safe='')))
        ours = next((s for s in self.sources() if s['url'] == self.url), None)
        if not ours:
            raise RuntimeError('pldmgr did not list the source after adding it')
        self.record['pldmgr']['install'] = redact(self.get(
            f'/repository_install?filename={elf.name}&source_id={urllib.parse.quote(ours["id"], safe="")}'
            f'&repo_url={urllib.parse.quote(self.url, safe="")}'))
        self.record['pldmgr']['load'] = redact(self.get(f'/loadpayload:{elf.name}')[:200])

    def finish(self):
        made = [path for path in self.files() if path not in self.files_before]
        index = next((i for i, s in enumerate(self.sources()) if s['url'] == self.url), None)
        last = index is not None and index == len(self.sources()) - 1
        with self.ftp() as ftp:
            for path in self.files():
                ours = path in made or (last and path.rsplit('/', 1)[-1].startswith(f'repository_cache.json.src{index}.'))
                if ours and (path.startswith('/data/pldmgr/payloads/') or 'repository_cache.json.src' in path or
                             'source_validate' in path):
                    try:
                        ftp.delete(path)
                    except ftplib.error_reply as reply:   # ftpsrv answers a delete with 226
                        if not str(reply).startswith('226'):
                            raise
            try:
                ftp.rmd('/data/pldmgr/payloads/big-elf-probe')
            except (ftplib.error_perm, ftplib.error_reply):
                pass
        if index is not None:
            self.record['pldmgr']['remove'] = redact(self.get(f'/sources_remove?index={index}'))
        after = self.sources()
        expected = [s for s in self.before if s['url'] != self.url]
        files_after = self.files()
        self.record['pldmgr'].update(
            sources_after=[redact(s['name']) for s in after], sources_restored=after == expected,
            files_after=len(files_after), files_left_by_run=[p for p in files_after if p not in self.files_before],
            files_restored=all(p in files_after for p in self.files_before))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--route', required=True, choices=('elfldr', 'websrv', 'pldmgr'))
    parser.add_argument('--mib', type=int, default=30)
    parser.add_argument('--program', choices=('big', 'space'), default='big',
                        help='space: tools/space-probe.c instead, the free space a payload can read (websrv route)')
    parser.add_argument('--serve-port', type=int, default=8765)
    parser.add_argument('--drive', action='store_true',
                        help='pldmgr: add the source, install and start the probe through pldmgr\'s own web API, then remove it')
    parser.add_argument('--wait', type=int, default=30, help='seconds to wait for the payload\'s line (pldmgr: the owner\'s steps)')
    args = parser.parse_args()
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists():
        raise SystemExit('evidence already exists; use a new run name')
    elf, info = build_space() if args.program == 'space' else build(args.mib)
    marker = 'mihawkos-space: done' if args.program == 'space' else 'mihawkos-big-elf:'
    console_guard.check_launch_allowed()   # a dropped console or storage damage refuses every payload too
    settings = ps5_console.load_settings()
    if ' count=0 ' not in ps5_console.ps5vkctl_command(settings, 'procs', timeout=15) + ' ':
        raise SystemExit('the console is running an application: payload tools never run beside a title')
    record = {'route': args.route, 'build': info, 'started': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
              'passed': False}
    lines = []
    capture = ROOT / 'klog' / (args.name + '.log')
    capture.parent.mkdir(exist_ok=True)
    with socket.create_connection((settings['host'], settings['klog_port']), timeout=10) as klog, capture.open('w') as output:
        klog.settimeout(0.5)
        quiet_until = time.monotonic() + 20
        while time.monotonic() < quiet_until:   # the klog server replays its backlog first
            try:
                if not klog.recv(65536):
                    raise SystemExit('klog closed before the probe')
            except socket.timeout:
                break
        log = LineLog(output, lines.append)
        sent_at = time.monotonic()
        server = None
        if args.route in ('elfldr', 'websrv'):
            try:
                record['send'] = (send_elfldr(settings['host'], elf.read_bytes()) if args.route == 'elfldr' else
                                  send_websrv(settings['host'], elf.read_bytes(), elf.name))
            except OSError as error:   # a route that refuses is an answer: recorded, not a crash
                record['send'] = {'error': redact(error)}
                args.wait = 0
        else:
            try:   # pldmgr's own read-only version page (http_server.c, MENU_PORT 8084)
                with urllib.request.urlopen(f'http://{settings["host"]}:8084/version', timeout=5) as page:
                    record['pldmgr_version'] = page.read(64).decode('ascii', 'replace').strip()
            except OSError as error:
                record['pldmgr_version'] = f'not answering on 8084 ({redact(error)})'
            server, served = serve_pldmgr(elf.parent, args.serve_port, elf, settings['host'])
            if args.drive:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as route:
                    route.connect((settings['host'], 9))
                    driver = Pldmgr(settings, f'http://{route.getsockname()[0]}:{args.serve_port}/payloads.json', record)
                try:
                    driver.start(elf)
                except Exception as error:   # the source may be in: finish() still removes it
                    record['pldmgr_error'] = redact(error)
                    args.wait = 0
            print(f'pldmgr: add the source http://<this PC>:{args.serve_port}/payloads.json on the console, then start '
                  f'"MihawkOS big ELF probe"; waiting {args.wait} s', flush=True)
        found = None
        while time.monotonic() - sent_at < args.wait and not found:
            try:
                chunk = klog.recv(65536)
                if not chunk:
                    break
                log.write(chunk)
            except socket.timeout:
                pass
            found = next((line for line in lines if marker in line), None)
        log.finish()
        if server:
            if args.drive:
                try:
                    driver.finish()
                except Exception as error:
                    record['pldmgr_cleanup_error'] = redact(error)
            server.shutdown()
            record['served'] = served
    record['klog_line'] = found
    if args.program == 'space':
        record['space_lines'] = [line.split('mihawkos-space: ', 1)[1] for line in lines if 'mihawkos-space: ' in line]
    record['seconds_to_line'] = round(time.monotonic() - sent_at, 1) if found else None
    record['intact'] = bool(found and (args.program == 'space' or (f'fnv={info["blob_fnv"]}' in found and
                                                                   f'bytes={args.mib << 20}' in found)))
    record['passed'] = record['intact']
    record['capture'] = str(capture.relative_to(ROOT))
    evidence.write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps(record, indent=2))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
