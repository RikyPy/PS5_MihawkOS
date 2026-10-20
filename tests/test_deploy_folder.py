#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/deploy-folder.py against the in-memory console FTP server (tests/fake_ftp.py): SELF off, --source, nested
folders, resuming, the manifest before the first file, the library entry, and the console guard around the writes."""
import contextlib
import ftplib
import hashlib
import importlib.util
import io
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from fake_ftp import FakeServer  # noqa: E402

spec = importlib.util.spec_from_file_location('deploy_folder', ROOT / 'tools/deploy-folder.py')
folder_tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(folder_tool)

SETTINGS = {'host': '192.0.2.7', 'ftp_port': 2121, 'klog_port': 3232, 'ctl_port': 9111,
            'ftp_user': 'anonymous', 'ftp_password': ''}
GAMES = '/data/ps5proton/prefixes/mihawk/drive_c/Games'
SIGNED = bytes.fromhex('4f153d1d')


def pin(manifest):
    """The sha256 of the manifest bytes deploy-folder_tool stores: tools/console-setup.py's pin definition."""
    return hashlib.sha256(json.dumps(manifest, indent=0, sort_keys=True).encode() + b'\n').hexdigest()


class Case(unittest.TestCase):
    def setUp(self):
        self.scratch = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.scratch, True)
        self.server = FakeServer()
        self.commands = []

        def control(settings, text, timeout=60.0):
            self.commands.append(text)
            return {'procs': 'ok procs count=0 pids=', 'sync': 'ok sync ms=2'}.get(text, 'err unknown')

        for patcher in (mock.patch('ftplib.FTP', self.server.connection),
                        mock.patch.object(folder_tool.ps5_console, 'load_settings', lambda: dict(SETTINGS)),
                        mock.patch.object(folder_tool.ps5_console, 'ps5vkctl_command', control),
                        mock.patch.object(folder_tool, 'command', lambda settings, text: 'ok procs count=0 pids='),
                        mock.patch.object(folder_tool.console_guard, 'STATE', self.scratch / 'guard.json')):
            patcher.start()
            self.addCleanup(patcher.stop)

    def tree(self, files):
        source = self.scratch / f'source{len(list(self.scratch.iterdir()))}'
        for relative, data in files.items():
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            if relative.endswith('/'):
                path.mkdir(exist_ok=True)
            else:
                path.write_bytes(data)
        return source

    def entry_file(self, data=b'{"name": "x"}\n'):
        path = self.scratch / f'entry{len(list(self.scratch.iterdir()))}.json'
        path.write_bytes(data)
        return path

    def main(self, *argv):
        out = io.StringIO()
        with mock.patch.object(sys, 'argv', ['deploy-folder_tool.py', *argv]), contextlib.redirect_stdout(out), \
                contextlib.redirect_stderr(out):
            try:
                code = folder_tool.main()
            except SystemExit as stop:
                code = stop.code
        self.output = out.getvalue()
        return code

    def source(self, source, folder, entry, *extra):
        return self.main('--source', str(source), '--folder', folder, '--id', 'g1', '--game-json', str(entry),
                         '--jobs', '1', *extra)

    def console_manifest(self, folder):
        return self.server.files.get(f'{GAMES}/{folder}/{folder_tool.MANIFEST}')


class SelfMode(Case):
    def test_connect_switches_self_off(self):
        ftp = folder_tool.connect(SETTINGS)
        self.assertFalse(ftp.self_mode)
        self.server.self_supported = False
        with self.assertRaises(ftplib.error_reply):
            folder_tool.connect(SETTINGS)

    def test_a_signed_file_reads_back(self):
        source = self.tree({'eboot.bin': SIGNED + b'payload', 'sce_module/libc.prx': SIGNED + b'libc'})
        self.assertEqual(self.source(source, 'Tools/games/T1', self.entry_file()), 0, self.output)
        self.assertEqual(self.server.files[f'{GAMES}/Tools/games/T1/eboot.bin'], SIGNED + b'payload')
        on = self.server.connection()            # SELF left on: the read-back is the decrypted ELF
        on.mkd('/data/x')
        item = folder_tool.Item('a.prx', 8, hashlib.sha256(SIGNED + b'libc').hexdigest(), data=SIGNED + b'libc')
        with self.assertRaisesRegex(ftplib.Error, 'read-back differs'):
            folder_tool.send(on, '/data/x', item, folder_tool.Progress(1, 8))


