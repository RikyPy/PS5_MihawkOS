#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Read the deployed title back from the console by hash.

  tools/verify-title.py [dist/PPSA99170] [--no-record]

Every file of the dist folder is read from /data/homebrew/<TITLE_ID>/<same path> with the FTP server's SELF mode off
(with it on, a signed file reads back decrypted) and compared by sha256. Exit 0 when all match: unless --no-record,
the guard then records the title as verified (tools/console_guard.py), which every launch after a deploy or a restart
needs. A read right after an upload may come from the console's cache; only one after a restart shows what is on the
disk, which is why new-boot asks for it again (libc.prx, read back before, did not load after the dmem-01 power cut).
Exit 1 lists the paths that differ or cannot be read (our own build's files, no game data). It only reads, so it is
allowed under every guard refusal and is the integrity read-back after a hang.

tools/gate.py also uses runtime_problems(): the console's runtime deploy manifest against .deps/staged/manifest.json.
That compares what the last tools/deploy-wine.py wrote (it read back each file it uploaded), not the runtime files'
present bytes.
"""
import argparse
import ftplib
import hashlib
import io
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402
sys.path.insert(0, str(ROOT / 'tools'))
import console_guard  # noqa: E402
from console_log import redact  # noqa: E402

RUNTIME_MANIFEST = '/data/ps5proton/runtime/ge11-7/deploy-manifest.json'


def connect(settings):
    ftp = ftplib.FTP()
    ftp.connect(settings['host'], settings['ftp_port'], timeout=30)
    ftp.login(settings['ftp_user'], settings['ftp_password'] or 'codex')
    return ftp


def self_off(ftp):
    """The SELF command toggles the server's mode for this connection, as tools/deploy-wine.py uses it."""
    for _ in range(2):
        if 'disabled' in ftp.sendcmd('SELF').lower():
            return
    raise SystemExit("the FTP server's SELF mode did not turn off; a signed file would read back decrypted")


def title_mismatches(ftp, folder):
    """(the relative paths whose console copy differs or cannot be read, a digest of the whole folder)."""
    base = f'/data/homebrew/{folder.name}'
    bad, digest = [], hashlib.sha256()
    for path in sorted(p for p in folder.rglob('*') if p.is_file()):
        relative = path.relative_to(folder).as_posix()
        local = hashlib.sha256(path.read_bytes()).hexdigest()
        remote = hashlib.sha256()
        try:
            ftp.retrbinary(f'RETR {base}/{relative}', remote.update, blocksize=262144)
        except ftplib.error_perm:
            remote = None
        if remote is None or remote.hexdigest() != local:
            bad.append(relative)
        digest.update(f'{relative}\0{local}\n'.encode())
    return bad, digest.hexdigest()


def runtime_problems(ftp, staged):
    """Where the console's runtime deploy manifest disagrees with this build's staged manifest."""
    manifest = json.loads(Path(staged).read_text())
    raw = io.BytesIO()
    try:
        ftp.retrbinary(f'RETR {RUNTIME_MANIFEST}', raw.write)
        console = json.loads(raw.getvalue())
    except ftplib.error_perm:
        return ['runtime: no deploy manifest on the console']
    except ValueError:
        return ['runtime: the deploy manifest on the console is not JSON']
    stale = sorted(key for key, value in console.items() if manifest.get(key) != value)
    missing = sorted(key for key in manifest if (key.startswith('runtime/') or key == 'run/proton-payload-child.elf')
                     and key not in console)
    return [f'runtime {key} differs' for key in stale] + [f'runtime {key} missing' for key in missing]


def check(settings, folder, staged=None):
    """(problems, the title folder's digest): problems is empty when the console holds this title, and with staged
    this runtime deploy."""
    with connect(settings) as ftp:
        self_off(ftp)
        bad, digest = title_mismatches(ftp, folder)
        problems = [f'title {relative} differs or is unreadable' for relative in bad]
        if staged:
            problems += runtime_problems(ftp, staged)
    return problems, digest


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('folder', nargs='?', default=str(ROOT / 'dist/PPSA99170'))
    parser.add_argument('--no-record', action='store_true', help='do not record the result in the console guard')
    args = parser.parse_args()
    folder = Path(args.folder).resolve()
    if not (folder / 'eboot.bin').is_file():
        raise SystemExit(f'{folder} is not a built title folder')
    settings = ps5_console.load_settings()
    try:
        problems, digest = check(settings, folder)
    except (OSError, ftplib.Error) as error:
        raise SystemExit('verify-title: ' + redact(str(error).replace(settings['host'], '<console>'))) from None
    files = sum(1 for p in folder.rglob('*') if p.is_file())
    if problems:
        print(f'verify-title: {len(problems)} of {files} files do not match the console:\n  ' + '\n  '.join(problems))
        return 1
    print(f'verify-title: all {files} files of {folder.name} match the console by sha256 (SELF off)')
    if not args.no_record:
        console_guard.record_verified('title', digest, 'every file read back by sha256 with SELF off')
    return 0


if __name__ == '__main__':
    sys.exit(main())
