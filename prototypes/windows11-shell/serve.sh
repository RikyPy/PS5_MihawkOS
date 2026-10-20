#!/bin/sh
# Serve the Windows 11 shell POC on http://127.0.0.1:${PORT:-8765}/prototypes/windows11-shell/
# The server root is the repository, because the POC reads the theme's font and icons there.
# The wallpaper is the project's own Aero Meadow, drawn into local/ by tools/make-mihawk-wallpaper.py (the image every
# build stages as proton.bmp) with its night variant; it takes about a minute, so only when the generator changed.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/../.." && pwd)
port=${PORT:-8765}
generator="$repo/tools/make-mihawk-wallpaper.py"
want=$(sha256sum "$generator" | cut -d' ' -f1)
mkdir -p "$here/local"
rm -f "$here/local/wallpaper.jpg"
if [ "$(cat "$here/local/wallpaper.sha256" 2>/dev/null)" != "$want" ] || [ ! -f "$here/local/wallpaper.png" ] ||
   [ ! -f "$here/local/wallpaper-dark.png" ]; then
  echo "Drawing Aero Meadow (light and dark) into local/ ..." >&2
  python3 "$generator" "$here/local/wallpaper.png"
  python3 "$generator" "$here/local/wallpaper-dark.png" --dark
  echo "$want" > "$here/local/wallpaper.sha256"
fi
echo "Open http://127.0.0.1:$port/prototypes/windows11-shell/"
exec python3 -m http.server "$port" --bind 127.0.0.1 --directory "$repo"
