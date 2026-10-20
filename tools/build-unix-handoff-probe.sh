#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The unix-socket handoff probe (tests/unix-handoff-probe.c) as a payload ELF,
# and the same source on Linux as its oracle. Never changes production.
set -euo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
sdk=$root/.deps/native/ps5-payload-sdk
output=$root/.deps/unix-handoff-probe
mkdir -p "$output"
cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread "$root/tests/unix-handoff-probe.c" -o "$output/host"
"$sdk/bin/prospero-clang" -std=gnu11 -O2 -Wall -Wextra -Werror \
    "$root/tests/unix-handoff-probe.c" -o "$output/probe.elf"
python3 - "$root" "$sdk" "$output" <<'PY'
from pathlib import Path
import hashlib,json,sys
root,sdk,out=map(Path,sys.argv[1:4])
digest=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
record={'source_sha256':digest(root/'tests/unix-handoff-probe.c'),
        'sdk_revision':(sdk/'.ps5-sdk-revision').read_text().strip(),
        'elf_sha256':digest(out/'probe.elf'),'host_sha256':digest(out/'host')}
(out/'build.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2))
PY
