#!/bin/sh
# Run each tests/scripts/*.txt through hobbit-ref in several modes and
# compare each transcript with tests/expected/NAME.out.
#
#   tests/run.sh TAPE.tzx [--update]
#
# Every (script, mode) pair is a job of its own, run from a pool of
# HOBBIT_JOBS (default: the number of cores), longest scripts first, each
# compared straight with the expected transcript. The modes:
#
#   check    ported routines and originals side by side, every call
#            compared (exit status 3 on a mismatch); its transcript is
#            the original's, so it also checks the expected output
#   native   the game with no Z80 at all
#   hybrid   ported routines replacing the originals on the Z80
#   orig     the original alone
#   alone    the clean edition alone (hobbit-clean: clean/, no Z80, no port),
#            with the original's bugs (--original-bugs)
#   fixed    the same with them fixed: the transcript must be the same,
#            unless tests/expected-fixed has one for the script
#
# Default: check and native. HOBBIT_FULL=1 adds hybrid and orig, and
# checks that a second orig run gives the same transcript (determinism).
# HOBBIT_QUICK=1: native only. HOBBIT_MODES="..." names the modes outright.
# (HOBBIT_NATIVE=1 is accepted for the old interface; native always runs.)
#
# A script's first line may be "#opts ARGS" with options for hobbit-ref;
# a "#clean-opts ARGS" line adds options with HOBBIT_RNG=clean only, and a
# "#scene EDITS" line starts it with a saved game to LOAD: the game saved
# at the start, with tools/editsave.py's EDITS made.
# Set HOBBIT_ROM to a 48K ROM file to run with the real ROM (expected
# transcripts then go in tests/expected-rom). BUILD_DIR is where
# hobbit-ref is (default build/); HOBBIT_ONLY limits check and hybrid
# runs to those routines (as --only); HOBBIT_SCRIPTS is a shell pattern
# for the script names to run (default *; several, space-separated).
# HOBBIT_STRICT=1 makes check mode compare the stack below each routine's
# return address too.
#
# HOBBIT_RNG=clean: the proper random number generator instead of the
# original's. The reference is then the native port with it (expected
# output in tests/expected-clean, written by mode "base"), and the modes
# are alone, fixed and check-clean (each clean routine compared with the
# faithful one, which also checks the expected output); HOBBIT_FULL=1
# adds clean (the clean edition's routines in the faithful port's place,
# no Z80), hybrid and base. Quick: alone and fixed.
#
# --update (or a script with no expected output, named in HOBBIT_SCRIPTS)
# first writes the expected transcript from the reference run, then runs
# the modes against it.
set -u
here=$(cd "$(dirname "$0")" && pwd)

if [ "${1:-}" = --job ]; then
  # One job: run.sh --job NAME MODE (settings from the environment).
  name=$2 mode=$3
  script=$here/scripts/$name.txt
  work=$RUN_TOP/$name.$mode.d
  mkdir -p "$work"
  opts=$(sed -n '1s/^#opts //p' "$script")
  # "#clean-opts ARGS": more options, with the clean generator only.
  [ "${HOBBIT_RNG:-}" = clean ] && opts="$opts $(sed -n 's/^#clean-opts //p' "$script")"
  only=${HOBBIT_ONLY:-}
  rng=${HOBBIT_RNG:-}
  case $mode in
  orig | again) args="" ;;
  base) args="--native --rng clean" ;;
  check) args="--check ${HOBBIT_STRICT:+--strict-stack} ${only:+--only $only}" ;;
  hybrid) args="--hybrid ${rng:+--rng $rng} ${only:+--only $only}" ;;
  native) args="--native" ;;
  clean) args="--native --clean" ;;
  check-clean) args="--native --check-clean --check-limit 20 --mutate 4" ;;
  alone) args="--original-bugs" ;;
  fixed) args="" ;;
  *) echo "FAIL $name (unknown mode $mode)"; exit 1 ;;
  esac
  out=$work/out
  # Each run starts with no saved game (scripts can SAVE and LOAD), or
  # for a script with a "#scene EDITS" line, with the game saved at the
  # start and those edits made (tools/editsave.py).
  rm -f "$work/hobbit-save.tap"
  scene=$(sed -n 's/^#scene //p' "$script")
  [ -n "$scene" ] && python3 "$here/../tools/editsave.py" "$RUN_TAPE" "$RUN_TOP/start.tap" "$work/hobbit-save.tap" $scene
  bin=$RUN_BIN
  case $mode in alone | fixed) bin=$RUN_CLEAN_BIN ;; esac
  if ! (cd "$work" && "$bin" ${HOBBIT_ROM:+--rom "$HOBBIT_ROM"} $opts $args "$RUN_TAPE" "$script" \
        > "$out" 2> "$out.err"); then
    echo "FAIL $name ($mode)"; grep -v '^\[ref\]' "$out.err" | head -20; exit 1
  fi
  if [ "$mode" = again ]; then
    cp "$out" "$RUN_TOP/$name.again"
    exit 0
  fi
  if [ -n "${RUN_WRITE:-}" ]; then
    cp "$out" "$RUN_EXPECTED/$name.out"
    echo "WROTE $name"
    exit 0
  fi
  want=$RUN_EXPECTED/$name.out
  # The clean edition with its bugs fixed: the same, except where a fix
  # changes what happens (tests/expected-fixed has those).
  [ "$mode" = fixed ] && [ -f "$here/expected-fixed/$name.out" ] && want=$here/expected-fixed/$name.out
  if ! cmp -s "$want" "$out"; then
    echo "FAIL $name ($mode: transcript differs)"; diff "$want" "$out" | head -20
    exit 1
  fi
  [ "$mode" = orig ] && cp "$out" "$RUN_TOP/$name.orig"
  exit 0
