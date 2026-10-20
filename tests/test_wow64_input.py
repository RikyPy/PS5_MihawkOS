#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile the actual patched Wine INPUT conversion on the host; no Wine process or display needed.

Run after the production build, or pass a scratch patched user.c before building.
The console input32/input64 cases separately check the syscall wiring and received HID bytes.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / '.deps/wine-ps5/source/dlls/wow64win/user.c'
text = source.read_text()
start = text.index('typedef struct\n{\n    LONG    dx;')
stop = text.index('\ntypedef struct', text.index('static void input_32to64', start))
unit = '''#define __WINESRC__
#define _WIN64
#include <stddef.h>
#include <assert.h>
#include <windows.h>
''' + text[start:stop] + '''
int main(void) {
    INPUT32 narrow = {0}; INPUT wide = {0};
    assert(offsetof(INPUT32, hi) == 4 && offsetof(INPUT, hi) == 8);
    narrow.type = INPUT_HARDWARE; narrow.hi.uMsg = WM_INPUT;
    narrow.hi.wParamL = 0x1234; narrow.hi.wParamH = 0xabcd;
    input_32to64(&wide, &narrow);
    assert(wide.type == INPUT_HARDWARE && wide.hi.uMsg == WM_INPUT);
    assert(wide.hi.wParamL == 0x1234 && wide.hi.wParamH == 0xabcd);
    narrow.type = INPUT_MOUSE; narrow.mi.dx = -7; narrow.mi.dy = 9;
    narrow.mi.mouseData = 0x7654; narrow.mi.dwFlags = 0x8765;
    narrow.mi.time = 0x9876; narrow.mi.dwExtraInfo = 0xf1234567;
    input_32to64(&wide, &narrow);
    assert(wide.type == INPUT_MOUSE && wide.mi.dx == -7 && wide.mi.dy == 9);
    assert(wide.mi.mouseData == 0x7654 && wide.mi.dwFlags == 0x8765);
    assert(wide.mi.time == 0x9876 && wide.mi.dwExtraInfo == 0xf1234567);
    narrow.type = INPUT_KEYBOARD; narrow.ki.wVk = 0x42; narrow.ki.wScan = 0x1234;
    narrow.ki.dwFlags = 0x5678; narrow.ki.time = 0x9abc; narrow.ki.dwExtraInfo = 0xe1234567;
    input_32to64(&wide, &narrow);
    assert(wide.type == INPUT_KEYBOARD && wide.ki.wVk == 0x42 && wide.ki.wScan == 0x1234);
    assert(wide.ki.dwFlags == 0x5678 && wide.ki.time == 0x9abc && wide.ki.dwExtraInfo == 0xe1234567);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='proton-input-') as folder:
    folder = Path(folder)
    (folder / 'check.c').write_text(unit)
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / '.deps/wine-runtime/source/include'),
                    '-I' + str(ROOT / '.deps/wine-runtime/build/include'),
                    str(folder / 'check.c'), '-o', str(folder / 'check')], check=True)
    subprocess.run([str(folder / 'check')], check=True)
print('WoW64 INPUT conversion: PASS (hardware, signed mouse coordinates, keyboard, high-bit extra info)')
