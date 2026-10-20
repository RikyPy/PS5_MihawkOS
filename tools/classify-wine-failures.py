#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build tests/wine-tests-expected-ps5.json from the rules below and the recorded runs.

A rule names a test file, a range of source lines of its test and a reason. Every console failure that
the baseline (the pinned Wine on Linux with fork() failing, as the console has no child processes) lacks
and that a rule covers is written to the expected list with that reason; the rest stays unexplained and
is printed. Reasons that start with "OPEN:" are defects of the port that are known and not yet fixed:
the comparison lists them apart, and they leave the list when their fix lands.

  tools/classify-wine-failures.py <dll> [--write]
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'evidence/wine-tests'
EXPECTED = ROOT / 'tests/wine-tests-expected-ps5.json'

PAGES = "the console's pages are 16 KiB and Windows' are 4 KiB (guard regions, partial frees and write watches work on host pages)"
NOTIFY = "directory change notifications are polled (25 ms): a wait with a zero timeout right after the change is too early"
ATTRS = "the console's file system has no hidden or system flags, and does not keep an access time that is set"
LINKS = "the console's file system has no hard links, symbolic links or reparse points"
CPU = "CPU state the console does not let a program have: hardware debug registers, the segment selectors Windows reports, alignment checks"
CHILD = "no child processes (patch 0550); the test needs one"
DOSDEV = ("OPEN: DOS device links, volume names and the mount manager's device are missing (the mount manager is a driver of "
          "winedevice.exe, which the console cannot start)")

# (test file, first line, last line, reason)
RULES = {
    'ntdll': [
        ('change', 240, 285, NOTIFY),
        ('directory', 176, 176, ATTRS),
        ('directory', 1440, 1630, LINKS),
        ('exception', 3100, 3400, CPU),
        ('exception', 9200, 9400, CHILD),
        ('virtual', 400, 460, PAGES), ('virtual', 1060, 1250, PAGES), ('virtual', 2500, 2600, PAGES),
        ('file', 1500, 1660, ATTRS),
        ('file', 2400, 3300, LINKS),
        ('file', 4690, 4730, ATTRS),
        ('file', 6150, 7300, LINKS),
        ('thread', 140, 160, CHILD),
        ('info', 2880, 2910, 'OPEN: ReadProcessMemory on a handle to the program\'s own process (patch 0903)'),
        ('info', 4700, 4860, 'OPEN: ProcessTlsInformation (reads other threads\' TEBs through process memory; patch 0903)'),
        ('info', 1950, 1990, 'OPEN: WorkingSetSize and PagefileUsage of the process are not reported (patch 0905)'),
        ('info', 770, 800, 'OPEN: SystemProcessorPerformanceInformation reports kernel time below idle time (patch 0906)'),
        ('path', 730, 740, 'OPEN: a file name with a trailing backslash opens; Windows refuses it (pw_wine_cwd_libc.c)'),
        ('file', 5000, 5020, 'OPEN: a file name with a trailing backslash opens; Windows refuses it (pw_wine_cwd_libc.c)'),
        ('file', 340, 370, DOSDEV),
        ('file', 4860, 4880, 'OPEN: FileFsVolumeInformation has no volume serial number (patch 0904)'),
        ('file', 4815, 4830, DOSDEV),
        ('file', 190, 200, 'OPEN: NtReadFile on a directory handle returns a different status'),
        ('file', 4525, 4535, "OPEN: the file id's volume serial number is 0, not the volume information's (patch 0904)"),
        ('om', 2750, 2860, 'OPEN: the object directory still holds an object of the test after it (or a batch effect: to rerun alone)'),
    ],
    # the same reasons for kernel32 and kernelbase, keyed by test file and the lines of its source
    'kernel32': [
        ('change', 1, 2000, NOTIFY),
        ('debugger', 1, 2000, CHILD),
        ('drive', 30, 80, DOSDEV),
        ('drive', 150, 270, DOSDEV),   # volume names and free space by volume name
        ('environ', 70, 80, 'OPEN: no user profile directory in the registry (wineboot, which creates it, cannot run)'),
        ('environ', 670, 880, 'OPEN: no computer name in the registry (wineboot, which creates it, cannot run)'),
        ('file', 430, 440, 'OPEN: a file name with a trailing slash opens and creates; Windows refuses it (pw_wine_cwd_libc.c)'),
        ('file', 1400, 1630, DOSDEV),
        ('file', 3520, 3600, PAGES),
        ('file', 5690, 5730, DOSDEV),
        ('file', 5870, 5890, ATTRS),
        ('file', 6350, 6870, LINKS),
        ('heap', 220, 226, 'OPEN: GetProcessHeaps lists the heaps in another order'),
        ('heap', 3645, 3655, 'OPEN: SystemPerformanceInformation reports no available pages (patch 0912)'),
        ('loader', 1530, 1540, LINKS),
        ('loader', 4085, 4205, CHILD),
        ('loader', 4720, 4740, 'OPEN: the headers of a loaded builtin DLL differ from its file in the prefix (the deploy kept the prefix\'s copies as they were at its creation; it now refreshes the probe prefix\'s)'),
        ('mailslot', 55, 65, 'OPEN: a mailslot name outside \\\\.\\mailslot is accepted'),
        ('module', 2045, 2085, 'OPEN: the file name of a loaded DLL is not the one the test expects'),
        ('pipe', 190, 196, 'OPEN: a named pipe name outside \\\\.\\pipe is accepted'),
        ('power', 60, 75, 'OPEN: the power status does not report AC power (patch 0913)'),
        ('process', 1470, 1530, CHILD),
        ('virtual', 150, 180, CHILD),
        ('virtual', 1790, 2400, PAGES),
        ('virtual', 4530, 4540, PAGES),
        ('virtual', 4725, 4735, 'OPEN: NtFlushProcessWriteBuffers does not order the CPUs\' stores (patch 0911 fixes it)'),
        ('file', 4770, 4780, ATTRS),
        ('path', 2680, 2690, 'the test program is staged at the drive root, where the DLL search path then names C:\\ twice'),
        ('volume', 1, 3000, DOSDEV),
        ('sync', 2800, 2900, CHILD),
        ('toolhelp', 480, 500, CHILD),
    ],
    'kernelbase': [
        ('process', 150, 190, PAGES),
    ],
    'd3d10core': [
        ('d3d10core', 31000, 41000, 'OPEN: two guarded test functions (offsets 0x7be0 and 0x9d90 of the test program) crash with an '
                                'access violation on the console, as on 2026-10-03; not yet identified'),
    ],
}


