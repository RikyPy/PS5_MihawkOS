#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep the modification time of files a generator rewrote with the same content.

  tools/preserve-mtimes.py snapshot TREE STATE   before the generator runs
  tools/preserve-mtimes.py restore TREE STATE    after it

Wine's generators (make_requests, make_specfiles, make_vulkan, autoreconf)
rewrite their outputs every time. make only sees times, so an unchanged
server_protocol.h would rebuild most of ntdll and the server. A rewritten
file whose bytes equal what it held before the generator ran gets its old
time back, so what make rebuilds is what really changed.

"What it held before" is the blob id recorded when the generator last wrote
the file (kept in TREE/.git/pw-generated.json), or git's index entry for a
file the generator has not written yet.
"""
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path


def files(tree):
    for directory, subdirectories, names in os.walk(tree):
        if '.git' in subdirectories:
            subdirectories.remove('.git')
        for name in names:
            path = Path(directory) / name
            if not path.is_symlink():
                yield path


def blob_id(path):
    data = path.read_bytes()
    return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ('snapshot', 'restore'):
        raise SystemExit(__doc__)
    action, tree, state = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    if action == 'snapshot':
        times = {str(path.relative_to(tree)): path.lstat().st_mtime_ns for path in files(tree)}
        state.write_text(json.dumps(times))
        return 0
    times = json.loads(state.read_text())
    cache_path = tree / '.git' / 'pw-generated.json'
    cache = json.loads(cache_path.read_text()) if cache_path.is_file() else {}
    index = {}
    listing = subprocess.run(['git', '-C', str(tree), 'ls-files', '-s'], capture_output=True, text=True,
                             check=True).stdout
    for line in listing.splitlines():
        meta, name = line.split('\t', 1)
        index[name] = meta.split()[1]
    kept = changed = created = 0
    for path in files(tree):
        relative = str(path.relative_to(tree))
        before = times.get(relative)
        stat = path.lstat()
        if before is not None and stat.st_mtime_ns == before:
            continue
        now = blob_id(path)
        if before is None:
            # New since the snapshot (configure after a clean tree): nothing to
            # keep, but its content is recorded for the next comparison.
            created += 1
        elif now == cache.get(relative, index.get(relative)):
            os.utime(path, ns=(stat.st_atime_ns, before))
            kept += 1
        else:
            changed += 1
        cache[relative] = now
    cache_path.write_text(json.dumps(cache, sort_keys=True))
    print(f'preserve-mtimes: {kept} rewritten unchanged, {changed} changed, {created} created')
    return 0


if __name__ == '__main__':
    sys.exit(main())
