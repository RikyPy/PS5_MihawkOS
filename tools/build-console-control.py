#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the pinned control payload: bounded retries after accept errors, and `sync` (tools/console_guard.py end_write)."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
REVISION='f3cbf875f89978deae9f855b9be2610cec2818ae'
SOURCE_SHA='f625ff130b4bbca07b92c20184cc69c9cd381d5cd2f9e65ff9d7dcf910bcb83f'
PATCHES=('console-control-accept-retry.patch','console-control-sync.patch')

def main():
    sdk=Path(subprocess.check_output(['bash',str(ROOT/'ps5/tools/setup-sdk.sh')],text=True).strip())
    source=subprocess.check_output(['git','-C',str(ROOT.parent/'PS5_Vulkan'),'show',REVISION+':payload/ps5vkctl/main.c'])
    if hashlib.sha256(source).hexdigest()!=SOURCE_SHA:raise RuntimeError('Control source differs from pin')
    patches=[ROOT/'tools/patches'/name for name in PATCHES]
    inputs={'revision':REVISION,'source_sha256':SOURCE_SHA,'patch_sha256':[hashlib.sha256(p.read_bytes()).hexdigest() for p in patches],
        'sdk_revision':(sdk/'.ps5-sdk-revision').read_text().strip(),'recipe_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    identity=hashlib.sha256(json.dumps(inputs,sort_keys=True).encode()).hexdigest()
    out=ROOT/'.deps/console-control'/identity[:16];out.mkdir(parents=True,exist_ok=True)
    tree=out/'source/payload/ps5vkctl';tree.mkdir(parents=True,exist_ok=True);(tree/'main.c').write_bytes(source)
    for patch in patches:subprocess.run(['patch','--batch','-p1','-d',str(out/'source')],input=patch.read_bytes(),check=True)
    subprocess.run([str(sdk/'bin/prospero-clang'),'-std=c11','-O2','-g','-Wall','-Wextra','-Werror',
        '-o',str(out/'ps5vkctl.elf'),str(tree/'main.c'),'-lSceSystemService','-lSceUserService'],check=True)
    inputs['elf_sha256']=hashlib.sha256((out/'ps5vkctl.elf').read_bytes()).hexdigest()
    (out/'manifest.json').write_text(json.dumps(inputs,indent=2)+'\n')
    print(out)

if __name__=='__main__':main()
