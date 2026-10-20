#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Patch 1131: a helper never starts win32u's PS5 EGL/AGC stack, whose eglInitialize maps wired GPU memory at an address
the kernel picks, outside ntdll's memory backend. Both gates in the applied opengl.c are evaluated for each environment:
a helper (WINE_PS5_CHILD) of a game that sets WINE_PS5_OPENGL returns before the stack starts, the title keeps it.
Read from the tree the last tools/build-wine-ps5.sh applied; skipped without one."""
from pathlib import Path
import re
import unittest

OPENGL = Path(__file__).resolve().parents[1] / '.deps/wine-ps5/source/dlls/win32u/opengl.c'


def gate(source, signature, reaches):
    """The condition of the WINE_PS5_OPENGL gate in the function, which must come before `reaches`."""
    start = source.index(signature)
    text = source[start:source.index('\n}\n', start)]
    found = re.search(r'#ifdef WINE_PS5_OPENGL\n(?:\s*/\*.*?\*/\n)?\s*if \((.*)\) return \w+;\n#endif', text, re.S)
    assert found, f'no PS5 gate in {signature}'
    assert found.end() < text.index(reaches), f'{signature}: the gate comes after {reaches}'
    return found.group(1)


def returns_early(condition, env):
    python = re.sub(r'getenv\( "(\w+)" \)', lambda m: repr(m.group(1) in env), condition)
    python = python.replace('!', ' not ').replace('||', ' or ').replace('&&', ' and ')
    return eval(python)


@unittest.skipUnless(OPENGL.exists(), 'no applied Wine tree')
class HelperOpenGL(unittest.TestCase):
    def test_gates(self):
        source = OPENGL.read_text()
        for signature, reaches in (('const struct opengl_funcs *__wine_get_opengl_driver( UINT version )',
                                    'pthread_once( &init_once, display_funcs_init )'),
                                   ('BOOL get_opengl_gpus( struct list *gpus )',
                                    '__wine_get_opengl_driver( WINE_OPENGL_DRIVER_VERSION )')):
            condition = gate(source, signature, reaches)
            with self.subTest(function=signature):
                self.assertTrue(returns_early(condition, {'WINE_PS5_OPENGL', 'WINE_PS5_CHILD'}))
                self.assertTrue(returns_early(condition, {'WINE_PS5_CHILD'}))
                self.assertTrue(returns_early(condition, set()))
                self.assertFalse(returns_early(condition, {'WINE_PS5_OPENGL'}))


if __name__ == '__main__':
    unittest.main()
