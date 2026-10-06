#!/bin/sh
# The README's demo, docs/demo.gif (and docs/demo.mp4, with sound): the
# game block loading from tape (tools/tape-intro.py: the loading screen,
# the border stripes, and in the MP4 the tape's sound; the game itself is
# silent), then the clean edition playing docs/demo/demo.txt, recorded by
# hobbit-clean --video, then an end card.
#
#   tools/make-demo.sh [TAPE.tzx] [ROM]
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
tape=${1:-$root/Hobbit, The v1.2 (1982)(Melbourne House).tzx}
rom=${2:-$root/48.rom}
make -C "$root" build/hobbit-clean >/dev/null
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

python3 "$root/tools/tape-intro.py" "$tape" "$rom" "$work"
script=$root/docs/demo/demo.txt
(cd "$work" && "$root/build/hobbit-clean" --rom "$rom" $(sed -n '1s/^#opts //p' "$script") \
  --video "$work/game.rgb" "$tape" "$script" > "$work/game.out" 2>/dev/null)
cat "$work/intro.rgb" "$work/game.rgb" "$work/end.rgb" > "$work/all.rgb"

raw="-f rawvideo -pix_fmt rgb24 -s 320x240 -r 25 -i $work/all.rgb"
ffmpeg -loglevel error -y $raw -i "$work/intro.wav" \
  -filter_complex "[0:v]scale=640:480:flags=neighbor[v];[1:a]apad[a]" -map "[v]" -map "[a]" -shortest \
  -c:v libx264 -pix_fmt yuv420p -crf 18 -c:a aac -b:a 96k "$root/docs/demo.mp4"
ffmpeg -loglevel error -y $raw \
  -vf "fps=12.5,scale=640:480:flags=neighbor,split[a][b];[a]palettegen=max_colors=16[p];[b][p]paletteuse=dither=none" \
  "$root/docs/demo.gif"
ls -l "$root/docs/demo.gif" "$root/docs/demo.mp4"
