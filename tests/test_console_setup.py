#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/console-setup.py against an in-memory console: tests/fake_ftp.py, a fake ps5vkctl, websrv and klog, and
fakes of the tools it runs (deploy-wine, run-console).

The prefix session's lines are verbatim from the last one (klog/mihawk-setup-2026-10-14-01*, a Mihawk OS desktop
session with no game), each with the file and line it came from; klog/ itself is never committed.
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import shutil
import socket
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from fake_ftp import FakeServer  # noqa: E402

spec = importlib.util.spec_from_file_location('console_setup', ROOT / 'tools/console-setup.py')
cs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cs)
guard, folder_tool = cs.console_guard, cs.deploy_folder

HOST = '192.0.2.7'
SETTINGS = {'host': HOST, 'ftp_port': 2121, 'klog_port': 3232, 'ctl_port': 9111, 'ftp_user': 'anonymous',
            'ftp_password': ''}
SIGNED = bytes.fromhex('4f153d1d')
DATA = '/data/ps5proton'
ELEVATION = '[MihawkOS] elevation: ok (filesystem access)'          # klog/mihawk-setup-2026-10-14-01.log:328
BASELINE = 'PS5 prefix baseline ready: 118 files updated'              # ...-01-wine-child-118.log:134
ONION = ('<118>[onion_daemon.elf] [587.887] [OnionHEN] info: [lifecycle] Big App started pid=111 appid=8216 '
         'tid=PPSA99170 via=exec source=exec')                         # klog/mihawk-setup-2026-10-14-01.log:94
DAMAGE = 'bfs_device_nsid1_read_ [sid=0x46f00197] -> errno=5, a53error=0x5, val=0x0000'  # a klog of 2026-10-14
# klog/salvage-2026-10-14/resume/lapy-service.log:1
LAPY = ('1\t591240849025\tMARK\tdaemon_start build=7f67784682e58d0d0401bf5508ec9c15cf8f938152b4c2d41997d0c4bccb1478 '
        'firmware=10010000 title=PPSA99170 service=1 require_client_result=1 max_requests=0')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_tree(root, files):
    for name, data in files.items():
        (root / name).parent.mkdir(parents=True, exist_ok=True)
        (root / name).write_bytes(data)
    return root


class FakeKlog:
    """klogsrv's lines for one connection, delivered when the replay is awaited (they arrive after the connect)."""
    world = None

    def __init__(self, settings, path):
        world = FakeKlog.world
        self.pending = world.klogs[min(world.connections, len(world.klogs) - 1)]
        world.connections += 1
        world.klog_paths.append(path)
        self.path, self.lines, self.damage, self.onion = path, 0, [], 0
        self.quiet, self.ended, self.closing = threading.Event(), threading.Event(), False

    def replay(self):
        for line in self.pending:
            self.lines += 1
            self.damage += guard.storage_damage([line])
            self.onion += 'OnionHEN' in line
        if self.path and self.pending:
            with self.path.open('a') as output:
                output.write('\n'.join(self.pending) + '\n')
        self.pending = []

    def close(self):
        self.closing = True


