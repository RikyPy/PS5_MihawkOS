#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Isolated host Wine with selected VM bookkeeping; Linux still has 4 KiB pages.

This tests Wine's larger-host-page policy, not PS5 kernel mapping behavior.
Reuse the pinned host objects without changing their source or build tree.
"""
import hashlib
import argparse
import json
import shlex
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--partial-decommit-fix', action='store_true')
    parser.add_argument('--host-page-size', type=int, choices=(4096, 16384), default=16384)
    args = parser.parse_args()
    runtime = ROOT / '.deps/wine-runtime'
    if (runtime / 'revision').read_text().strip() != 'a518d643873ff733bafb31595dfd01ab8bbdf790':
        raise RuntimeError('Host Wine revision differs from the pinned baseline')
    base = runtime / 'build'
    name = f'wine-host-page{args.host_page_size // 1024}k'
    output = ROOT / 'build' / (name + '-fixed' if args.partial_decommit_fix else name)
    source = runtime / 'source/dlls/ntdll/unix/virtual.c'
    text = source.read_text()
    changes = {
        '#if defined(__aarch64__) || defined(__PROSPERO__)': ('#if 1 /* diagnostic host-page policy */', 2),
        'host_page_size = sysconf( _SC_PAGESIZE );': (f'host_page_size = {args.host_page_size};', 1),
        'if (!size) size = sysconf( _SC_PAGESIZE );': (f'if (!size) size = {args.host_page_size};', 1),
    }
    for old, (new, count) in changes.items():
        if text.count(old) != count:
            raise RuntimeError('Pinned host page-policy source differs: ' + old)
        text = text.replace(old, new)
    output.mkdir(parents=True, exist_ok=True)
    diagnostic = output / 'virtual.c'
    diagnostic.write_text(text)
    patch_file = ROOT / 'wine/patches/0936-ntdll-zero-partial-host-pages-on-decommit.patch'
    if args.partial_decommit_fix:
        subprocess.run(['patch', '--batch', str(diagnostic), str(patch_file)], check=True)
    # The server's shared-data section and x86 dispatcher slot also use
    # sysconf. Model the same page size in every native Wine process.
    shim = output / 'pagesize.c'
    shim.write_text('''#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
static long (*original_sysconf)(int);
static pthread_once_t once=PTHREAD_ONCE_INIT;
static void resolve(void){original_sysconf=dlsym(RTLD_NEXT,"sysconf");}
long sysconf(int name){
    if(name==_SC_PAGESIZE)return 16384;
    pthread_once(&once,resolve);
    return original_sysconf(name);
}
'''.replace('return 16384;', f'return {args.host_page_size};'))
    subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror',
                    str(shim), '-ldl', '-lpthread', '-o', str(output / 'pagesize.so')], check=True)
    commands = subprocess.check_output(['make', '--no-print-directory', '--dry-run',
        '--what-if=../source/dlls/ntdll/unix/virtual.c', 'dlls/ntdll/ntdll.so'], cwd=base, text=True)
    commands = commands.replace(chr(92) + '\n', ' ')
    compile_line = next(line for line in commands.splitlines() if ' -c -o dlls/ntdll/unix/virtual.o ' in line)
    link_line = next(line for line in commands.splitlines() if ' -o dlls/ntdll/ntdll.so ' in line)
    obj = output / 'virtual.o'
    target = output / 'dlls/ntdll'
    target.mkdir(parents=True, exist_ok=True)
    compile_args = [str(obj) if value == 'dlls/ntdll/unix/virtual.o' else
                    str(diagnostic) if value == '../source/dlls/ntdll/unix/virtual.c' else value
                    for value in shlex.split(compile_line)]
    compile_args += ['-I' + str(source.parent)]
    subprocess.run(compile_args, cwd=base, check=True)
    link_args = [str(target / 'ntdll.so') if value == 'dlls/ntdll/ntdll.so' else
                 str(obj) if value == 'dlls/ntdll/unix/virtual.o' else value
                 for value in shlex.split(link_line)]
    subprocess.run(link_args, cwd=base, check=True)
    (output / 'loader').mkdir(exist_ok=True)
    shutil.copy2(base / 'loader/wine', output / 'loader/wine')
    shutil.copy2(base / 'loader/wine.inf', output / 'loader/wine.inf')
    for directory in ('server', 'programs', 'nls'):
        if not (output / directory).exists():
            (output / directory).symlink_to(base / directory, target_is_directory=True)
    for directory in (base / 'dlls').iterdir():
        destination = output / 'dlls' / directory.name
        if not destination.exists():
            destination.symlink_to(directory, target_is_directory=True)
    pe = target / 'x86_64-windows'
    if not pe.exists():
        pe.symlink_to(base / 'dlls/ntdll/x86_64-windows', target_is_directory=True)
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    manifest = {'wine_revision': (runtime / 'revision').read_text().strip(),
        'scope': 'host-only bookkeeping simulation; actual Linux pages unchanged',
        'host_page_policy_bytes': args.host_page_size,
        'original_source_sha256': digest(source), 'diagnostic_source_sha256': digest(diagnostic),
        'ntdll_sha256': digest(target / 'ntdll.so'), 'script_sha256': digest(Path(__file__)),
        'pagesize_shim_sha256': digest(output / 'pagesize.so'),
        'partial_decommit_fix': args.partial_decommit_fix,
        'patch_sha256': digest(patch_file) if args.partial_decommit_fix else None,
        'base_objects_sha256': {value: digest(base / value) for value in shlex.split(link_line)
                               if value.endswith('.o')}}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(output / 'loader/wine')


if __name__ == '__main__':
    main()