# test files whose end differs from the baseline's for a known reason
STATUS_RULES = {
    'ntdll': {'exception': CHILD},
    'd3d10core': {'d3d10core': 'OPEN: the last slice ends when a worker thread runs 35 KB past the bottom of its 1 MiB stack and '
                               'its exception cannot be dispatched ("Exception frame is not in stack limits"); Linux completes the file'},
    'kernel32': {'process': CHILD, 'sync': CHILD, 'toolhelp': CHILD, 'debugger': CHILD,
                 'virtual': "a stack made of guard pages is not possible on 16 KiB host pages: test_stack_commit ends the title with STATUS_STACK_OVERFLOW",
                 'volume': DOSDEV},
}


def load(path):
    return json.loads(path.read_text()) if path.is_file() else {}


def main():
    dll = sys.argv[1]
    # The same records the comparison reads (tools/run-wine-tests.py compare): the Direct3D files' DXVK baseline,
    # and their slices merged into one record per file.
    import importlib.util
    spec = importlib.util.spec_from_file_location('run_wine_tests', ROOT / 'tools/run-wine-tests.py')
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    console = load(OUT / f'{dll}-console.json')
    if dll in runner.D3D_DLLS:
        console = runner.merge_slices(console, dll)
        host = load(OUT / f'{dll}-host-dxvk.json')
    else:
        host = load(OUT / f'{dll}-host-noproc.json') or load(OUT / f'{dll}-host.json')
    expected = load(EXPECTED)
    mine = {}
    left = []
    for name, entry in sorted(console.items()):
        seen = {(f['file'], f['line']) for f in host.get(name, {}).get('failures', [])}
        done = set()
        for failure in entry.get('failures', []):
            key = (failure['file'], failure['line'])
            if key in seen or key in done:
                continue
            done.add(key)
            for test, first, last, reason in RULES.get(dll, []):
                if test == name and first <= failure['line'] <= last:
                    mine.setdefault(name, {})[f"{failure['file']}:{failure['line']}"] = reason
                    break
            else:
                left.append(f"{name}: {failure['file']}:{failure['line']}: {failure['text'][:110]}")
    for name, entry in sorted(console.items()):
        reason = STATUS_RULES.get(dll, {}).get(name)
        there, here = entry.get('status'), host.get(name, {}).get('status')
        if reason and here and there != here and there != 'skipped':
            mine.setdefault(name, {})['status'] = reason
    print(f'{sum(len(v) for v in mine.values())} lines explained, {len(left)} not')
    for line in left:
        print('  ' + line)
    if '--write' in sys.argv:
        expected[dll] = mine
        EXPECTED.write_text(json.dumps(expected, indent=1, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
