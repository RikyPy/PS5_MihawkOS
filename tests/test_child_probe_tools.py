# SPDX-License-Identifier: GPL-3.0-or-later
"""The readback verifier must catch changed code/imports despite unstable unmapped metadata."""
import importlib.util
from pathlib import Path
import struct
import sys
import json
import tempfile

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tools'))
spec = importlib.util.spec_from_file_location('child_probe', root / 'tools/run-child-probe.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)

image = bytearray(512)
image[:6] = b'\x7fELF\x02\x01'
struct.pack_into('<Q', image, 32, 64)
struct.pack_into('<HH', image, 54, 56, 3)
struct.pack_into('<IIQQQQQQ', image, 64, 1, 5, 256, 0, 0, 16, 16, 16384)
struct.pack_into('<IIQQQQQQ', image, 120, 2, 4, 272, 16384, 0, 16, 16, 8)
struct.pack_into('<IIQQQQQQ', image, 176, 0x6fffff01, 0, 288, 0, 0, 16, 16, 1)
original = probe.executable_sha(image)
for offset in (257, 273, 64):
    changed = bytearray(image)
    changed[offset] ^= 1
    assert probe.executable_sha(changed) != original
changed = bytearray(image)
changed[289] ^= 1
assert probe.executable_sha(changed) == original
struct.pack_into('<Q', changed, 64 + 32, 1024)
try:
    probe.executable_sha(changed)
except RuntimeError:
    pass
else:
    raise AssertionError('Out-of-bounds program segment accepted')
print('child probe readback verification: PASS')

# Both service modes must reject a direct-spawn build before any console access.
with tempfile.TemporaryDirectory() as directory:
    Path(directory, 'manifest.json').write_text(json.dumps({'kind': 'direct'}))
    prior_args = sys.argv
    try:
        for kind in ('service-preflight', 'service-native'):
            sys.argv = ['run-child-probe.py', '--kind', kind, '--build', directory,
                        '--name', 'verify-only']
            try:
                probe.main()
            except RuntimeError as error:
                assert 'does not match' in str(error)
            else:
                raise AssertionError('Wrong probe kind accepted')
    finally:
        sys.argv = prior_args
print('probe kind isolation: PASS')

# Validate actual original helper artifacts offline when their build is supplied.
# A pointer-free default helper must fail the selective-metadata precondition.
if len(sys.argv) == 3:
    selective = Path(sys.argv[1]).read_bytes()
    ordinary = Path(sys.argv[2]).read_bytes()
    assert probe.helper_preload(selective) == 0x8000000000000002
    try:
        probe.helper_preload(ordinary)
    except RuntimeError as error:
        assert 'preload pointer' in str(error)
    else:
        raise AssertionError('Helper without selective metadata accepted')
    print('helper preload relocation verification: PASS')
