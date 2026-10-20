#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the same appearance program under pinned Linux Wine, in an owned prefix."""
import json
import os
import re
import shutil
import subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
PREFIX=ROOT/'.deps/mihawk-appearance-oracle'
WINE=ROOT/'.deps/wine-runtime/build/wine'


def main():
    environment=dict(os.environ,WINEPREFIX=str(PREFIX),WINEDEBUG='+debugstr',WINEARCH='wow64')
    init=subprocess.run([str(WINE),'wineboot','--init'],env=environment,capture_output=True,timeout=120)
    (ROOT/'klog/mihawk-appearance-host-init.log').write_bytes(init.stdout+init.stderr)
    if init.returncode:raise RuntimeError('Owned host prefix initialization failed')
    windows=PREFIX/'drive_c/windows'
    for relative,source in [('resources/themes/light/light.msstyles',ROOT/'.deps/windows11-theme/windows11.msstyles'),
                            ('resources/mihawk/icons.dll',ROOT/'.deps/mihawk-icons/icons.dll')]:
        destination=windows/relative;destination.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,destination)
    for source in (ROOT/'.deps/windows11-theme/fonts').glob('*.ttf'):shutil.copy2(source,windows/'Fonts'/source.name)
    runs=[]
    for architecture in (64,32):
        executable=ROOT/'.deps/probes'/f'appearance{architecture}-probe.exe'
        run=subprocess.run([str(WINE),'explorer','/desktop=appearance-oracle,1920x1080',str(executable),'--probe'],
                           env=environment,capture_output=True,timeout=90)
        output=(run.stdout+run.stderr).decode(errors='replace')
        log=ROOT/'klog'/f'mihawk-appearance-host{architecture}.log';log.write_text(output)
        markers=re.findall(r'PS5_PROTON_APPEARANCE_PROBE (PASS|FAIL) checks=(\d+) failures=(\d+)',output)
        last=markers[-1] if markers else None
        runs.append({'architecture':architecture,'status':run.returncode,'marker':last,
                     'passed':run.returncode==0 and last is not None and last[0]=='PASS' and last[2]=='0'})
    record={'oracle':'pinned Wine on Linux','wine_revision':(ROOT/'.deps/wine-runtime/revision').read_text().strip(),
            'prefix':'owned diagnostic environment','runs':runs,'passed':all(x['passed'] for x in runs),
            'console_qualified':False,'windows_equivalence_verified':False}
    (ROOT/'evidence/mihawk-appearance-host-2026-10-09-01.json').write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps(record,indent=2))
    return 0 if record['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
