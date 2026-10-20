#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The privacy guard: a blocked personal address never enters a commit and never leaves the machine.

On 2026-10-07 the owner's work address entered through `git format-patch` From: lines (patches 0945-0954, exported
from a scratch Wine tree whose author identity was that address); it reached GitHub on mihawk-os and was removed on
2026-10-16 by a history rewrite. The owner's identity everywhere is his GitHub no-reply address.

  privacy_check.py staged [MSG]  the commit hooks: the author and committer identity git will record (for git am, the
                                 patch's author), every line the staged diff adds (binary files as text), every staged
                                 path, and the message file MSG. pre-commit runs it before the commit gate;
                                 prepare-commit-msg on every commit git makes, merges, cherry-picks, reverts and
                                 rebases included, --no-verify or not; commit-msg on the message as edited;
                                 pre-applypatch on git am. Not seen before the push: the original author a cherry-pick
                                 or rebase keeps.
  privacy_check.py push R [URL]  the pre-push hook (tools/git-hooks/pre-push), refs on stdin as git gives them: each
                                 pushed ref name; every commit the remote does not hold (git reads what it holds during
                                 the push, so a new branch checks its whole history), its author, committer, message and
                                 the lines and paths it adds against its first parent (a root commit: its whole tree);
                                 a tag's tagger and message; then the whole tree of each pushed tip, so content
                                 committed before this guard existed cannot ride along.
  privacy_check.py scan REV...   the commit checks for an audit: a range, a ref, or --all, which also checks every
                                 ref name and every ref that names a tag, tree or blob (git log walks commits only).

The owner's release signing key (tools/sign-release.py, docs/SELF_INSTALL.md phase 8) is refused the same ways: a path
in the ignored keys/ folder, and a line that opens a PEM private key, under any name.

Exit 1 on a hit, naming the identity, the commit, or the file and line, never the address. A token that looks like an
address is lower-cased and compared by sha256, and so is every suffix of its local part with every dot-prefix of its
domain: escapes (\\u003c, %3C, =3C) glue letters onto the front, a file extension or a sentence onto the end. An @
written %40, &#64; or \\u0040 counts as one.
Removed lines are not checked, so a commit that deletes an address passes.
PW_PRIVACY_EXTRA_SHA256 (comma-separated hashes) blocks more addresses; the tests block a dummy one with it.

Measured 2026-10-16 on this repository: `staged` 52 ms with a three-file change staged; `push` of one commit
(a tree grep included) 130 ms; `scan --all` (426 commits) 1.8 s. A push that creates a branch checks its whole history
as `scan --all` does: 4.1 s against its 5.2 s, both with the host at load 34 on 12 cores. `git commit --author=` is
seen: git exports it before the hooks.
"""
import functools
import hashlib
import os
import re
import subprocess
import sys

# sha256 of each lower-cased address. Only hashes: this file is public, the addresses it guards are not.
BLOCKED = {
    '02decb54b6534bb691b6601ee37e8483216d870c5f57a6fcf5b6659649ad9fbe',  # the owner's work address
    '79fdc04453908d30f698edef11da1ff71991ef8b925d71d830ca4df124dbf417',  # the owner's old personal address
} | {h.strip().lower() for h in os.environ.get('PW_PRIVACY_EXTRA_SHA256', '').split(',') if h.strip()}

AT = r'(?i:@|%40|&#0*64;|&#x0*40;|\\u0040)'  # an @ as written, in a URL, in HTML, in JSON
EMAIL_RE = r'[A-Za-z0-9._%+-]+' + AT + r'[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+'  # also git grep's, below
EMAIL, AT_SIGN = re.compile(EMAIL_RE.encode()), re.compile(AT.encode())
# The release key's folder, and a PEM private key's first line (any kind: PRIVATE, RSA PRIVATE, ENCRYPTED PRIVATE).
KEY_FOLDER = b'keys/'
PRIVATE_KEY = re.compile(rb'-----BEGIN (?:[A-Z]+ )*PRIVATE KEY-----')
PRIVATE_KEY_ERE = '-----BEGIN ([A-Z]+ )*PRIVATE KEY-----'  # the same, for git grep -E
KEY = 'a private key'
MARK = b'\x01pw-commit '
LOG = ['log', '--format=%x01pw-commit %H %ae %ce%n%w(0,4,4)%B', '--root', '--diff-merges=first-parent']
DIFF = ['-p', '--raw', '--no-renames', '-U0', '--text', '--no-color', '--no-ext-diff', '--no-textconv',
        '--src-prefix=a/', '--dst-prefix=b/']


@functools.lru_cache(maxsize=None)
def blocked_token(token):
    local, _, domain = AT_SIGN.sub(b'@', token).lower().rpartition(b'@')
    labels = domain.split(b'.')
    return any(hashlib.sha256(local[i:] + b'@' + b'.'.join(labels[:j])).hexdigest() in BLOCKED
               for i in range(len(local)) for j in range(2, len(labels) + 1))


def blocked(text):
    return AT_SIGN.search(text) is not None and any(blocked_token(m.group()) for m in EMAIL.finditer(text))


def git_lines(*args, ok=(0,)):
    with subprocess.Popen(['git', *args], stdout=subprocess.PIPE) as proc:
        yield from proc.stdout
    if proc.returncode not in ok:
        sys.exit(f'privacy check: git {args[0]} failed ({proc.returncode}); nothing was checked')


def check_diff(lines, hits):
    """What a stream in LOG/DIFF's shape adds: identities and messages (log), new paths (raw), added lines (patch)."""
    commit, where, number, hunk = '', '', 0, False
    for line in lines:
        line = line.rstrip(b'\n')
        if line.startswith(MARK):
            commit, hunk = ' in commit ' + line.split()[1].decode()[:12], False
            if blocked(line):
                hits.append('author or committer identity' + commit)
        elif hunk and line.startswith(b'+'):
            if blocked(line):
                hits.append(f'{where}:{number}{commit}')
            if PRIVATE_KEY.search(line):
                hits.append((f'{where}:{number}{commit}', KEY))
            number += 1
        elif line.startswith(b'@@'):
            hunk, number = True, int(re.search(rb'\+(\d+)', line).group(1))
        elif line.startswith(b'diff --git '):
            hunk = False
        elif hunk:
            continue
        elif line.startswith(b'+++ '):
            where = line[4:].decode('utf-8', 'replace').strip('"').removeprefix('b/')
        elif line.startswith(b'    ') and blocked(line):
            hits.append('message' + commit)
        elif line.startswith(b':'):
            meta, _, path = line.partition(b'\t')
            if not meta.endswith(b'D') and blocked(path):
                hits.append('a file name' + commit)
            if not meta.endswith(b'D') and path.startswith(KEY_FOLDER):
                hits.append((path.decode('utf-8', 'replace') + commit, KEY))


def check_tree(tip, name, hits):
    # -P: 75 ms on this tree, -E: 2.2 s. shortcut: needs a git built with PCRE (else exit 128 refuses the push);
    # use -E on one that is not.
    for line in git_lines('grep', '-I', '-o', '-z', '-P', '-e', EMAIL_RE, tip, '--', ok=(0, 1)):
        path, _, token = line.rstrip(b'\n').partition(b'\0')
        if blocked(token):
            hits.append(f'{path[len(tip) + 1:].decode("utf-8", "replace")} in the tree of {name}')
    paths = b''.join(git_lines('ls-tree', '-r', '-z', '--name-only', tip)).split(b'\0')
    if any(blocked(path) for path in paths):
        hits.append(f'a file name in the tree of {name}')
    if any(path.startswith(KEY_FOLDER) for path in paths):
        hits.append((f'the keys/ folder in the tree of {name}', KEY))
    for line in git_lines('grep', '-I', '-l', '-E', '-e', PRIVATE_KEY_ERE, tip, '--', ok=(0, 1)):
        path = line.rstrip(b'\n').decode('utf-8', 'replace')[len(tip) + 1:]
        hits.append((f'{path} in the tree of {name}', KEY))


def check_object(sha, name, hits):
    """What git log does not show behind a ref: a tag's tagger and message, a tree or blob, tagged or not. Returns the
    commit it names, or None."""
    while (kind := b''.join(git_lines('cat-file', '-t', sha)).strip()) == b'tag':
        tag = b''.join(git_lines('cat-file', 'tag', sha))
        if blocked(tag):
            hits.append(f'the tag {name}')
        sha = tag.split()[1].decode()  # "object <sha>"
    if kind == b'tree':
        check_tree(sha, name, hits)
    elif kind == b'blob' and blocked(b''.join(git_lines('cat-file', 'blob', sha))):
        hits.append(f'the blob {name}')
    return sha if kind == b'commit' else None


def check_push(hits):
    rows = [line.split() for line in sys.stdin if line.strip()]
    # What the remote holds, as git read it from the remote for this push; remote-tracking refs may be stale.
    held = [old for *_, old in rows if old.strip('0') and not subprocess.run(
        ['git', 'cat-file', '-e', old + '^{commit}'], capture_output=True).returncode]
    tips = []
    for _, sha, ref, _ in (row for row in rows if row[1].strip('0')):  # a deletion publishes nothing
        if blocked(ref.encode()):
            hits.append('a pushed ref name')
        if commit := check_object(sha, ref, hits):
            tips.append((ref, commit))
    if tips:
        check_diff(git_lines(*LOG, *DIFF, *(sha for _, sha in tips), '--not', *held), hits)
    for ref, sha in tips:
        check_tree(sha, ref, hits)


def main(argv):
    hits = []
    mode = argv[1] if len(argv) > 1 else ''
    if mode == 'staged':
        for var in ('GIT_AUTHOR_IDENT', 'GIT_COMMITTER_IDENT'):
            if blocked(subprocess.run(['git', 'var', var], capture_output=True).stdout):
                hits.append(var.split('_')[1].lower() + ' identity')
        # git am keeps the patch's author here, not in the environment.
        am = subprocess.run(['git', 'rev-parse', '--git-path', 'rebase-apply/author-script'],
                            capture_output=True).stdout.strip()
        if os.path.isfile(am) and blocked(open(am, 'rb').read()):
            hits.append('author identity of the patch')
        if len(argv) > 2 and argv[2]:
            # git commit -v lists the diff below the scissors line, removed lines too; git drops it after the hooks.
            with open(argv[2], 'rb') as msg:
                if blocked(re.split(rb'(?m)^\S+ -+ >8 -+$', msg.read())[0]):
                    hits.append('message')
        check_diff(git_lines('diff', '--cached', *DIFF), hits)
    elif mode == 'push':
        check_push(hits)
    elif mode == 'scan':
        check_diff(git_lines(*LOG, *DIFF, *argv[2:]), hits)
        if '--all' in argv:
            for line in git_lines('for-each-ref', '--format=%(objectname) %(objecttype) %(refname)'):
                sha, kind, ref = line.rstrip(b'\n').split(b' ', 2)
                if blocked(ref):
                    hits.append('a ref name')
                if kind != b'commit':
                    check_object(sha.decode(), ref.decode('utf-8', 'replace'), hits)
    else:
        print(__doc__, file=sys.stderr)
        return 2
    for label in dict.fromkeys(hits):
        label, what = label if isinstance(label, tuple) else (label, 'a blocked personal address')
        label = label if not blocked(label.encode()) else 'a name that itself carries it'
        print(f'privacy check: {label}: {what}', file=sys.stderr)
    if hits:
        print('privacy check: refused. Remove it (identities use the GitHub no-reply address; the release key stays in '
              'the ignored keys/ folder) and try again.', file=sys.stderr)
    return 1 if hits else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
