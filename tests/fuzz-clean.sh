#!/bin/sh
# The clean edition against the faithful port on random games: for each
# seed, a script of random commands (tools/fuzz.py), run by hobbit-clean
# --original-bugs and by hobbit-ref --native --rng clean (the reference
# for tests/expected-clean): the transcripts must be the same; and by
# hobbit-clean with the bugs fixed: it must neither crash nor hang.
#
#   tests/fuzz-clean.sh TAPE.tzx FIRST_SEED LAST_SEED [COMMANDS] [ROM]
#
# Work files go to $BUILD_DIR/fuzz-clean/SEED/ (BUILD_DIR default build/).
# Prints a line per seed (with where the transcripts part, if they do) and
# exits 1 if any differ.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
tape=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
first=$2 last=$3 count=${4:-600} rom=${5:-$root/48.rom}
build=${BUILD_DIR:-$root/build}
out=$build/fuzz-clean
jobs=${HOBBIT_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}
mkdir -p "$out"
export root tape count rom build out
seq "$first" "$last" | xargs -P "$jobs" -n 1 sh -c '
  seed=$0; d=$out/$seed
  rm -rf "$d" && mkdir -p "$d" && cd "$d" || exit 1
  python3 "$root/tools/fuzz.py" "$tape" "$seed" "$count" > script.txt
  opts=$(sed -n "1s/^#opts //p" script.txt)
  rm -f hobbit-save.tap
  "$build/hobbit-ref" ${rom:+--rom "$rom"} $opts --native --rng clean "$tape" script.txt > faithful.txt 2> faithful.err
  rm -f hobbit-save.tap
  "$build/hobbit-clean" --original-bugs ${rom:+--rom "$rom"} $opts "$tape" script.txt > clean.txt 2> clean.err
  rm -f hobbit-save.tap
  "$build/hobbit-clean" ${rom:+--rom "$rom"} $opts "$tape" script.txt > fixed.txt 2> fixed.err
  fixed=$(grep -o "\[hang[^]]*\]\|\[crash[^]]*\]" fixed.txt | head -1)
  [ -n "$fixed" ] && fixed=", FIXED EDITION: $fixed"
  if cmp -s faithful.txt clean.txt; then
    echo "seed $seed: same ($(grep -c "^> " faithful.txt) prompts)$fixed"
  else
    echo "seed $seed: DIFFERS from line $(cmp faithful.txt clean.txt | awk "{print \$NF}")$fixed"
  fi' | sort -t' ' -k2 -n > "$out/summary.txt"
cat "$out/summary.txt"
! grep -q "DIFFERS\|FIXED EDITION" "$out/summary.txt"
