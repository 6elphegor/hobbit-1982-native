#!/bin/sh
# Run random command scripts (tools/fuzz.py) in check mode, several seeds
# in parallel, and summarise: mismatches between the ported routines and
# the originals, crashes of the original game, and hangs.
#
#   tests/fuzz.sh TAPE.tzx FIRST_SEED LAST_SEED [COMMANDS] [ROM]
#
# Work files go to $BUILD_DIR/fuzz/SEED/ (BUILD_DIR default build/); a
# mismatch leaves the state before the call in mismatch.bin there, for
# hobbit-replay. HOBBIT_ONLY limits the routines checked (as --only).
# HOBBIT_NATIVE=1 also runs each script with no Z80 (--native) and compares
# its transcript with the original's.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
tape=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
first=$2 last=$3 count=${4:-1000} rom=${5:-$root/48.rom}
build=${BUILD_DIR:-$root/build}
out=$build/fuzz
only=${HOBBIT_ONLY:-}
mkdir -p "$out"
run_seed() {
  d=$out/$1
  rm -rf "$d" && mkdir -p "$d" && cd "$d" || return
  python3 "$root/tools/fuzz.py" "$tape" "$1" "$count" > script.txt
  # The script's seed, for every run: after a restart the seed comes from
  # the R register, which native code does not reproduce.
  opts=$(sed -n '1s/^#opts //p' script.txt)
  rm -f hobbit-save.tap
  "$build/hobbit-ref" ${rom:+--rom "$rom"} $opts --check ${only:+--only "$only"} --max-idle 60000000 \
    --dump-mismatch mismatch.bin "$tape" script.txt > out.txt 2> err.txt
  prompts=$(grep -c '^> ' out.txt)
  native=""
  if [ -n "${HOBBIT_NATIVE:-}" ]; then
    # The same script with no Z80, against the original alone.
    # Each run starts with no saved game (scripts can SAVE and LOAD).
    rm -f hobbit-save.tap
    "$build/hobbit-ref" ${rom:+--rom "$rom"} $opts --max-idle 60000000 "$tape" script.txt > orig.txt 2> orig.err
    rm -f hobbit-save.tap
    "$build/hobbit-ref" ${rom:+--rom "$rom"} $opts --native --max-idle 60000000 "$tape" script.txt > native.txt 2> native.err
    if cmp -s orig.txt native.txt; then native=", native same"; else native=", NATIVE DIFFERS"; fi
  fi
  if grep -q '[1-9][0-9]* mismatches' err.txt; then result="MISMATCH"
  elif grep -q '^\[crash' out.txt; then result="crash"
  elif grep -q 'stopped asking' err.txt; then result="hang: $(grep -o 'PC=.*' err.txt)"
  elif grep -q 'did not return' err.txt; then result="hang inside $(grep -o '\] [A-Za-z0-9]* (\$[0-9A-F]*)' err.txt | head -1 | cut -c3-)"
  else result="ok"; fi
  echo "seed $1: $prompts commands, $result$native"
}
export -f run_seed 2>/dev/null
for seed in $(seq "$first" "$last"); do run_seed "$seed" & done
wait
