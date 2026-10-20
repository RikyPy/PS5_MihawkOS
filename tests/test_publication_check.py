#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/publication-check.py on small planted repositories: each category it promises is caught in the tree or in
history, nothing planted is ever printed, a clean tree passes, and --allow accepts a named finding. The planted values
are assembled at run time, so this file holds none of them."""
import hashlib
import importlib.util
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'tools/publication-check.py'
_spec = importlib.util.spec_from_file_location('stage_licences', ROOT / 'tools/stage-licences.py')
TOOL_LICENCES = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(TOOL_LICENCES)
_spec = importlib.util.spec_from_file_location('mihawk_wallpaper', ROOT / 'tools/make-mihawk-wallpaper.py')
WALLPAPER = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(WALLPAPER)
CONSOLE = '.'.join(['192', '168', '77', '41'])
SECRET = 'pl' + 'anted-ftp-' + 'secret'
TOKEN = 'gh' + 'p_' + 'Q' * 36
EMAIL = 'someone' + '@' + 'personal.example.invalid'


def git(repo, *args):
    subprocess.run(['git', '-C', str(repo), '-c', 'user.name=t', '-c', 'user.email=t@example.invalid',
                    '-c', 'core.hooksPath=/dev/null', '-c', 'commit.gpgsign=false', *args],
                   check=True, capture_output=True)


class PublicationCheck(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.base = Path(self.folder.name)
        self.repo = self.base / 'repo'
        self.repo.mkdir()
        git(self.repo, 'init', '-q')
        self.env = self.base / 'console.env'
        self.env.write_text(f'PS5_HOST={CONSOLE}\nPS5_FTP_PASSWORD={SECRET}\n')
        (self.repo / 'clean.c').write_text('/* SPDX-License-Identifier: GPL-3.0-or-later */\nint x;\n')

    def tearDown(self):
        self.folder.cleanup()

    def run_tool(self, *args):
        env = dict(os.environ, PW_PRIVACY_EXTRA_SHA256=hashlib.sha256(EMAIL.lower().encode()).hexdigest())
        result = subprocess.run(['python3', str(TOOL), *args, '--repo', str(self.repo), '--env', str(self.env)],
                                capture_output=True, text=True, env=env)
        for value in (CONSOLE, SECRET, TOKEN, EMAIL):
            self.assertNotIn(value, result.stdout + result.stderr)
        return result.returncode, result.stdout

    def failed(self, output):
        return {line.split(':')[0].split(None, 1)[1] for line in output.splitlines() if line.strip().startswith('FAIL')}

    def test_a_clean_tree_passes(self):
        code, output = self.run_tool('tree')
        self.assertEqual(code, 0, output)
        self.assertIn('verdict: clean', output)

    def test_every_tree_category_is_caught(self):
        mac, serial, user = ':'.join(['aa', 'bb', 'cc', 'dd', 'ee', '01']), 'serial' + '=ABCD123456', 'Some' + 'one'
        (self.repo / 'notes.txt').write_text(f'host {CONSOLE}\npass {SECRET}\n{TOKEN}\nmail {EMAIL}\n'
                                              f'{mac}\n{serial}\nC:\\Users\\{user}\\x\n')
        (self.repo / 'klog').mkdir()
        (self.repo / 'klog/boot.log').write_text('log\n')
        (self.repo / 'game.iso').write_bytes(b'\0' * 0x8001 + b'CD001' + b'\0' * 16)
        (self.repo / 'tool.c').write_text('int main(void) { return 0; }\n')
        (self.repo / 'notes2.md').write_text('a run of ' + 'Hollow' + ' Knight\n' + 'PPSA' + '12345\n')
        code, output = self.run_tool('tree')
        self.assertEqual(code, 1, output)
        self.assertEqual(self.failed(output), {
            'console address (.env)', 'FTP credential (.env)', 'token or key', 'blocked personal e-mail',
            'MAC address', 'serial or console id', 'Windows user folder', 'tracked under klog/ or .claude/',
            'binary that could hold game data, firmware or keys', 'source file without SPDX',
            'named game or launcher'})
        self.assertIn('named game or launcher: 2 in 1 places: notes2.md', output)

    def test_names_that_are_not_games_and_the_trademark_notice_pass(self):
        (self.repo / 'prefix.txt').write_text('C:\\users\\Mihawk\\Documents, C:\\users\\steamuser\\Documents, a steaming bowl, PPSA99170\n')
        (self.repo / 'LEGAL.md').write_text('| St' + 'eam, Proton | Valve Corporation |\n')
        code, output = self.run_tool('tree')
        self.assertEqual(code, 0, output)
        self.assertIn('allowed  named game or launcher: 1 in LEGAL.md (the trademark table)', output)

    def test_a_build_must_not_carry_what_never_ships(self):
        (self.repo / '.gitignore').write_text('.deps/\ndist/\n')  # build output is ignored, as in this repository
        (self.repo / 'dist/PPSA99170').mkdir(parents=True)  # a title folder without licenses/
        (self.repo / 'dist/PPSA99170/eboot.bin').write_bytes(b'\0')
        runtime = self.repo / '.deps/staged/runtime/ge11-7'
        (runtime / 'share/licenses').mkdir(parents=True)
        wallpaper = runtime / 'lib/wine/prefix/drive_c/windows/web/wallpaper'
        wallpaper.mkdir(parents=True)
        Image.new('RGB', (1920, 1080), (40, 90, 200)).save(wallpaper / 'proton.bmp')  # some other wallpaper
        modules = runtime / 'lib/wine/x86_64-windows'
        modules.mkdir(parents=True)
        microsoft = 'Microsoft Corporation'.encode('utf-16-le')
        (modules / 'd3dcompiler_47.dll').write_bytes(b'MZ' + b'\0' * 126 + microsoft)  # a native one
        (modules / 'd3d9.dll').write_bytes(b'MZ' + b'\0' * 62 + b'Wine builtin DLL' + b'\0' * 62 + microsoft)  # Wine's
        (modules / 'wine-probe.exe').write_bytes(b'MZ')
        (self.repo / 'dist/release').mkdir(parents=True)
        (self.repo / 'dist/release/mount.elf').write_bytes(b'\x7fELF' + b'\0' * 60 + b'ShadowMountPlus 1.7')
        code, output = self.run_tool('tree')
        self.assertEqual(code, 1, output)
        self.assertEqual(self.failed(output), {'licence text missing beside a build', 'file that must never ship in a build'})
        never = next(line for line in output.splitlines() if line.strip().startswith('FAIL') and 'never ship' in line)
        self.assertIn('never ship in a build: 4 in 4 places', never)  # wallpaper, native DLL, probe, ShadowMountPlus
        self.assertIn('dist/release/mount.elf (ShadowMountPlus)', output)
        self.assertIn('a wallpaper that is not Aero Meadow', output)
        self.assertIn('title: no licenses/ folder', output)
        self.assertNotIn('d3d9.dll', output)

    def test_a_build_with_aero_meadow_passes(self):
        (self.repo / '.gitignore').write_text('.deps/\ndist/\n')
        wallpaper = self.repo / '.deps/staged/runtime/ge11-7/lib/wine/prefix/drive_c/windows/web/wallpaper'
        wallpaper.mkdir(parents=True)
        WALLPAPER.wallpaper(480, 270).resize((1920, 1080), Image.Resampling.LANCZOS).save(wallpaper / 'proton.bmp')
        licences = self.repo / '.deps/staged/runtime/ge11-7/share/licenses'
        for name in TOOL_LICENCES.STAGE_WINE + list(TOOL_LICENCES.RUNTIME):
            (licences / name).parent.mkdir(parents=True, exist_ok=True)
            (licences / name).write_text('text\n')
        code, output = self.run_tool('tree')
        self.assertEqual(code, 0, output)

    def test_history_keeps_what_the_tree_dropped(self):
        (self.repo / 'deploy.json').write_text(f'{{"host": "{CONSOLE}"}}\n')
        git(self.repo, 'add', '-A')
        git(self.repo, 'commit', '-q', '-m', 'add')
        (self.repo / 'deploy.json').write_text('{"host": "<masked>"}\n')
        git(self.repo, 'commit', '-q', '-am', 'mask')
        self.assertEqual(self.run_tool('tree')[0], 0)
        code, output = self.run_tool('history')
        self.assertEqual(code, 1, output)
        self.assertEqual(self.failed(output), {'console address (.env)'})
        self.assertIn('deploy.json', output)  # where it is, by commit and path
        code, output = self.run_tool('history', '--allow', 'console address (.env)')
        self.assertEqual(code, 0, output)
        self.assertIn('accepted', output)


if __name__ == '__main__':
    unittest.main()
