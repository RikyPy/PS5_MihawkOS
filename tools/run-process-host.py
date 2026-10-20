#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run original process probes against the already-built, pinned host Wine."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name',required=True)
    parser.add_argument('--case',choices=('basic','suspended','terminate','repeat','parent-exit','descendants','descendants-repeat','descendants-parent-exit','java','graphics'),action='append')
    args=parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+',args.name):parser.error('use a plain name')
    evidence=ROOT/'evidence'/(args.name+'.json')
    if evidence.exists():raise RuntimeError('Use a new evidence name')
    runtime=ROOT/'.deps/wine-runtime';prefix=ROOT/'.deps/process-host-prefix'
    probe=ROOT/'.deps/probes/process-probe.exe'
    if not (prefix/'drive_c').is_dir():raise RuntimeError('Initialize the dedicated host prefix first')
    shutil.copy2(probe,prefix/'drive_c/process-probe.exe')
    env=dict(os.environ,WINEPREFIX=str(prefix),WINEDEBUG='warn+debugstr',WINEDLLOVERRIDES='winemenubuilder.exe=d')
    if args.case and 'java' in args.case:
        manifest=json.loads((ROOT/'.deps/java-process-validation/manifest.json').read_text())
        jdk=Path(os.environ.get('PROTON_JDK_ROOT',ROOT/'.deps/java'))/manifest['runtime']
        env.update(PS5_PROCESS_JAVA='Z:'+str(jdk/'bin'/manifest['entrypoint']),PS5_PROCESS_CLASSES='Z:'+str(ROOT/'.deps/java-process-validation/classes'))
    if args.case and 'graphics' in args.case:
        graphics=ROOT/'.deps/probes/graphics-render-probe.exe'
        shutil.copy2(graphics,prefix/'drive_c/graphics-render-probe.exe')
        env.update(PS5_PROTON_PROBE_CAPTURE='Z:'+str(ROOT/'klog'/(args.name+'-readback.ppm')),
                   WINEDLLOVERRIDES='winemenubuilder.exe=d;d3d11,dxgi=b')
    rows=[]
    for case in args.case or ['basic','suspended','terminate','repeat','parent-exit','descendants','descendants-repeat','descendants-parent-exit']:
        path=ROOT/'klog'/(args.name+'-'+case+'-host.log');started=time.monotonic()
        with path.open('wb') as log:
            result=subprocess.run([str(runtime/'build/loader/wine'),'C:\\process-probe.exe']+(['--'+case] if case!='basic' else []),env=env,stdout=log,stderr=log,timeout=60)
        text=path.read_text(errors='replace')
        row={'case':case,'exit_code':result.returncode,'seconds':round(time.monotonic()-started,2),'verdict':'PASS' if 'PS5_PROTON_PROCESS_PROBE PASS' in text else 'FAIL',
             'handles':[{'before':int(a),'after':int(b)} for a,b in re.findall(r'parent handles before (\d+) after (\d+)',text)]}
        if case.startswith('descendants'):
            row['descendant_nodes']=len(re.findall(r'OutputDebugStringA "PS5_PROTON_DESCENDANT_NODE PASS',text))
            row['expected_nodes']=12 if case=='descendants-repeat' else 3
        if case.endswith('parent-exit'):
            marker='PS5_PROTON_DESCENDANT_SURVIVED_PARENT' if case.startswith('descendants') else 'PS5_PROTON_PROCESS_SURVIVED_PARENT'
            row['survived_parent']=marker+' PASS' in text
        if case=='java':
            row['java_nodes']=len(re.findall(r'OutputDebugStringA "JAVA_NODE PASS depth=',text))
        if case=='graphics':
            row['graphics_metadata_passed']='PS5_PROTON_GRAPHICS_CHILD PASS: Windows parent and inherited pipe' in text
            row['graphics_teardown_passed']='PS5_PROTON_GRAPHICS_CHILD PASS: GPU initialized' in text
        row['passed']=result.returncode==0 and row['verdict']=='PASS' and row.get('survived_parent',True) and row.get('descendant_nodes',0)==row.get('expected_nodes',0)
        if case=='java':row['passed']=row['passed'] and row['java_nodes']==3
        if case=='graphics':row['passed']=row['passed'] and row['graphics_metadata_passed'] and row['graphics_teardown_passed']
        rows.append(row)
    record={'probe_sha256':hashlib.sha256(probe.read_bytes()).hexdigest(),'wine_revision':(runtime/'revision').read_text().strip(),'cases':rows,'passed':all(row['passed'] for row in rows)}
    if args.case and 'java' in args.case:record['java_manifest']=manifest
    evidence.write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record,indent=2))
    return 0 if record['passed'] else 1
if __name__=='__main__':raise SystemExit(main())
