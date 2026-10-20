# SPDX-License-Identifier: GPL-3.0-or-later
"""An in-memory stand-in for the console's FTP server (ftpsrv), for the host tests of the tools that write to it.

Only what those tools call, with the console's behaviour where it differs from a plain server:
  SELF   a per-connection toggle, on by default; while on, RETR of a signed file (4f153d1d or 5414f5ee) serves the
         decrypted ELF, which is another length (PS5_RetroArch docs/FINDINGS.md, 2026-10-04). The first SELF answers
         '226 SELF transfer mode disabled'.
  MKD    refuses an existing folder and a missing parent with 550 (the 2026-10-14 hand upload needed every parent).
  DELE   completes with 226: sendcmd returns it, ftplib's delete() would call it an error.
  LIST   9 fields, '.' and '..' first ('drwxrwxrwx 2 0 0 65536 Oct 14 16:15 PPSA99170', 2026-10-14).
  STOR   to a missing folder is refused with 550.
FakeServer.writes records every STOR, MKD, DELE and RNFR as (command, path); reads records every RETR path.
"""
import ftplib
import threading

SIGNED = (bytes.fromhex('4f153d1d'), bytes.fromhex('5414f5ee'))


class FakeServer:
    def __init__(self, self_supported=True):
        self.files, self.dirs = {}, {'/', '/data'}
        self.writes, self.reads = [], []
        self.fail_stor = {}        # path -> how many more STORs of it fail with a reset connection
        self.corrupt = set()       # paths whose STOR keeps the bytes with the last one flipped
        self.self_supported = self_supported
        self.lock = threading.Lock()

    def connection(self, *args, **kwargs):
        return FakeFTP(self)

    def put(self, path, data):
        """State that was already on the console (no write recorded); parents are made."""
        parts = path.strip('/').split('/')
        for depth in range(1, len(parts)):
            self.dirs.add('/' + '/'.join(parts[:depth]))
        self.files[path] = bytes(data)


def parent(path):
    return path.rsplit('/', 1)[0] or '/'


class FakeFTP:
    def __init__(self, server):
        self.server = server
        self.self_mode = True

    def connect(self, host, port, timeout=None):
        return '220 ftpsrv'

    def login(self, user='', password=''):
        return '230 logged in'

    def sendcmd(self, command):
        verb, _, argument = command.partition(' ')
        if verb == 'SELF' and self.server.self_supported:
            self.self_mode = not self.self_mode
            return f"226 SELF transfer mode {'enabled' if self.self_mode else 'disabled'}"
        if verb == 'DELE':
            with self.server.lock:
                if argument not in self.server.files:
                    raise ftplib.error_perm('550 No such file or directory')
                del self.server.files[argument]
                self.server.writes.append(('DELE', argument))
            return '226 File deleted'
        return '200 OK'

    def storbinary(self, command, source, blocksize=8192, callback=None, rest=None):
        path = command.split(' ', 1)[1]
        data = bytearray()
        while True:
            block = source.read(blocksize)
            if not block:
                break
            data += block
            if callback:
                callback(block)
        with self.server.lock:
            if self.server.fail_stor.get(path, 0) > 0:
                self.server.fail_stor[path] -= 1
                raise ConnectionResetError('connection reset')
            if parent(path) not in self.server.dirs:
                raise ftplib.error_perm('550 No such file or directory')
            if path in self.server.corrupt and data:
                data[-1] ^= 1
            self.server.files[path] = bytes(data)
            self.server.writes.append(('STOR', path))
        return '226 Transfer complete'

    def retrbinary(self, command, callback, blocksize=8192, rest=None):
        path = command.split(' ', 1)[1]
        with self.server.lock:
            if path not in self.server.files:
                raise ftplib.error_perm('550 No such file or directory')
            data = self.server.files[path]
            self.server.reads.append(path)
        if self.self_mode and data[:4] in SIGNED:
            data = b'\x7fELF' + data[4:] + b'decoded'
        for start in range(0, len(data), blocksize):
            callback(data[start:start + blocksize])
        return '226 Transfer complete'

    def retrlines(self, command, callback=None):
        verb, _, directory = command.partition(' ')
        assert verb == 'LIST', command
        directory = directory.rstrip('/') or '/'
        with self.server.lock:
            if directory not in self.server.dirs:
                raise ftplib.error_perm('550 No such file or directory')
            prefix = directory.rstrip('/') + '/'
            folders = sorted(d[len(prefix):] for d in self.server.dirs if d.startswith(prefix) and
                             '/' not in d[len(prefix):] and d != directory)
            files = sorted((p[len(prefix):], len(v)) for p, v in self.server.files.items()
                           if p.startswith(prefix) and '/' not in p[len(prefix):])
        lines = ['drwxrwxrwx 2 0 0 65536 Oct 14 16:15 .', 'drwxrwxrwx 2 0 0 65536 Oct 14 16:15 ..']
        lines += [f'drwxrwxrwx 2 0 0 65536 Oct 14 16:15 {name}' for name in folders]
        lines += [f'-rwxrwxrwx 1 0 0 {size} Oct 14 16:21 {name}' for name, size in files]
        for line in lines:
            callback(line)
        return '226 Transfer complete'

    def mkd(self, path):
        with self.server.lock:
            if path in self.server.dirs or path in self.server.files or parent(path) not in self.server.dirs:
                raise ftplib.error_perm('550 Cannot create directory')
            self.server.dirs.add(path)
            self.server.writes.append(('MKD', path))
        return path

    def cwd(self, path):
        if path not in self.server.dirs:
            raise ftplib.error_perm('550 No such file or directory')
        return '250 OK'

    def rename(self, source, destination):
        with self.server.lock:
            if source not in self.server.files or parent(destination) not in self.server.dirs:
                raise ftplib.error_perm('550 Rename failed')
            self.server.files[destination] = self.server.files.pop(source)
            self.server.writes.append(('RNFR', source))
        return '250 OK'

    def quit(self):
        return '221 Goodbye'

    def close(self):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