class World:
    def __init__(self):
        self.server = FakeServer()
        self.loaded, self.pid, self.busy, self.ctl = False, 109, 0, None
        self.commands, self.posts, self.tools, self.klog_paths = [], [], [], []
        self.klogs, self.connections = [['<6>boot replay line', '<6>another']], 0
        self.prefix = {'stamp': b'1760000000 wow64\r\n', 'klog': [ELEVATION], 'child': [BASELINE], 'passed': True,
                       'code': 0, 'lapy': LAPY}
        self.wine = (0, 'Uploaded and verified {n} files; prefix probe: created')

    def command(self, settings, text, timeout=60.0):
        self.commands.append(text)
        if not self.loaded:
            raise SystemExit(f'ps5vkctl: {settings["host"]}:9111 did not answer ([Errno 111] Connection refused)')
        return {'ping': f'ok ps5vkctl 2 pid={self.pid}', 'sync': 'ok sync ms=3',
                'procs': f'ok procs count={self.busy} pids={"99" if self.busy else ""}'}.get(text, 'err unknown')

    def port_state(self, host, port):
        if port == SETTINGS['ctl_port']:
            return 'open' if self.loaded else (self.ctl or 'refused')
        return 'open'

    def post(self, settings, elf):
        self.posts.append(elf)
        self.loaded = True

    def tool(self, run, *argv):
        self.tools.append(list(argv))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = {'tools/deploy-wine.py': self.deploy_wine,
                    'tools/run-console.py': self.run_console}[argv[0]](list(argv[1:]))
        lines = [run.mask(line) for line in out.getvalue().splitlines()]
        run.connect()
        return code, lines

    def deploy_wine(self, args):
        staged = json.loads(cs.STAGED.read_text())
        runtime = {k: v for k, v in staged.items() if k.startswith('runtime/') or k == 'run/proton-payload-child.elf'}
        code, last = self.wine
        if code:
            print(last)
            return code
        for key in runtime:
            self.server.put(f'{DATA}/{key}', (cs.STAGED.parent / key).read_bytes())
        self.server.put(f'{DATA}/runtime/ge11-7/deploy-manifest.json', json.dumps(runtime).encode())
        self.server.dirs |= {f'{DATA}/prefixes', f'{DATA}/prefixes/probe'}
        print(last.format(n=len(staged)))
        return 0

    def run_console(self, args):
        name, effect = args[args.index('--name') + 1], self.prefix
        (cs.KLOG / f'{name}.log').write_text('\n'.join(['[MihawkOS] starts', *effect['klog']]) + '\n')
        (cs.KLOG / f'{name}-title.log').write_text('[MihawkOS] starts (standard error to klog: yes)\n')
        (cs.KLOG / f'{name}-wine-child-118.log').write_text('\n'.join(effect['child']) + '\n')
        (cs.EVIDENCE / f'{name}.json').write_text(json.dumps({'run': name, 'passed': effect['passed'],
                                                               'result': 'finished'}))
        self.server.put(f'{DATA}/prefixes/mihawk/.update-timestamp', effect['stamp'])
        self.server.put(f'{DATA}/logs/lapy-service.log', (effect['lapy'] + '\n').encode())
        return effect['code']

