#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# One small native ABI probe and the same POSIX oracle. Never changes production.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
sdk=$root/.deps/native/ps5-payload-sdk
for probe in physical-shared-probe physical-shared-transfer; do
output=$root/.deps/$probe
mkdir -p "$output"
cc -std=gnu11 -O2 -Wall -Wextra -Werror "$root/tests/$probe.c" -o "$output/host"
"$sdk/bin/prospero-clang" -std=gnu11 -O2 -Wall -Wextra -Werror -DPW_PHYSICAL_SHARED_PS5 \
    "$root/tests/$probe.c" -lkernel_sys -o "$output/probe.elf"
python3 - "$root" "$sdk" "$output" "$probe" <<'PY'
from pathlib import Path
import hashlib,json,sys
root,sdk,out=map(Path,sys.argv[1:4])
probe=sys.argv[4]
digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
record={'source_sha256':digest(root/('tests/'+probe+'.c')),
        'sdk_revision':(sdk/'.ps5-sdk-revision').read_text().strip(),
        'elf_sha256':digest(out/'probe.elf'),'host_sha256':digest(out/'host')}
(out/'build.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2))
PY
done