class Source(Case):
    def test_sends_the_folder_as_it_is(self):
        source = self.tree({'logs/a.txt': b'a', 'cache/b': b'b', 'settings.cfg': b'Path=C:/u/x\n',
                            'empty/': b'', 'eboot.bin': SIGNED + b'e'})
        args = mock.Mock(source=str(source), extra=[], mkdir=[])
        items, empty = folder_tool.plan(args)
        sent = {item.remote: item.sha for item in items}
        self.assertEqual(set(sent), {'logs/a.txt', 'cache/b', 'settings.cfg', 'eboot.bin'})
        cfg = (source / 'settings.cfg').read_bytes()
        self.assertEqual(sent['settings.cfg'], hashlib.sha256(cfg).hexdigest())   # not rewritten
        self.assertEqual(empty, ['empty'])

    def test_case_collision_stops(self):
        source = self.tree({'A.txt': b'1', 'a.txt': b'2'})
        args = mock.Mock(source=str(source), extra=[], mkdir=[])
        with self.assertRaisesRegex(SystemExit, 'differ only by case'):
            folder_tool.plan(args)


class Options(Case):
    def test_nested_folder(self):
        source, entry = self.tree({'program.exe': b'p'}), self.entry_file()
        self.assertEqual(self.source(source, 'Tools/games/T1', entry), 0, self.output)
        self.assertEqual(self.server.files[f'{GAMES}/Tools/games/T1/program.exe'], b'p')
        self.assertEqual(self.server.files['/data/ps5proton/games/g1/game.json'], entry.read_bytes())

    def test_rejected(self):
        source, entry = self.tree({'a': b'a'}), self.entry_file()
        for folder in ('../x', 'a//b', 'a b', '/a', 'a/'):
            self.assertIn('--folder must', str(self.source(source, folder, entry)), folder)
        self.assertEqual(self.main('--source', str(source), '--game-json', str(entry)), 2)
        self.assertEqual(self.main('--source', str(source), '--id', 'g1'), 2)
        self.assertEqual(self.server.writes, [])


class Resume(Case):
    def test_resume_after_a_failed_file(self):
        files = {'a.bin': b'a' * 10, 'sub/b.bin': b'b', 'zz/z.bin': b'z'}   # the failing one is sent last
        source, entry = self.tree(files), self.entry_file()
        self.server.fail_stor[f'{GAMES}/K/zz/z.bin'] = folder_tool.ATTEMPTS
        self.assertEqual(self.source(source, 'K', entry), 1)
        self.assertEqual(set(json.loads(self.console_manifest('K'))), {'a.bin', 'sub/b.bin'})
        self.assertNotIn('/data/ps5proton/games/g1/game.json', self.server.files)
        self.server.writes.clear()
        self.assertEqual(self.source(source, 'K', entry), 0, self.output)
        stored = [path for op, path in self.server.writes if op == 'STOR' and not path.endswith(folder_tool.MANIFEST)]
        self.assertEqual(stored, [f'{GAMES}/K/zz/z.bin', '/data/ps5proton/games/g1/game.json'])
        self.assertEqual(self.server.files['/data/ps5proton/games/g1/game.json'], entry.read_bytes())
        expected = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
        self.assertEqual(hashlib.sha256(self.console_manifest('K')).hexdigest(), pin(expected))

    def test_manifest_before_the_first_file(self):
        source, entry = self.tree({'a.bin': b'a', 'sub/b.bin': b'b'}), self.entry_file()
        # A hard kill right after the folders were made: the manifest is already there.
        with mock.patch.object(folder_tool, 'Progress', side_effect=RuntimeError('killed')):
            with self.assertRaisesRegex(RuntimeError, 'killed'):
                self.source(source, 'K', entry)
        self.assertEqual(json.loads(self.console_manifest('K')), {})
        self.assertIn(f'{GAMES}/K/sub', self.server.dirs)
        self.assertEqual(self.source(source, 'K', entry), 0, self.output)
        self.assertNotIn('refusing to mix', self.output)
        # A kill between the folder and its manifest leaves an empty folder, which is resumed too.
        self.server.fail_stor[f'{GAMES}/L/{folder_tool.MANIFEST}'] = 1
        with self.assertRaises(ConnectionResetError):
            self.source(source, 'L', entry)
        self.assertEqual(self.source(source, 'L', entry), 0, self.output)

    def test_a_foreign_folder_is_refused(self):
        source, entry = self.tree({'a.bin': b'a'}), self.entry_file()
        self.server.put(f'{GAMES}/K/other', b'x')
        self.assertIn('refusing to mix', str(self.source(source, 'K', entry)))


