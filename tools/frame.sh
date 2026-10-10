#!/usr/bin/env bash
# tools/frame.sh OUT.png: the game's next presented frame (BB_PRESENT_DUMP_TRIGGER=out/present_trigger,
# BB_DUMP_DIR=out/present, set when the game starts) as a PNG, without a screen capture: works
# while the game window is on another workspace.
# KIND=final: the frame after FSR and post processing instead (BB_FINAL_DUMP_TRIGGER=out/final_trigger).
set -u
cd -- "$(dirname -- "$0")/.."
out=${1:?usage: tools/frame.sh OUT.png}
mkdir -p out/present
before=$(ls out/present 2>/dev/null | wc -l)
kind=${KIND:-present}
touch out/${kind}_trigger
for i in $(seq 1 50); do
    sleep 0.1
    [[ $(ls out/present | wc -l) -gt $before ]] && break
done
sleep 0.3
raw=$(ls -t out/present/*_${kind}_*.raw 2>/dev/null | head -1)
[[ -n $raw ]] || { echo "no frame"; exit 1; }
size=$(basename "$raw" | sed -E "s/.*_${kind}_([0-9]+x[0-9]+)_.*/\\1/")
fmt=$(basename "$raw" | sed -E 's/.*_(rgba|bgra)\.raw/\1/')
if command -v magick > /dev/null; then
    magick -size "$size" -depth 8 "$fmt:$raw" -alpha off "$out" && rm -f "$raw" && echo "$out ($size)"
else # bbcoop: no ImageMagick (the Steam Deck's distrobox): Pillow
    python3 -I -c 'import sys; from PIL import Image; w, h = map(int, sys.argv[2].split("x")); im = Image.frombytes("RGBA", (w, h), open(sys.argv[1], "rb").read()); im = im if sys.argv[3] == "rgba" else Image.merge("RGBA", im.split()[2::-1] + im.split()[3:]); im.convert("RGB").save(sys.argv[4])' "$raw" "$size" "$fmt" "$out" && rm -f "$raw" && echo "$out ($size)"
fi
