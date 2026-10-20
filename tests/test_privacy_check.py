#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The privacy guard (tools/privacy_check.py) and its hooks, in a throwaway repository, against a dummy address that
PW_PRIVACY_EXTRA_SHA256 blocks. Every run's output is checked not to carry the address it refuses."""
import hashlib
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECK = ROOT / 'tools' / 'privacy_check.py'
HOOKS = ROOT / 'tools' / 'git-hooks'
DUMMY = 'blocked.person@example.invalid'
ZERO = '0' * 40


class PrivacyCheck(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name)
        self.env = dict(os.environ, HOME=self.tmp.name, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM='1',
                        PW_PRIVACY_EXTRA_SHA256=hashlib.sha256(DUMMY.encode()).hexdigest())
        for name in ('GIT_AUTHOR_EMAIL', 'GIT_COMMITTER_EMAIL', 'GIT_AUTHOR_NAME', 'GIT_COMMITTER_NAME', 'GIT_DIR'):
            self.env.pop(name, None)
        self.git('init', '-q', '-b', 'main')
        self.git('config', 'user.name', 'Tester')
        self.git('config', 'user.email', '1+tester@users.noreply.github.com')
        self.git('remote', 'add', 'origin', str(self.repo / 'nowhere.git'))

    def tearDown(self):
        self.tmp.cleanup()

    def git(self, *args, env=None):
        return subprocess.run(['git', *args], cwd=self.repo, env=env or self.env, check=True, capture_output=True,
                              text=True).stdout.strip()

    def commit(self, path, text, env=None):
        (self.repo / path).parent.mkdir(parents=True, exist_ok=True)
        (self.repo / path).write_text(text)
        self.git('add', '-A')
        self.git('commit', '-q', '--no-verify', '-m', f'change {path}', env=env)
        return self.git('rev-parse', 'HEAD')

    def run_check(self, *args, stdin='', env=None, command=None):
        result = subprocess.run(command or ['python3', str(CHECK), *args], cwd=self.repo, env=env or self.env,
                                input=stdin, capture_output=True, text=True)
        self.assertNotIn(DUMMY, (result.stdout + result.stderr).lower())
        return result.returncode, result.stderr

    def stage(self, path, text):
        (self.repo / path).parent.mkdir(parents=True, exist_ok=True)
        (self.repo / path).write_bytes(text if isinstance(text, bytes) else text.encode())
        self.git('add', '-A')

    def push(self, tip, base=None, ref='refs/heads/main'):
        if base:
            self.git('update-ref', 'refs/remotes/origin/main', base)
        return self.run_check('push', 'origin', 'url', stdin=f'refs/heads/main {tip} {ref} {base or ZERO}\n')

    def test_staged_content_is_refused_by_file_and_line(self):
        self.stage('notes.txt', 'one\ncontact: Blocked.Person@Example.Invalid\n')
        code, err = self.run_check('staged')
        self.assertEqual(code, 1)
        self.assertIn('notes.txt:2: a blocked personal address', err)

    def test_the_release_key_is_refused_by_folder_and_content(self):
        pem = '-----BEGIN ' + 'PRIVATE KEY-----\nMC4CAQAwBQYDK2VwBCIEIA==\n-----END ' + 'PRIVATE KEY-----\n'
        self.stage('keys/release-ed25519.pem', 'not even a key\n')
        code, err = self.run_check('staged')
        self.assertEqual(code, 1)
        self.assertIn('keys/release-ed25519.pem: a private key', err)
        self.git('rm', '-q', '-f', 'keys/release-ed25519.pem')
        self.stage('notes/backup.txt', 'one\n' + pem)
        code, err = self.run_check('staged')
        self.assertEqual(code, 1)
        self.assertIn('notes/backup.txt:2: a private key', err)
        self.assertNotIn('MC4CAQAw', err)
        self.git('rm', '-q', '-f', 'notes/backup.txt')
        self.stage('platform/update/release-key.txt', 'ab' * 32 + '\n')   # the public half is fine
        self.assertEqual(self.run_check('staged')[0], 0)
        base = self.commit('old.pem', pem)
        code, err = self.push(self.commit('new.txt', 'clean\n'), base)
        self.assertEqual(code, 1)
        self.assertIn('old.pem in the tree of refs/heads/main: a private key', err)

    def test_patch_from_line_mailto_and_escapes_are_refused(self):
        at = DUMMY.replace('@', '{}')
        for text in (f'From: Some Body <{DUMMY}>\n', f'[mail](mailto:{DUMMY})\n', f'"\\u003c{DUMMY}\\u003e"\n',
                     f'to=%3C{DUMMY}%3E\n', f'[form](https://example.org/f?email={at.format("%40")})\n',
                     at.format('&#64;'), at.format('&#X40;'), f'"{at.format(chr(92) + "u0040")}"',
                     b'\0\0' + DUMMY.encode() + b'\0'):
            with self.subTest(text=text):
                self.stage('wine/patches/0999-x.patch', text)
                self.assertEqual(self.run_check('staged')[0], 1)

    def test_upstream_addresses_and_removals_pass(self):
        self.commit('wine/patches/0001-x.patch', f'From: Alexandre Julliard <julliard@winehq.org>\nFrom: <{DUMMY}>\n')
        self.stage('wine/patches/0001-x.patch', 'From: Alexandre Julliard <julliard@winehq.org>\nSigned-off-by: '
                   'Some One <some.one@example.org>\n')
        self.assertEqual(self.run_check('staged'), (0, ''))

    def test_identity_and_file_name_are_refused(self):
        self.stage('a.txt', 'clean\n')
        code, err = self.run_check('staged', env=dict(self.env, GIT_AUTHOR_EMAIL=DUMMY))
        self.assertEqual(code, 1)
        self.assertIn('author identity', err)
        self.stage(f'people/{DUMMY}.txt', 'clean\n')
        code, err = self.run_check('staged')
        self.assertEqual(code, 1)
        self.assertIn('a file name', err)

    def test_pre_commit_hook_refuses_before_the_gate(self):
        self.git('config', 'core.hooksPath', str(HOOKS))
        self.stage('a.txt', 'clean\n')
        code, err = self.run_check(command=['git', 'commit', '-q', '-m', 'x', f'--author=Some Body <{DUMMY}>'])
        self.assertEqual(code, 1)
        self.assertIn('author identity', err)
        self.assertNotIn('gate', err)
        self.stage('a.txt', f'{DUMMY}\n')
        self.assertEqual(self.run_check(command=['git', 'commit', '-q', '-m', 'x'])[0], 1)
        self.assertEqual(subprocess.run(['git', 'rev-parse', '-q', '--verify', 'HEAD'], cwd=self.repo, env=self.env,
                                        capture_output=True).returncode, 1, 'nothing was committed')

    def test_push_refuses_new_commits_that_carry_it(self):
        base = self.commit('a.txt', 'clean\n')
        self.assertEqual(self.push(self.commit('b.txt', f'{DUMMY}\n'), base)[0], 1)
        self.git('reset', '-q', '--hard', base)
        code, err = self.push(self.commit('c.txt', 'clean\n', env=dict(self.env, GIT_AUTHOR_EMAIL=DUMMY)), base)
        self.assertEqual(code, 1)
        self.assertIn('author or committer identity in commit', err)
        self.git('reset', '-q', '--hard', base)
        self.git('commit', '-q', '--allow-empty', '-m', f'Reported-by: {DUMMY}')
        self.assertEqual(self.push(self.git('rev-parse', 'HEAD'), base)[0], 1)

    def test_push_refuses_a_tip_whose_tree_carries_it(self):
        old = self.commit('old.txt', f'{DUMMY}\n')
        code, err = self.push(self.commit('new.txt', 'clean\n'), old)
        self.assertEqual(code, 1)
        self.assertIn('old.txt in the tree of refs/heads/main', err)

    def test_every_way_of_making_a_commit_is_checked(self):
        base = self.commit('a.txt', f'{DUMMY}\n')
        self.git('checkout', '-q', '-b', 'side')
        self.commit('b.txt', f'{DUMMY}\n')
        self.git('checkout', '-q', '-b', 'patch', base)
        self.commit('c.txt', 'clean\n', env=dict(self.env, GIT_AUTHOR_EMAIL=DUMMY))
        patch = self.repo / 'x.patch'
        patch.write_text(self.git('format-patch', '-1', '--stdout'))
        self.git('checkout', '-q', 'main')
        self.commit('d.txt', 'clean\n')
        (self.repo / 'tools').mkdir()
        (self.repo / 'tools' / 'gate.py').write_text('')  # the commit gate passes here
        (self.repo / '.git' / 'info' / 'exclude').write_text('tools/\nx.patch\n')
        self.git('config', 'core.hooksPath', str(HOOKS))
        head = self.git('rev-parse', 'HEAD')
        editor = dict(self.env, GIT_EDITOR=f"echo 'Reported-by: {DUMMY}' >")
        for command, undo, env in ((['cherry-pick', 'side'], 'cherry-pick', None),
                                   (['merge', '--no-verify', '--no-ff', '-m', 'm', 'side'], 'merge', None),
                                   (['am', str(patch)], 'am', None),
                                   (['commit', '--no-verify', '--allow-empty', '-m', f'By: {DUMMY}'], None, None),
                                   (['commit', '--allow-empty'], None, editor)):
            with self.subTest(command=command[0], editor=bool(env)):
                self.assertNotEqual(self.run_check(command=['git', *command], env=env)[0], 0)
                self.assertEqual(self.git('rev-parse', 'HEAD'), head, 'nothing was committed')
                if undo:
                    self.git(undo, '--abort')
        self.stage('a.txt', 'clean\n')
        self.assertEqual(self.run_check(command=['git', 'commit', '-q', '-v', '-e', '-m', 'Remove it'],
                                        env=dict(self.env, GIT_EDITOR='true'))[0], 0, 'removing it is allowed')

    def test_push_trusts_only_what_the_remote_reports(self):
        self.commit('a.txt', f'{DUMMY}\n')
        self.git('update-ref', 'refs/remotes/origin/old', 'HEAD')  # stale: the remote no longer holds it
        tip = self.commit('a.txt', 'clean\n')
        code, err = self.push(tip)
        self.assertEqual(code, 1)
        self.assertIn('a.txt:1 in commit', err)
        code, err = self.push(tip, tip, ref=f'refs/heads/from-{DUMMY}')
        self.assertEqual(code, 1)
        self.assertIn('a pushed ref name', err)
        self.git('tag', '-a', '-m', f'Signed: {DUMMY}', 'v1')
        code, err = self.run_check('push', 'origin', stdin=f'refs/tags/v1 {self.git("rev-parse", "v1")} refs/tags/v1 '
                                   f'{tip}\n')
        self.assertEqual(code, 1)
        self.assertIn('the tag refs/tags/v1', err)

    def test_push_refuses_a_merge_resolution_that_carried_it(self):
        base = self.commit('a.txt', 'clean\n')
        self.git('checkout', '-q', '-b', 'side')
        self.commit('b.txt', 'clean\n')
        self.git('checkout', '-q', 'main')
        self.commit('c.txt', 'clean\n')
        self.git('merge', '-q', '--no-ff', '--no-commit', 'side')
        self.stage('b.txt', f'clean\n{DUMMY}\n')
        self.git('commit', '-q', '--no-verify', '-m', 'merge')
        code, err = self.push(self.commit('b.txt', 'clean\n'), base)
        self.assertEqual(code, 1)
        self.assertIn('b.txt:2 in commit', err)

    def test_push_passes_clean_history_removals_and_deletes(self):
        self.commit('a.txt', f'{DUMMY}\n')
        self.git('update-ref', 'refs/remotes/origin/main', 'HEAD')
        self.assertEqual(self.push(self.commit('a.txt', 'clean\n'), self.git('rev-parse', 'HEAD~1')), (0, ''))
        self.assertEqual(self.run_check('push', 'origin', stdin=f'(delete) {ZERO} refs/heads/old {ZERO}\n'), (0, ''))
        self.git('update-ref', '-d', 'refs/remotes/origin/main')
        hook = ['sh', str(HOOKS / 'pre-push'), 'origin', 'url']
        stdin = f'refs/heads/main {self.git("rev-parse", "HEAD")} refs/heads/main {ZERO}\n'
        self.assertEqual(self.run_check(command=hook, stdin=stdin)[0], 1, 'the root commit added it')

    def test_scan_audits_history(self):
        self.commit('a.txt', f'{DUMMY}\n')
        self.commit('a.txt', 'clean\n')
        code, err = self.run_check('scan', '--all')
        self.assertEqual(code, 1)
        self.assertIn('a.txt:1 in commit', err)
        self.assertEqual(self.run_check('scan', 'HEAD~1..HEAD'), (0, ''))

    def test_scan_all_checks_refs_that_name_a_tree(self):
        self.commit('a.txt', 'clean\n')
        self.stage('wine/patches/0945-x.patch', f'From: Some Body <{DUMMY}>\n')
        self.git('update-ref', 'refs/codex/checkpoint', self.git('write-tree'))
        self.git('reset', '-q', '--hard')
        code, err = self.run_check('scan', '--all')
        self.assertEqual(code, 1)
        self.assertIn('wine/patches/0945-x.patch in the tree of refs/codex/checkpoint', err)


if __name__ == '__main__':
    unittest.main()