class Entry(Case):
    def test_the_entry_is_kept_current(self):
        files = {'a.bin': b'a'}
        source, entry = self.tree(files), self.entry_file(b'{"name": "new"}\n')
        self.server.put(f'{GAMES}/K/a.bin', b'a')
        self.server.put(f'{GAMES}/K/{folder_tool.MANIFEST}', json.dumps(
            {'a.bin': hashlib.sha256(b'a').hexdigest()}, indent=0, sort_keys=True).encode() + b'\n')
        self.assertEqual(self.source(source, 'K', entry, '--dry-run'), 0)
        self.assertEqual(self.server.writes, [])
        self.assertEqual(self.source(source, 'K', entry), 0, self.output)
        self.assertEqual([op for op, path in self.server.writes if op == 'STOR'], ['STOR'])
        self.assertEqual(self.server.files['/data/ps5proton/games/g1/game.json'], entry.read_bytes())
        self.server.files['/data/ps5proton/games/g1/game.json'] = b'{"name": "old"}'
        self.assertEqual(self.source(source, 'K', entry), 0)
        self.assertEqual(self.server.files['/data/ps5proton/games/g1/game.json'], entry.read_bytes())
        self.server.writes.clear()
        self.assertEqual(self.source(source, 'K', entry), 0)
        self.assertEqual(self.server.writes, [])


class Guard(Case):
    def test_writes_go_through_the_guard(self):
        source, entry = self.tree({'a.bin': b'a'}), self.entry_file()
        self.assertEqual(self.source(source, 'K', entry), 0)
        self.assertEqual(self.commands, ['procs', 'sync'])
        state = json.loads((self.scratch / 'guard.json').read_text())
        self.assertEqual(state['sync'], 'ok sync ms=2')

    def test_storage_damage_refuses_before_any_write(self):
        (self.scratch / 'guard.json').write_text(json.dumps(
            {'launches': 0, 'dropped': None, 'last_launch': 0, 'boot_started': 0,
             'storage': {'run': 'r', 'at': 0, 'lines': ['[IOD] process_completion: error']}}))
        source, entry = self.tree({'a.bin': b'a'}), self.entry_file()
        self.assertIn('storage damage', str(self.source(source, 'K', entry)))
        self.assertEqual(self.server.writes, [])

    def test_a_running_title_refuses(self):
        source, entry = self.tree({'a.bin': b'a'}), self.entry_file()
        busy = lambda settings, text, timeout=60: 'ok procs count=1 pids=99'
        with mock.patch.object(folder_tool.ps5_console, 'ps5vkctl_command', busy):
            self.assertIn('running an application', str(self.source(source, 'K', entry)))
        self.assertEqual(self.server.writes, [])


if __name__ == '__main__':
    unittest.main()
