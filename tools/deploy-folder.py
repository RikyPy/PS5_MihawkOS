#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Put a host folder (a Windows program and its files) on the console, as C:\\Games\\<folder> of Mihawk OS.

--source DIR is sent as it is, with --id and --game-json for its library entry (/data/ps5proton/games/<id>/game.json);
--folder may be nested with /. The library entry is kept current even when no file needs sending.

The upload is resumable. A manifest of verified sha256 digests is kept on the console (.folder-deploy-manifest.json)
and only changed files are sent again. Every file is read back and compared before it counts.

Every connection turns the FTP server's SELF mode off (with it on, a signed file reads back decrypted and never
matches). The manifest is stored before the first file, so a run killed at any point can be resumed. The writes go
through the console guard (tools/console_guard.py begin_write/end_write: refused on a drop, storage damage or a
running title; ps5vkctl's sync after).
"""
import argparse
import concurrent.futures
import ftplib
import hashlib
import io
import json
import os
import queue
import re
import socket
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console
sys.path.insert(0, str(ROOT / 'tools'))
import console_guard

DATA_ROOT = '/data/ps5proton'
MANIFEST = '.folder-deploy-manifest.json'
BLOCK = 262144
ATTEMPTS = 3


class Item:
    def __init__(self, remote, size, sha, path=None, data=None):
        self.remote, self.size, self.sha, self.path, self.data = remote, size, sha, path, data

    def open(self):
        return io.BytesIO(self.data) if self.data is not None else open(self.path, 'rb')


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        while True:
            block = handle.read(1 << 20)
            if not block:
                return digest.hexdigest()
            digest.update(block)


def tree_size(path):
    if path.is_file():
        return path.stat().st_size
    return sum(f.stat().st_size for f in path.rglob('*') if f.is_file())


def plan(args):
    root = Path(args.source)
    if not root.is_dir():
        raise SystemExit(f'source directory is not there: {root}')
    files, empty_dirs, lowered = {}, set(), {}
    for directory, dirs, names in os.walk(root):
        relative = Path(directory).relative_to(root)
        dirs.sort()
        if not dirs and not names and relative.parts:
            empty_dirs.add(relative.as_posix())
        for name in sorted(names):
            remote = (relative / name).as_posix()
            if remote.lower() in lowered:
                raise SystemExit(f'{remote} and {lowered[remote.lower()]} differ only by case')
            lowered[remote.lower()] = remote
            files[remote] = root / relative / name

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        digests = dict(zip(files, pool.map(sha256_file, files.values())))
    items = [Item(remote, path.stat().st_size, digests[remote], path=path) for remote, path in files.items()]
    generated = {}
    for extra in args.extra:
        name = Path(extra).name
        if name in generated or name in files:
            raise SystemExit(f'{name} is already part of the folder')
        generated[name] = Path(extra).read_bytes()
    empty_dirs |= set(args.mkdir)
    for remote, raw in generated.items():
        items.append(Item(remote, len(raw), hashlib.sha256(raw).hexdigest(), data=raw))
    return items, sorted(empty_dirs)


def library_entry(args):
    return Path(args.game_json).read_bytes()


def command(settings, text):
    with socket.create_connection((settings['host'], settings['ctl_port']), timeout=15) as sock:
        sock.sendall((text + '\n').encode())
        return sock.recv(65536).decode().strip()


def connect(settings):
    ftp = ftplib.FTP(encoding='utf-8')
    ftp.connect(settings['host'], settings['ftp_port'], timeout=60)
    ftp.login(settings['ftp_user'], settings['ftp_password'])
    # SELF is a per-connection toggle, on by default: a signed file then reads back decrypted (PS5_RetroArch
    # docs/FINDINGS.md, 2026-10-04), so its read-back never matches the host copy.
    for _ in range(2):
        if 'disabled' in ftp.sendcmd('SELF').lower():
            return ftp
    ftp.close()
    raise ftplib.error_reply('the FTP server kept its SELF transfer mode on')


def listing(ftp, directory):
    lines = []
    try:
        ftp.retrlines('LIST ' + directory, lines.append)
    except ftplib.error_perm:
        return None
    names = [line.split(None, 8)[-1] for line in lines if len(line.split(None, 8)) == 9]
    return [name for name in names if name not in ('.', '..')]


def read_remote(ftp, path):
    data = io.BytesIO()
    try:
        ftp.retrbinary('RETR ' + path, data.write)
    except ftplib.error_perm:
        return None
    return data.getvalue()


def make_directories(ftp, directories):
    made = set()
    for directory in sorted(directories):
        parts = directory.strip('/').split('/')
        for depth in range(1, len(parts) + 1):
            path = '/' + '/'.join(parts[:depth])
            if path in made:
                continue
            made.add(path)
            try:
                ftp.mkd(path)
            except ftplib.error_perm:
                pass  # already there; a real failure shows up on the first STOR


class Progress:
    def __init__(self, files, size):
        self.files, self.size = files, size
        self.done_files = self.done_bytes = 0
        self.lock = threading.Lock()
        self.start = time.time()

    def bytes(self, count):
        with self.lock:
            self.done_bytes += count

    def file(self):
        with self.lock:
            self.done_files += 1

    def line(self):
        elapsed = max(time.time() - self.start, 0.001)
        rate = self.done_bytes / elapsed
        left = (self.size - self.done_bytes) / rate if rate else 0
        return (f'{self.done_files}/{self.files} files, {self.done_bytes / 2**20:.0f}/{self.size / 2**20:.0f} MiB, '
                f'{rate / 2**20:.1f} MiB/s, ~{left:.0f}s left')


def send(ftp, game_dir, item, progress):
    destination = f'{game_dir}/{item.remote}'
    with item.open() as source:
        ftp.storbinary('STOR ' + destination, source, BLOCK, lambda block: progress.bytes(len(block)))
    digest = hashlib.sha256()
    ftp.retrbinary('RETR ' + destination, digest.update, BLOCK)
    if digest.hexdigest() != item.sha:
        raise ftplib.Error(f'read-back differs from the host copy: {item.remote}')


def worker(settings, game_dir, work, verified, failures, stop, progress):
    ftp = None
    while not stop.is_set():
        try:
            item = work.get_nowait()
        except queue.Empty:
            break
        error = None
        for _ in range(ATTEMPTS):
            try:
                ftp = ftp or connect(settings)
                send(ftp, game_dir, item, progress)
                error = None
                break
            except (OSError, EOFError, ftplib.Error) as caught:
                error = caught
                try:
                    ftp.close()
                except Exception:
                    pass
                ftp = None
        if error is not None:
            failures.append((item.remote, error))
            stop.set()
            break
        verified[item.remote] = item.sha
        progress.file()
    if ftp:
        try:
            ftp.close()
        except Exception:
            pass


def store_manifest(ftp, game_dir, entries):
    ftp.storbinary('STOR ' + f'{game_dir}/{MANIFEST}', io.BytesIO(
        json.dumps(entries, indent=0, sort_keys=True).encode() + b'\n'), BLOCK)


def put_entry(ftp, entry_dir, entry, write):
    """'current', 'written' (read back), or 'differs' when write is False."""
    if read_remote(ftp, f'{entry_dir}/game.json') == entry:
        return 'current'
    if not write:
        return 'differs'
    make_directories(ftp, {entry_dir})
    ftp.storbinary('STOR ' + f'{entry_dir}/game.json', io.BytesIO(entry))
    if read_remote(ftp, f'{entry_dir}/game.json') != entry:
        raise SystemExit(f'{entry_dir}/game.json readback mismatch')
    return 'written'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', required=True, help='the host folder to send, as it is')
    parser.add_argument('--id', required=True, help='games/<id>, its library entry on the console')
    parser.add_argument('--folder', required=True, help='C:\\Games\\<folder> in Mihawk OS, nested with /')
    parser.add_argument('--game-json', required=True, help='the library entry (game.json) to send')
    parser.add_argument('--jobs', type=int, default=4, help='parallel FTP connections (default 4)')
    parser.add_argument('--extra', action='append', default=[], metavar='FILE',
                        help='also send FILE into the folder under its own name (repeatable)')
    parser.add_argument('--mkdir', action='append', default=[], metavar='DIR',
                        help='also create this empty folder under the folder (repeatable)')
    parser.add_argument('--verify', action='store_true',
                        help='send and read back every file again, not just changed ones (rewrites the whole folder)')
    parser.add_argument('--dry-run', action='store_true', help='print the plan; write nothing to the console')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.id):
        raise SystemExit('--id must be letters, digits, - or _')
    if not re.fullmatch(r'[A-Za-z0-9_-]+(/[A-Za-z0-9_-]+)*', args.folder):
        raise SystemExit('--folder must be letters, digits, - or _, nested with / (launch.args splits words on spaces)')
    game_dir = f'{DATA_ROOT}/prefixes/mihawk/drive_c/Games/{args.folder}'
    windows_dir = 'C:\\Games\\' + args.folder.replace('/', '\\')
    entry_dir = f'{DATA_ROOT}/games/{args.id}'

    items, empty_dirs = plan(args)
    print(f'plan: {len(items)} files, {sum(item.size for item in items) / 2**20:.0f} MiB -> {game_dir}')

    settings = ps5_console.load_settings()
    if args.dry_run:
        return deploy(args, settings, items, empty_dirs, game_dir, windows_dir, entry_dir)
    console_guard.begin_write('deploy-folder', settings)   # refused on a drop, storage damage or a running title
    try:
        return deploy(args, settings, items, empty_dirs, game_dir, windows_dir, entry_dir)
    finally:
        console_guard.end_write(settings)


def deploy(args, settings, items, empty_dirs, game_dir, windows_dir, entry_dir):
    ftp = connect(settings)
    existing = listing(ftp, game_dir)
    manifest = {}
    raw = read_remote(ftp, f'{game_dir}/{MANIFEST}') if existing else None
    if raw:
        manifest = json.loads(raw)
    elif existing:
        raise SystemExit(f'{game_dir} has files that this tool did not put there; refusing to mix')

    todo = [item for item in items if args.verify or manifest.get(item.remote) != item.sha]
    if args.verify:
        manifest = {}
    print(f'{len(items) - len(todo)} files already verified on the console, {len(todo)} to send '
          f'({sum(i.size for i in todo) / 2**20:.0f} MiB)')
    if args.dry_run or not todo:
        status = put_entry(ftp, entry_dir, library_entry(args), not args.dry_run)
        ftp.quit()
        print(f'library entry: {entry_dir}/game.json {status}')
        return 0

    planned = {item.remote: item.sha for item in items}
    # The manifest goes in before anything else, so a folder holding anything always holds one too.
    make_directories(ftp, {game_dir})
    store_manifest(ftp, game_dir, {k: v for k, v in manifest.items() if k in planned})
    make_directories(ftp, {f'{game_dir}/{Path(i.remote).parent.as_posix()}'
                           for i in todo if Path(i.remote).parent.parts} | {f'{game_dir}/{d}' for d in empty_dirs})
    work = queue.Queue()
    for item in todo:
        work.put(item)
    verified, failures, stop = {}, [], threading.Event()
    progress = Progress(len(todo), sum(i.size for i in todo))
    threads = [threading.Thread(target=worker, args=(settings, game_dir, work, verified, failures, stop, progress),
                                daemon=True) for _ in range(max(1, args.jobs))]
    for thread in threads:
        thread.start()
    last_save = time.time()
    try:
        while any(thread.is_alive() for thread in threads):
            for thread in threads:
                thread.join(timeout=5 / len(threads))
            print(progress.line(), flush=True)
            if time.time() - last_save > 60:
                last_save = time.time()
                store_manifest(ftp, game_dir, {k: v for k, v in {**manifest, **verified}.items() if k in planned})
    except KeyboardInterrupt:
        stop.set()
        for thread in threads:
            thread.join()
    finally:
        store_manifest(ftp, game_dir, {k: v for k, v in {**manifest, **verified}.items() if k in planned})
        ftp.quit()
    if failures:
        for remote, error in failures:
            print(f'FAILED {remote}: {error}')
        try:
            print('console control payload:', command(settings, 'procs'))
        except OSError as error:
            print('console control payload is not answering:', error)
        return 1
    if stop.is_set():
        print('interrupted; run again to resume')
        return 1
    print(f'done: {len(todo)} files sent and read back, {progress.done_bytes / 2**20:.0f} MiB in '
          f'{time.time() - progress.start:.0f}s')
    ftp = connect(settings)
    try:
        status = put_entry(ftp, entry_dir, library_entry(args), True)
    finally:
        ftp.quit()
    print(f'library entry: {entry_dir}/game.json -> {windows_dir} ({status})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
