#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Isolated experimental client; retains PS5_Vulkan's complete RADV link recipe.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
radv=$1 out=$2 tool=$3
# Arguments: pinned RADV checkout, output, pinned PRX converter, title SDK.
sdk=$4
draw_flags=()
[[ ${5:-0} == 1 ]] && draw_flags=(-DPROTON_GPU_DRAW)
[[ ${6:-0} == 1 ]] && draw_flags+=(-DPROTON_GPU_PRESENT)
wsi=${7:-0}
foundation=$(bash "$root/tools/setup-graphics-foundation.sh")
install=$radv/.deps/native/radv-release
mkdir -p "$out/obj" "$out/build" "$out/title-stubs"
cp "$sdk"/target/lib/*.so "$out/title-stubs/"
rm -f "$out/title-stubs/libkernel_web.so" "$out/title-stubs/libkernel_sys.so" "$out/title-stubs/libScePosixForWebKit.so"
# The RADV checkout may be tools/setup-pinned-radv.sh's export, which has no PS5_Mesa beside it: name the fork.
PS5_MESA_FORK=${PS5_MESA_FORK:-$(cd -- "$root/../PS5_Mesa" && pwd)} PS5_VULKAN_BUILD_ROOT="$radv" \
    bash "$foundation/tools/build-gpu-broker-client.sh" "$out" "$sdk" "$wsi" "${5:-0}"
for source in native/proton_gpu_client.c native/proton_gpu_vulkan_probe.c; do
    "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC "${draw_flags[@]}" \
        -I"$install/include" -I"$out/source/src/amd/vulkan/winsys/ps5" \
        -c "$root/$source" -o "$out/obj/$(basename "$source" .c).o"
done
if [[ $wsi == 1 ]]; then
    "$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -I"$install/include" -c "$root/native/proton_gpu_vulkan_exports.c" -o "$out/obj/client_exports.o"
    # pw_module_lookup: ntdll's module loader hands it its lookup as it opens the module in a helper, for the GPU stamp
    # the client makes when it gives the broker up (the graphics foundation's tooling/gpu-broker/client.h).
    python3 "$root/tools/gen_prx_descriptor.py" "$out/obj/client_desc.c" proton_gpu_vulkan_probe vkGetInstanceProcAddr vkGetDeviceProcAddr pw_gpu_vulkan_bind_window \
        pw_module_lookup
else
    python3 "$root/tools/gen_prx_descriptor.py" "$out/obj/client_desc.c" proton_gpu_vulkan_probe
fi
"$sdk/bin/prospero-clang" -std=c11 -O2 -Wall -Wextra -Werror -fPIC -I"$root/wine/ps5" \
    -c "$out/obj/client_desc.c" -o "$out/obj/client_desc.o"
client_exports=()
[[ $wsi == 1 ]] && client_exports=("$out/obj/client_exports.o")
source "$foundation/tools/radv-link.sh"
radv_link_recipe "$out" "$sdk" "$out/client.a"
# Mesa contains weak pthread/libc references as well as optional Vulkan
# entry points. Never zero a function the title SDK actually exports.
python3 - "$sdk" "$out" <<'PYZERO'
import subprocess,sys
from pathlib import Path
sdk,out=map(Path,sys.argv[1:]);nm=str(sdk/'bin/llvm-nm')
weak={p[-1] for line in subprocess.check_output([nm,'-g',str(out/'client.a')],text=True).splitlines()
      if len(p:=line.split())==2 and p[0]=='w'}
exports=set()
for stub in (out/'title-stubs').glob('*.so'):
    exports.update(subprocess.check_output([nm,'-D','--defined-only','-j',str(stub)],text=True).splitlines())
(out/'build/mesa_optional_zero.ld').write_text(''.join('PROVIDE_HIDDEN('+s+' = 0);\n' for s in sorted(weak-exports)))
PYZERO
"$sdk/bin/prospero-lld" --shared -Bsymbolic -T "$radv/tooling/native/ps5-pie.ld" \
    -T "$root/wine/ps5/prx_eh_frame.ld" --eh-frame-hdr -T "$out/build/mesa_optional_zero.ld" \
    -soname gpu-client.prx -z defs --warn-unresolved-symbols -o "$out/client.shared.elf" \
    "$out/obj/proton_gpu_client.o" "$out/obj/proton_gpu_vulkan_probe.o" \
    "${client_exports[@]}" "$out/obj/client_desc.o" \
    "${radv_link_flags[@]}" "${radv_link_inputs[@]}" --as-needed "$out"/title-stubs/*.so > "$out/link.log" 2>&1
# Fail before signing if a native graphics module or unsupported kernel appeared.
python3 - "$out" <<'PY'
import json,subprocess,sys
from pathlib import Path
out=Path(sys.argv[1]);lines=subprocess.check_output(['llvm-readelf','-d',str(out/'client.shared.elf')],text=True)
needed=[s.split('[')[1].split(']')[0] for s in lines.splitlines() if '(NEEDED)' in s]
allowed={'libkernel.sprx','libSceLibcInternal.sprx'}
if not set(needed)<=allowed:raise RuntimeError('Unexpected client module dependencies: '+repr(needed))
(out/'needed.json').write_text(json.dumps(needed)+'\n')
PY
"$tool" link --module --in "$out/client.shared.elf" --out "$out/client.elf" \
    --stub-dir "$out/title-stubs" --module-sdk 0x02000009 --companion-sdk 0x08050001 \
    --file-name gpu-client.prx >> "$out/link.log" 2>&1
"$tool" self --sign --in "$out/client.elf" --out "$out/gpu-client.prx" >> "$out/link.log" 2>&1
python3 - "$root" "$sdk" "$radv" "$out" "$tool" "$wsi" <<'PYMANIFEST'
import json,hashlib,sys
from pathlib import Path
root,sdk,radv,out,tool=map(Path,sys.argv[1:6]);digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
files=['native/proton_gpu_client.c','native/proton_gpu_vulkan_probe.c',
 'native/proton_gpu_vulkan_exports.c','src/pw_gpu_broker.h','tools/setup-graphics-foundation.sh',
 'tools/link_gpu_client.sh','native/proton_gpu_probe_spv.h','tools/gen_prx_descriptor.py','wine/ps5/prx_eh_frame.ld']
manifest={'wsi':sys.argv[6]=='1','sha256':digest(out/'gpu-client.prx'),'elf_sha256':digest(out/'client.elf'),
 'needed':json.loads((out/'needed.json').read_text()),'sdk_revision':(sdk/'.ps5-sdk-revision').read_text().strip(),
 'mesa_revision':'1ba9874f478657ccf6da72ce55f0260164cc0bd0','archive_sha256':digest(radv/'.deps/native/radv-release/lib/libvulkan_radeon.ps5.a'),
 'recipe_sha256':digest(radv/'tools/radv-link.sh'),'tool_sha256':digest(tool),
 'source_sha256':{f:digest(root/f) for f in files}}
manifest['graphics_foundation_revision']=(root/'.deps/graphics-foundation/.revision').read_text().strip()
manifest['graphics_foundation']=json.loads((out/'foundation-manifest.json').read_text())
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
PYMANIFEST
