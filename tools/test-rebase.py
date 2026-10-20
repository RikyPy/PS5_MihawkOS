#!/usr/bin/env python3
"""Regression checks for the local rebase helpers; no Wine checkout required."""
# Copyright (C) 2026 Mihawk
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

TOOLS = Path(__file__).resolve().parent


def run(*args, cwd, ok=True, **kwargs):
    result = subprocess.run(args, cwd=cwd, text=True, capture_output=True, **kwargs)
    assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
    return result.stdout.strip()


with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    tree = root / '.deps/wine-proton'
    patches = root / 'wine/patches'
    tree.mkdir(parents=True)
    patches.mkdir(parents=True)
    (root / 'docs').mkdir()
    (root / 'tools').mkdir()
    for name in ('rebase-next.sh', 'rebase-commit.sh', 'check-patches.sh'):
        shutil.copy2(TOOLS / name, root / 'tools' / name)
    run('git', 'init', '-q', '-b', 'ps5-rebase', cwd=tree)
    run('git', 'config', 'user.name', 'Rebase test', cwd=tree)
    run('git', 'config', 'user.email', 'test@invalid', cwd=tree)
    (tree / 'dlls').mkdir()
    source = tree / 'dlls/test.c'
    original = ''.join(f'line {n}\n' for n in range(30))
    source.write_text(original)
    run('git', 'add', '.', cwd=tree)
    run('git', 'commit', '-qm', 'base', cwd=tree)
    base = run('git', 'rev-parse', 'HEAD', cwd=tree)
    (root / '.deps/wine-proton.pin').write_text(base + '\n')
    source.write_text(original.replace('line 2\n', 'changed 2\n').replace('line 25\n', 'changed 25\n'))
    diff = run('git', 'diff', cwd=tree) + '\n'
    patch = patches / '0100-test.patch'
    patch.write_text('From: Original Author <author@invalid>\n'
                     'Date: Sat, 26 Sep 2026 12:19:12 +0200\n'
                     'Subject: [PATCH] Keep a wrapped\n subject intact\n\n'
                     'Keep the body.\n\n---\n' + diff)
    initial_patch = patch.read_bytes()

    # A rejected resolution must not advance HEAD or overwrite the source patch.
    reject = tree / 'dlls/test.c.rej'
    reject.write_text('unresolved\n')
    run(str(root / 'tools/rebase-next.sh'), '--commit', cwd=root, ok=False)
    assert run('git', 'rev-parse', 'HEAD', cwd=tree) == base
    reject.unlink()
    source.write_text(original.replace('line 2\n', 'changed 2\n'))
    run(str(root / 'tools/rebase-next.sh'), '--commit', cwd=root, ok=False)
    assert run('git', 'rev-parse', 'HEAD', cwd=tree) == base
    assert patch.read_bytes() == initial_patch

    # Reapplying the next patch cannot erase tracked or untracked work.
    marker = tree / 'notes.txt'
    marker.write_text('keep me\n')
    before = source.read_bytes()
    run(str(root / 'tools/rebase-next.sh'), cwd=root, ok=False)
    assert marker.read_text() == 'keep me\n' and source.read_bytes() == before
    marker.unlink()
    source.write_text(original.replace('line 2\n', 'changed 2\n').replace('line 25\n', 'changed 25\n'))
    run(str(root / 'tools/rebase-next.sh'), '--commit', cwd=root)
    assert run('git', 'log', '-1', '--format=%s', cwd=tree) == 'Keep a wrapped subject intact'
    assert run('git', 'log', '-1', '--format=%an <%ae>', cwd=tree) == 'Original Author <author@invalid>'
    assert 'Keep the body.' in run('git', 'log', '-1', '--format=%b', cwd=tree)

    # Replay starts at the pin, sees dependent patches in order, and is read-only.
    (patches / '0200-dependent.patch').write_text(diff.replace('-line 2', '-changed 2').replace('+changed 2', '+again 2').replace('-line 25', '-changed 25').replace('+changed 25', '+again 25'))
    marker.write_text('keep me\n')
    before_status = run('git', 'status', '--porcelain', cwd=tree)
    result = run(str(root / 'tools/check-patches.sh'), cwd=root)
    assert '2 apply in order, 0 conflict, 0 untested' in result
    shutil.copy2(patches / '0200-dependent.patch', patches / '0300-conflict.patch')
    shutil.copy2(patch, patches / '0400-untested.patch')
    result = run(str(root / 'tools/check-patches.sh'), '--write', cwd=root, ok=False)
    assert '2 apply in order, 1 conflict, 1 untested' in result
    assert run('git', 'status', '--porcelain', cwd=tree) == before_status
    assert marker.read_text() == 'keep me\n'
    # An intentional hunk reduction requires an explicit reason in the commit.
    marker.unlink()
    source.write_text(source.read_text().replace('changed 2\n', 'reviewed 2\n'))
    run(str(root / 'tools/rebase-commit.sh'), str(patch), cwd=root,
        env={**os.environ, 'REBASE_HUNK_REDUCTION_REASON': 'The second change is already upstream.'})
    assert 'The second change is already upstream.' in run('git', 'log', '-1', '--format=%b', cwd=tree)
print('PASS: rejection guards, dirty-tree preservation, mail metadata, ordered read-only replay')
