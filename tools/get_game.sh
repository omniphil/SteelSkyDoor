#!/bin/sh
# Fetches Beneath a Steel Sky, which Revolution Software released as freeware.
#
# The CD version, for the speech and the longer intro. sky.cpt is ScummVM's own
# engine data rather than game data, and ships inside this archive too.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$HERE/../data"
URL="https://downloads.scummvm.org/frs/extras/Beneath%20a%20Steel%20Sky/bass-cd-1.2.zip"

mkdir -p "$DEST"
cd "$DEST"

if [ -f sky.dsk ] && [ -f sky.cpt ] && [ -f sky.dnr ]; then
    echo "Game data already present in $DEST"
else
    if [ ! -f bass-cd-1.2.zip ]; then
        echo "Downloading Beneath a Steel Sky (CD version, about 66 MiB)..."
        curl -L -o bass-cd-1.2.zip "$URL"
    fi
    echo "Extracting..."
    python3 - <<'PY'
import zipfile, os, shutil
z = zipfile.ZipFile("bass-cd-1.2.zip")
for info in z.infolist():
    name = os.path.basename(info.filename)
    if name in ("sky.dsk", "sky.cpt", "sky.dnr"):
        with z.open(info) as src, open(name, "wb") as dst:
            shutil.copyfileobj(src, dst)
        print("  ", name)
PY
fi

# The engine refuses to start on any other size, so check it here.
CPT=$(wc -c < sky.cpt)
if [ "$CPT" != "419427" ]; then
    echo "sky.cpt is $CPT bytes, expected 419427 -- the engine will refuse it" >&2
    exit 1
fi

echo "Game data ready:"
ls -l sky.dsk sky.cpt sky.dnr | awk '{printf "  %-10s %12s bytes\n", $9, $5}'