fi

tape=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
update=${2:-}
pattern=${HOBBIT_SCRIPTS:-*}
ncpu=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
jobs=${HOBBIT_JOBS:-$ncpu}
expected=$here/expected
[ -n "${HOBBIT_ROM:-}" ] && expected=$here/expected-rom
full=${HOBBIT_FULL:-}
if [ "${HOBBIT_RNG:-}" = clean ]; then
  expected=$here/expected-clean
  ref=base
  modes="alone fixed check-clean${full:+ clean hybrid base}"
  [ -n "${HOBBIT_QUICK:-}" ] && modes="alone fixed"
else
  ref=orig
  modes="check native${full:+ hybrid orig}"
  [ -n "${HOBBIT_QUICK:-}" ] && modes="native"
fi
modes=${HOBBIT_MODES:-$modes}
mkdir -p "$expected"
top=$(mktemp -d)
trap 'rm -rf "$top"' EXIT
export RUN_TOP=$top RUN_TAPE=$tape RUN_EXPECTED=$expected
export RUN_BIN=${BUILD_DIR:-$here/../build}/hobbit-ref
export RUN_CLEAN_BIN=${BUILD_DIR:-$here/../build}/hobbit-clean
# hobbit-ref limits runs machine-wide; let it use the whole pool.
export HOBBIT_SLOTS=${HOBBIT_SLOTS:-$jobs}
[ -n "${HOBBIT_ROM:-}" ] && export HOBBIT_ROM

# The scripts to run, biggest (roughly: slowest) first.
names=$top/names
set -f # split the pattern list without expanding it here
for p in $pattern; do
  set +f
  for script in "$here"/scripts/$p.txt; do
    [ -f "$script" ] && echo "$(wc -c < "$script") $(basename "$script" .txt)"
  done
  set -f
done | sort -rn | awk '!seen[$2]++ { print $2 }' > "$names"
set +f

# The game saved at the start (for "#scene" scripts): made here, not kept,
# as it holds the game's own tables.
if grep -lq '^#scene ' "$here"/scripts/*.txt; then
  printf '#opts --seed 42\nSAVE\n@key  \n@key  \n' > "$top/save.txt"
  (cd "$top" && rm -f hobbit-save.tap && "$RUN_CLEAN_BIN" ${HOBBIT_ROM:+--rom "$HOBBIT_ROM"} --seed 42 \
    "$tape" save.txt > /dev/null 2>&1 && mv hobbit-save.tap start.tap)
fi

# Pool: each job's output to a log of its own; a job list as "NAME MODE".
pool() {
  xargs -n 2 -P "$jobs" sh -c \
    '"$0" --job "$1" "$2" > "$RUN_TOP/log.$1.$2${RUN_WRITE:+.w}" 2>&1 || echo "$1 $2${RUN_WRITE:+.w}" >> "$RUN_TOP/failed"' "$here/run.sh"
}

# Expected transcripts that need writing first, from the reference run.
: > "$top/write"
: > "$top/skip"
while read -r name; do
  if [ "$update" = --update ]; then
    echo "$name"
  elif [ ! -f "$expected/$name.out" ]; then
    # Someone else's new script, perhaps unfinished: run it with
    # HOBBIT_SCRIPTS naming it (or --update) to write its expected output.
    if [ "$pattern" = "*" ]; then echo "$name" >> "$top/skip"; else echo "$name"; fi
  fi
done < "$names" > "$top/write"
if [ -s "$top/write" ]; then
  sed "s/\$/ $ref/" "$top/write" | RUN_WRITE=1 pool
fi

# Everything else, every mode at once.
while read -r name; do
  grep -qx "$name" "$top/skip" && continue
  grep -qx "$name" "$top/write" && [ ! -f "$expected/$name.out" ] && continue
  for mode in $modes; do echo "$name $mode"; done
  [ -n "$full" ] && [ "$ref" = orig ] && echo "$name again"
done < "$names" | pool

# Report, one line per script, in name order.
fail=0
for name in $(sort "$names"); do
  if grep -qx "$name" "$top/skip"; then
    echo "skip $name (no expected output yet)"; continue
  fi
  [ -f "$top/log.$name.$ref.w" ] && grep -q WROTE "$top/log.$name.$ref.w" && echo "WROTE $name"
  bad=$(grep "^$name " "$top/failed" 2>/dev/null)
  if [ -z "$bad" ] && [ -f "$top/$name.again" ] && [ -f "$top/$name.orig" ] &&
     ! cmp -s "$top/$name.orig" "$top/$name.again"; then
    echo "FAIL $name (not deterministic)"; fail=1; continue
  fi
  if [ -n "$bad" ]; then
    fail=1
    echo "$bad" | while read -r n m; do cat "$top/log.$n.$m"; done
  else
    echo "ok   $name"
  fi
done
exit $fail
