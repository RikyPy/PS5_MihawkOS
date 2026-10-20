#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the original Java probe using the already-installed Windows JDK."""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    work = ROOT / '.deps/java-process-validation'
    config = json.loads((work / 'input.json').read_text())
    runtime = config['java_windows_path'].replace('\\', '/').split('/java/')[1].split('/')[0]
    jdk = Path(os.environ.get('PROTON_JDK_ROOT', ROOT / '.deps/java')) / runtime   # a folder of Windows JDKs, by runtime name
    source = ROOT / 'tests/JavaProcessProbe.java'
    classes = work / 'classes'; classes.mkdir(exist_ok=True)
    host = ROOT / '.deps/wine-runtime'
    env = dict(os.environ, WINEPREFIX=str(ROOT / '.deps/process-host-prefix'), WINEDEBUG='-all', WINEDLLOVERRIDES='winemenubuilder.exe=d')
    with (work / 'compile.log').open('wb') as log:
        subprocess.run([str(host / 'build/loader/wine'), str(jdk / 'bin/javac.exe'), '--release', '17', '-d', 'Z:' + str(classes), 'Z:' + str(source)], env=env, stdout=log, stderr=log, check=True, timeout=120)
    entrypoint = config['java_windows_path'].replace('\\', '/').split('/')[-1]
    files = [source, jdk / 'bin/java.exe', jdk / 'bin'/entrypoint, jdk / 'bin/javac.exe', jdk / 'release', classes / 'JavaProcessProbe.class']
    manifest = {'runtime': runtime, 'entrypoint': entrypoint, 'host_wine_revision': (host / 'revision').read_text().strip(), 'sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
    (work / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))

if __name__ == '__main__': main()