class Case(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        world = self.world = World()
        FakeKlog.world = world
        (self.tmp / '.deps').mkdir()
        self.dist = write_tree(self.tmp / 'dist/PPSA99170', {
            'eboot.bin': SIGNED + b'title', 'sce_module/libc.prx': SIGNED + b'libc', 'sce_sys/param.json': b'{}',
            'assets/a.png': b'png'})
        staged = {'runtime/ge11-7/lib/a.so': b'so', 'run/proton-payload-child.elf': b'\x7fELF child',
                  'prefixes/probe/user.reg': b'reg'}
        write_tree(self.tmp / 'staged', staged)
        (self.tmp / 'staged/manifest.json').write_text(json.dumps({k: sha(v) for k, v in staged.items()}))
        (self.tmp / 'evidence').mkdir()
        self.guard_path = self.tmp / 'guard.json'
        self.guard_path.write_text(json.dumps({'launches': 0, 'dropped': None, 'last_launch': 0.0,
                                               'boot_started': 1760400000.0, 'mode': 'lapy'}))
        for patcher in (
                mock.patch('ftplib.FTP', world.server.connection),
                mock.patch.object(cs.ps5_console, 'load_settings', lambda: dict(SETTINGS)),
                mock.patch.object(cs.ps5_console, 'ps5vkctl_command', world.command),
                mock.patch.object(folder_tool, 'command', lambda settings, text: 'ok procs count=0 pids='),
                mock.patch.object(cs, 'port_state', world.port_state),
                mock.patch.object(cs, 'post_elf', world.post),
                mock.patch.object(cs, 'build_control', lambda: b'\x7fELF ps5vkctl'),
                mock.patch.object(cs, 'KlogStream', FakeKlog),
                mock.patch.object(cs.Run, 'tool', lambda run, *argv: world.tool(run, *argv)),
                mock.patch.object(cs, 'build_state', lambda: {'matches_tree': True, 'head': 'f129706', 'time': 't'}),
                mock.patch.object(cs.time, 'sleep', lambda seconds: None),
                mock.patch.object(cs, 'DIST', self.dist),
                mock.patch.object(cs, 'STAGED', self.tmp / 'staged/manifest.json'),
                mock.patch.object(cs, 'EVIDENCE', self.tmp / 'evidence'),
                mock.patch.object(cs, 'KLOG', self.tmp / 'klog'),
                mock.patch.object(guard, 'STATE', self.guard_path)):
            patcher.start()
            self.addCleanup(patcher.stop)

    def setup(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = cs.main(list(argv))
        self.output = out.getvalue()
        return code

    def evidence(self):
        return json.loads(sorted((self.tmp / 'evidence').glob('console-setup-2*.json'))[-1].read_text())

    def step(self, number):
        return [line for line in self.output.splitlines() if line.startswith(number + ' ')][0]

    def guard_state(self):
        return guard.load(self.guard_path)

    def ready_console(self):
        self.assertEqual(self.setup(), 0, self.output)
        world = self.world
        world.server.writes.clear()
        world.server.reads.clear()
        world.tools.clear()
        world.posts.clear()


class Check(Case):
    def test_an_empty_console_changes_nothing(self):
        guard_bytes = self.guard_path.read_bytes()
        self.assertEqual(self.setup('--check'), 2, self.output)
        states = {number: self.step(number).split(': ')[1] for number in [f'S{n}' for n in range(11)]}
        self.assertEqual(states, {'S0': 'done', 'S1': 'done', 'S2': 'done', 'S3': 'todo', 'S4': 'done',
                                  'S5': 'done', 'S6': 'todo', 'S7': 'todo', 'S8': 'todo', 'S9': 'todo',
                                  'S10': 'skipped'})
        world = self.world
        self.assertEqual((world.server.writes, world.posts, world.tools), ([], [], []))
        self.assertEqual(self.guard_path.read_bytes(), guard_bytes)
        self.assertEqual(list((self.tmp / 'evidence').iterdir()), [])
        self.assertFalse((self.tmp / 'klog').exists())
        self.assertEqual(world.klog_paths, [None])

    def test_a_running_title_stops_every_read(self):
        self.world.loaded, self.world.busy = True, 1
        self.assertEqual(self.setup('--check'), 1)
        self.assertIn('blocked: a title is running', self.step('S3'))
        self.assertNotIn('S4 klog', self.output)
        self.assertEqual(self.world.server.reads, [])
        self.assertNotIn('kill', self.world.commands)


class Apply(Case):
    def test_a_fresh_console_in_order(self):
        self.assertEqual(self.setup(), 0, self.output)
        world, server = self.world, self.world.server
        self.assertEqual(world.posts, [b'\x7fELF ps5vkctl'])
        prefix = world.tools[1]
        self.assertEqual(world.tools[0], ['tools/deploy-wine.py'])
        self.assertEqual(prefix[:2] + prefix[3:], ['tools/run-console.py', '--name', '--profile', 'mihawk-os',
                                                   '--seconds', '150'])
        self.assertRegex(prefix[2], r'^console-setup-prefix-\d{4}-\d\d-\d\d-01$')
        self.assertEqual(len(world.tools), 2)
        stores = [path for op, path in server.writes if op in ('STOR', 'RNFR')]
        self.assertLess(stores.index('/data/homebrew/dev.conf'),
                        stores.index('/data/homebrew/PPSA99170/sce_module/.libc.prx.upload'))
        renamed = [path for op, path in server.writes if op == 'RNFR']   # eboot.bin and param.json are published last
        self.assertEqual(renamed[-2:], ['/data/homebrew/PPSA99170/.eboot.bin.upload',
                                        '/data/homebrew/PPSA99170/sce_sys/.param.json.upload'])
        for name, data in {'eboot.bin': SIGNED + b'title', 'sce_module/libc.prx': SIGNED + b'libc'}.items():
            self.assertEqual(server.files[f'/data/homebrew/PPSA99170/{name}'], data)
        evidence = self.evidence()
        self.assertEqual({k: evidence[k] for k in ('result', 'firmware', 'title_files')},
                         {'result': 'ready', 'firmware': '10010000', 'title_files': 4})
        self.assertEqual(evidence['read_back'], {'files': 2, 'mismatches': 0})
        self.assertEqual(evidence['prefix_session'], prefix[2])
        self.assertEqual(evidence['klog'], {'connections': 2, 'lines': 4, 'storage_damage': 0, 'onionhen': 0})
        state = self.guard_state()
        self.assertEqual((state['ps5vkctl_pid'], state['sync']), (109, 'ok sync ms=3'))
        self.assertTrue(state['deploys']['title']['verified'] and state['deploys']['runtime']['verified'])

    def test_a_ready_console_is_left_alone(self):
        self.ready_console()
        self.assertEqual(self.setup(), 0, self.output)
        world = self.world
        self.assertEqual((world.tools, world.posts, world.server.writes), ([], [], []))
        self.assertEqual(self.evidence()['result'], 'ready')
        self.assertEqual(self.setup('--check'), 0, self.output)
        self.assertEqual(self.setup('--check', '--deep'), 0, self.output)
        self.assertIn('S10 read-back: done: 2 files match', self.output)

    def test_the_prefix_session_is_alone(self):
        for change, reason in ((dict(stamp=b'1760000000\n'), 'unfinished'),
                               (dict(child=['wine-child: census']), "without 'PS5 prefix baseline ready'"),
                               (dict(klog=[ELEVATION, ONION]), 'OnionHEN was loaded'),
                               (dict(passed=False, code=1), 'did not pass'),
                               (dict(child=[BASELINE, DAMAGE]), 'storage-damage lines in the logs'),
                               (dict(lapy=LAPY.replace('10010000', '10020000')), 'firmware 10020000')):
            with self.subTest(reason=reason):
                self.setUp()
                self.world.prefix.update(change)
                self.assertEqual(self.setup(), 1, self.output)
                self.assertIn(reason, self.step('S9'))
                self.assertEqual([argv[0] for argv in self.world.tools],
                                 ['tools/deploy-wine.py', 'tools/run-console.py'])
                self.assertEqual(self.evidence()['stopped_at'], 'S9')

    def test_damage_in_a_prefix_log_blocks_the_console(self):
        self.world.prefix['child'] = [BASELINE, DAMAGE]
        self.assertEqual(self.setup(), 1)
        storage = self.guard_state()['storage']
        self.assertTrue(storage['run'].startswith('console-setup-prefix-'))
        self.assertEqual(storage['lines'], [DAMAGE])


class Stops(Case):
    def stops_at(self, number, reason):
        self.assertEqual(self.setup(), 1, self.output)
        self.assertIn(reason, self.step(number))
        self.assertEqual(self.evidence()['stopped_at'], number)

    def test_a_dropped_console(self):
        state = self.guard_state()
        state['dropped'] = {'why': 'ps5vkctl refused', 'at': time.time()}
        self.guard_path.write_text(json.dumps(state))
        self.stops_at('S2', 'dropped off the network')
        self.assertEqual(self.world.posts, [])

    def test_ps5vkctl_gone_this_boot(self):
        state = self.guard_state()
        state['ps5vkctl_pid'] = 77
        self.guard_path.write_text(json.dumps(state))
        self.stops_at('S3', 'answered this boot as pid 77')
        self.assertEqual(self.world.posts, [])

    def test_a_hung_ps5vkctl_is_never_loaded_twice(self):
        for port in ('open', 'silent'):
            with self.subTest(port=port):
                self.setUp()
                self.world.ctl = port
                self.stops_at('S3', 'loaded and hung')
                self.assertEqual(self.world.posts, [])

    def test_another_firmware_stops_before_any_write(self):
        self.world.loaded = True
        self.world.server.put(f'{DATA}/logs/lapy-service.log', (LAPY.replace('10010000', '10020000') + '\n').encode())
        self.stops_at('S3', 'firmware 10020000')
        self.assertEqual((self.world.server.writes, self.world.tools), ([], []))

    def test_a_running_title(self):
        self.world.loaded, self.world.busy = True, 1
        self.stops_at('S3', 'never force-closes')
        self.assertNotIn('kill', self.world.commands)

    def test_damage_in_the_replay_stops_before_the_post(self):
        self.world.klogs = [['<6>a', DAMAGE]]
        self.stops_at('S3', 'storage-damage lines in the klog')
        self.assertEqual(self.world.posts, [])
        self.assertEqual(self.guard_state()['storage']['lines'], [DAMAGE])

    def test_damage_or_onionhen_with_ps5vkctl_loaded(self):
        for line, reason in ((DAMAGE, 'storage-damage'), (ONION, 'OnionHEN is loaded')):
            with self.subTest(reason=reason):
                self.setUp()
                self.world.loaded, self.world.klogs = True, [['<6>a', line]]
                self.stops_at('S4', reason)

    def test_another_dev_conf_is_kept(self):
        self.world.server.put('/data/homebrew/dev.conf', b'DEV_SERVER=192.0.2.50\nDEV_PORT=9000\n')
        self.stops_at('S6', 'change it by hand')
        self.assertEqual(self.world.server.files['/data/homebrew/dev.conf'], b'DEV_SERVER=192.0.2.50\nDEV_PORT=9000\n')
        self.assertNotIn('192.0.2.50', self.output)

    def test_a_title_that_still_differs(self):
        self.world.server.corrupt.add('/data/homebrew/PPSA99170/.eboot.bin.upload')
        self.stops_at('S7', '1 title files still differ')
        self.assertFalse(self.guard_state()['deploys']['title']['verified'])

    def test_deploy_wine_fails(self):
        self.world.wine = (1, f'Readback mismatch: runtime/ge11-7/lib/a.so at {HOST}')
        self.stops_at('S8', 'tools/deploy-wine.py exited 1')

    def test_damage_after_the_prefix_session(self):
        self.world.klogs = [['<6>a'], ['<6>b', DAMAGE]]
        self.stops_at('S10', 'storage-damage lines in the klog')
        self.assertEqual(len(self.world.tools), 2)

    def test_a_file_that_rots(self):
        self.ready_console()
        self.world.server.files[f'{DATA}/run/proton-payload-child.elf'] = b'rotten'
        self.stops_at('S10', '1 of 2 files do not match')
        self.assertFalse(self.guard_state()['deploys']['runtime']['verified'])
        self.assertEqual(self.world.server.writes, [])

    def test_unreachable(self):
        with mock.patch.object(cs, 'port_state', lambda host, port: 'silent'):
            self.stops_at('S1', 'DHCP')
        self.assertEqual(self.world.server.reads, [])


class Hygiene(Case):
    def test_no_address_leaks(self):
        self.world.wine = (1, f'Readback mismatch at {HOST}')
        self.assertEqual(self.setup(), 1)
        self.assertNotIn(HOST, self.output)
        self.assertNotIn(HOST, (sorted((self.tmp / 'evidence').glob('console-setup-2*.json'))[-1]).read_text())
        self.assertIn('<console>', self.output)

    def test_no_pc_path_in_the_evidence(self):
        self.assertEqual(self.setup(), 0, self.output)
        text = sorted((self.tmp / 'evidence').glob('console-setup-2*.json'))[-1].read_text()
        self.assertNotIn(str(self.tmp), text)

    def test_a_file_error_names_no_path_in_the_evidence(self):
        (self.tmp / 'staged/manifest.json').chmod(0)   # S8 cannot read it: an OSError that carries the path
        self.addCleanup((self.tmp / 'staged/manifest.json').chmod, 0o644)
        self.assertEqual(self.setup(), 1)
        text = sorted((self.tmp / 'evidence').glob('console-setup-2*.json'))[-1].read_text()
        self.assertIn('PermissionError', text)
        self.assertNotIn(str(self.tmp), text)


class Stream(unittest.TestCase):
    """The real KlogStream against a local klogsrv stand-in."""

    def test_replay_then_live_lines(self):
        server = socket.create_server(('127.0.0.1', 0))
        port = server.getsockname()[1]
        go = threading.Event()

        def serve():
            connection, _ = server.accept()
            with connection:
                connection.sendall(b'<6>replay one\n<6>replay at 192.0.2.9\n')
                go.wait(5)
                connection.sendall((DAMAGE + '\n' + ONION + '\n').encode())
                time.sleep(0.3)

        threading.Thread(target=serve, daemon=True).start()
        path = Path(tempfile.mkdtemp()) / 'run.log'
        self.addCleanup(shutil.rmtree, path.parent, True)
        with mock.patch.object(guard, 'BOOT_QUIET', 0.2), mock.patch.object(guard, 'BOOT_LIMIT', 5):
            stream = cs.KlogStream({'host': '127.0.0.1', 'klog_port': port}, path)
            stream.replay()
            self.assertEqual((stream.lines, stream.damage, stream.onion), (2, [], 0))
            go.set()
            self.assertTrue(stream.ended.wait(5))
            self.assertEqual((stream.lines, stream.damage, stream.onion), (4, [DAMAGE], 1))
            stream.close()
        text = path.read_text()
        self.assertIn('replay at <address>', text)
        self.assertNotIn('192.0.2.9', text)
        server.close()


if __name__ == '__main__':
    unittest.main()
