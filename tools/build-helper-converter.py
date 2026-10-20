#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the pinned native converter with optional helper preload metadata."""
import hashlib
import io
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
REVISION = 'ab37acd506305ba3e575435e3ae9a351cfaf7cf4'


def build():
    patch = ROOT / 'tools/patches/native-helper-preload.patch'
    identity = REVISION[:12] + '-' + hashlib.sha256(patch.read_bytes()).hexdigest()[:12]
    work = ROOT / '.deps/native-helper-tool' / identity
    tool = work / 'ps5-native-tool'
    if not tool.exists():
        work.mkdir(parents=True, exist_ok=True)
        source = work / 'source'
        if source.exists():
            import shutil
            shutil.rmtree(source)
        source.mkdir()
        archive = subprocess.check_output(['git', '-C', str(ROOT.parent / 'PS5_Vulkan'),
                                           'archive', REVISION, 'tooling/native'])
        with tarfile.open(fileobj=io.BytesIO(archive)) as tree:
            tree.extractall(source, filter='data')
        subprocess.run(['git', 'apply', str(patch)], cwd=source, check=True)
        native = source / 'tooling/native'
        subprocess.run(['clang++', '-std=c++20', '-O2',
                        *[str(native / name) for name in ('native_app_builder.cpp',
                            'self_container.cpp', 'elf_object.cpp', 'sce_module_writer.cpp')],
                        '-lz', '-o', str(tool)], check=True)
    return tool


if __name__ == '__main__':
    print(build())
