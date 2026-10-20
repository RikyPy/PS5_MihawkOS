#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/check-payload-firmware.py: the firmware-gate parser and the equal-set-with-10.01-and-13.40 verdict.

The parser is checked against a crafted `__kernel_init` disassembly (the three shapes the gate uses: an exact `je`
match, an exact `jne`-to-fail match, and a `cmpl`/`jle` range bound that must not count). When the shipped payloads
have been built in this checkout (the production build always builds them), their real sets are cross-checked too, so
a future SDK re-pin that splits the daemon from the helper fails here as well as in the build."""
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('check_payload_firmware', ROOT / 'tools/check-payload-firmware.py')
cpf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cpf)

# llvm-objdump prints "<addr>:\t<mnemonic>\t<operands>"; the parser keeps lines with a tab and reads from the first.
DISASM = '\n'.join([
    '0000000000000000 <__kernel_init>:',
    '       d4:\tcmpl\t$0x103fffff, %r14d       # imm = 0x103FFFFF',   # range bound
    '       e4:\tjg\t0x19d <__kernel_init+0x19d>',                     # not je/jne -> ignored
    '       f6:\tcmpl\t$0x10010000, %r14d',                            # 10.01, exact
    '      100:\tje\t0x32c <__kernel_init+0x32c>',
    '     1c4:\tcmpl\t$0x13400000, %r14d',                             # 13.40, exact
    '     1cb:\tje\t0xb3d <__kernel_init+0xb3d>',
    '     1de:\tcmpl\t$0x13600000, %r14d',                             # 13.60, accepted via jne-to-fail
    '     1e5:\tjne\t0xd6b <__kernel_init+0xd6b>',
])

SHIPPED = {
    'daemon': ROOT / '.deps/lapy-validation/source/build/owned_root_daemon-service-verified-client/lapy-root-daemon.elf',
    'helper': ROOT / 'build/proton-payload-child.elf',
}


class ParserTest(unittest.TestCase):
    def test_exact_matches_counted_ranges_ignored(self):
        self.assertEqual(cpf.parse_firmware(DISASM), frozenset({'10.01', '13.40', '13.60'}))

    def test_decode_rejects_non_firmware_constants(self):
        self.assertIsNone(cpf._decode(0x103fffff))  # low 16 bits set
        self.assertEqual(cpf._decode(0x13400000), '13.40')


class VerdictTest(unittest.TestCase):
    full = frozenset({'10.01', '13.40', '13.42', '13.60'})

    def test_shared_set_with_required_passes(self):
        self.assertIsNone(cpf.verdict({'a': self.full, 'b': self.full}))

    def test_missing_required_fails(self):
        reason = cpf.verdict({'a': frozenset({'10.01', '13.20'})})
        self.assertIn('13.40', reason)

    def test_divergent_sets_fail(self):
        reason = cpf.verdict({'a': self.full, 'b': self.full | {'13.80'}})
        self.assertIn('differ', reason)


class ShippedPayloadsTest(unittest.TestCase):
    def test_built_payloads_share_a_set_with_required(self):
        present = {name: path for name, path in SHIPPED.items() if path.is_file()}
        if not present:
            self.skipTest('no payloads built in this checkout (the production build builds them)')
        sets = {name: cpf.firmware_set(path) for name, path in present.items()}
        self.assertIsNone(cpf.verdict(sets), sets)


if __name__ == '__main__':
    unittest.main()
