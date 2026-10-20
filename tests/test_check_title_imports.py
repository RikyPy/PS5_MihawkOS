#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/check-title-imports.py: imports from a library a title loads pass; one bound to libkernel_sys, to
libScePosixForWebKit or to a VSH module, or a name exported but faulting (statvfs), fails and is named.

The fake SDK is laid out as the real one is: libSceLibcInternal and libc_stub_weak declare the same names (statvfs,
getenv), libScePosixForWebKit declares isatty and fnmatch, libkernel_sys alone declares statfs. The title is a small
converted ELF this test writes itself, in the layout ps5-native-tool gives (an import library table and NID#lib#module
symbols). With the pinned SDK and the native tool present, a real program is linked, converted and checked too."""
import base64
import hashlib
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'tools/check-title-imports.py'
spec = importlib.util.spec_from_file_location('check', TOOL)
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)
NM = shutil.which('llvm-nm') or shutil.which('nm')


def stub(path, names):
    source = path.with_suffix('.c')
    source.write_text(''.join(f'void {name}(void) {{}}\n' for name in names))
    subprocess.run([os.environ.get('CC', 'cc'), '-shared', '-fPIC', '-nostdlib', '-w', '-o', str(path), str(source)], check=True)


def converted(path, imports):
    """A converted title holding `imports` [(name, library)], as check.imports() reads one."""
    libraries = sorted({library for _, library in imports})
    strings = bytearray(b'\0')
    def string(text):
        offset = len(strings)
        strings.extend(text.encode() + b'\0')
        return offset
    library_offsets = [string(library) for library in libraries]
    symbols = bytearray()
    for name, library in imports:
        token = check.ALPHABET[libraries.index(library)]
        symbols += struct.pack('<IBBHQQ', string(f'{check.nid(name)}#{token}#A'), 0x12, 0, 0, 0, 0)
    base = 0x1000
    strtab, symtab = base, base + len(strings)
    dynamic = [(check.DT_STRTAB, strtab), (check.DT_STRSZ, len(strings)), (check.DT_SYMTAB, symtab),
               (check.SCE_SYMTAB_SIZE, len(symbols))] + [(check.SCE_IMPORT_LIB, i << 48 | offset)
                                                         for i, offset in enumerate(library_offsets)] + [(0, 0)]
    table = b''.join(struct.pack('<qQ', tag, value) for tag, value in dynamic)
    body = bytes(strings) + bytes(symbols)
    dynamic_offset = base + len(body)
    header = bytearray(base)
    header[:6] = b'\x7fELF\x02\x01'
    struct.pack_into('<Q', header, 32, 64)          # program headers right after the ELF header
    struct.pack_into('<HH', header, 54, 56, 2)
    struct.pack_into('<IIQQQQQQ', header, 64, 1, 4, base, base, base, len(body), len(body), 0x1000)
    struct.pack_into('<IIQQQQQQ', header, 120, 2, 4, dynamic_offset, dynamic_offset, dynamic_offset, len(table),
                     len(table), 8)
    path.write_bytes(bytes(header) + body + table)


class CheckTitleImports(unittest.TestCase):
    def run_check(self, elf, stubs, *more):
        return subprocess.run([sys.executable, str(TOOL), str(elf), '--stub-dir', str(stubs), '--nm', NM, *more],
                              capture_output=True, text=True)

    def test_the_rule(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            lib = work / 'lib'
            lib.mkdir()
            libc = ['getenv', 'statvfs', 'malloc']
            stub(lib / 'libSceLibcInternal.so', libc)
            stub(lib / 'libc_stub_weak.so', libc)                       # the same names, the same SONAME on the SDK
            stub(lib / 'libScePosixForWebKit.so', ['isatty', 'fnmatch'])
            stub(lib / 'libkernel_sys.so', ['statfs'])
            stub(lib / 'libkernel.so', ['open'])
            stub(lib / 'libSceVideoOut.so', ['sceVideoOutOpen'])
            stub(lib / 'libSceGLSlimVSH.so', ['glSlimOnly'])
            good = [('getenv', 'libSceLibcInternal'), ('malloc', 'libSceLibcInternal'), ('open', 'libkernel'),
                    ('sceVideoOutOpen', 'libSceVideoOut')]
            converted(work / 'good.elf', good)
            ran = self.run_check(work / 'good.elf', lib)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            self.assertIn('title imports: 4, from libSceLibcInternal, libSceVideoOut, libkernel', ran.stdout)
            converted(work / 'bad.elf', good + [('statfs', 'libkernel_sys'), ('isatty', 'libScePosixForWebKit'),
                                                ('fnmatch', 'libScePosixForWebKit'), ('statvfs', 'libSceLibcInternal'),
                                                ('glSlimOnly', 'libSceGLSlimVSH')])
            ran = self.run_check(work / 'bad.elf', lib)
            self.assertEqual(ran.returncode, 1)
            self.assertIn('5 of 9 imports resolve to nothing', ran.stderr)
            for refused in ('statfs (libkernel_sys)', 'isatty (libScePosixForWebKit)', 'fnmatch (libScePosixForWebKit)',
                            'statvfs (libSceLibcInternal)', 'glSlimOnly (libSceGLSlimVSH)'):
                self.assertIn(refused, ran.stderr)

    def test_the_real_toolchain(self):
        """The pinned SDK's linker and stubs, and ps5-native-tool, decide the libraries; the check reads them."""
        sdk = next(ROOT.glob('.deps/native/ps5-payload-sdk'), None)
        native = ROOT.parent / 'PS5_Vulkan/build/host/ps5-native-tool'
        if sdk is None or not native.is_file():
            self.skipTest('no pinned SDK in .deps or no PS5_Vulkan native tool')
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            (work / 't.c').write_text('#include <stdlib.h>\n#include <sys/mount.h>\nint isatty(int);\n'
                                      'int _start(void) { struct statfs f; return statfs("/", &f) + isatty(0) + '
                                      '(getenv("X") != 0); }\n')
            subprocess.run([str(sdk / 'bin/prospero-clang'), '-O1', '-c', str(work / 't.c'), '-o', str(work / 't.o')],
                           check=True, capture_output=True)
            subprocess.run([str(sdk / 'bin/prospero-lld'), '-pie', '-e', '_start', '-o', str(work / 't.elf'),
                            str(work / 't.o'), '--as-needed', *map(str, sorted((sdk / 'target/lib').glob('*.so')))],
                           check=True, capture_output=True)
            subprocess.run([str(native), 'link', '--in', str(work / 't.elf'), '--out', str(work / 't.conv'), '--stub-dir',
                            str(sdk / 'target/lib'), '--module-sdk', '0x02000009', '--companion-sdk', '0x08050001',
                            '--file-name', 'eboot.elf'], check=True, capture_output=True)
            ran = self.run_check(work / 't.conv', sdk / 'target/lib', '--nm', str(sdk / 'bin/llvm-nm'))
            self.assertEqual(ran.returncode, 1)
            self.assertIn('2 of 3 imports', ran.stderr)
            self.assertIn('statfs (libkernel_sys)', ran.stderr)
            self.assertIn('isatty (libScePosixForWebKit)', ran.stderr)


if __name__ == '__main__':
    unittest.main()
