#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Wine consumes the foundation's optional PE64 WGL/Zink artifacts.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
foundation=$(bash "$root/tools/setup-graphics-foundation.sh")
work=$root/.deps/mesa-foundation-wgl
bash "$foundation/tools/build-mesa-windows-zink.sh" "$work" "$root/../PS5_Mesa" "$root/.deps/llvm-mingw.tar.xz"
python3 - "$foundation" "$work" <<'PYMANIFEST'
import json,sys
from pathlib import Path
foundation,work=map(Path,sys.argv[1:]);path=work/'manifest.json'
manifest=json.loads(path.read_text());manifest['foundation_revision']=(foundation/'.revision').read_text().strip()
path.write_text(json.dumps(manifest,indent=2)+'\n')
PYMANIFEST
