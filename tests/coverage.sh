#!/bin/sh
# Build hobbit-ref with coverage, run every test script in check mode, and
# list the lines of the ported code that no test reached.
#
#   tests/coverage.sh TAPE.tzx [ROM] [FILES]
#
# FILES: the port/ files to report on (default all). BUILD_DIR as for
# run.sh (coverage goes to $BUILD_DIR/cov); HOBBIT_ONLY and HOBBIT_SCRIPTS
# limit the routines checked and the scripts run; EXTRA_SCRIPTS is a file
# listing more script files, one path per line (e.g. fuzz scripts).
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
tape=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
rom=${2:-}
[ -n "$rom" ] && rom=$(cd "$(dirname "$rom")" && pwd)/$(basename "$rom")
files=${3:-$(cd "$root" && ls port/*.c)}
only=${HOBBIT_ONLY:-}
pattern=${HOBBIT_SCRIPTS:-*}
cov=${BUILD_DIR:-$root/build}/cov
rm -rf "$cov" && mkdir -p "$cov/run"
for f in ref/refrun.c ref/script.c ref/spectrum.c ref/hybrid.c third_party/z80/z80.c common/tzx.c port/*.c clean/*.c; do
  o=$(basename "$f" .c)
  case $f in clean/*) o=clean_$o ;; esac # (clean/text.c and port/text.c, ...)
  (cd "$root" && cc -O0 -g --coverage -DCLEAN_ADAPTERS -c -o "$cov/$o.o" "$f") || exit 1
done
cc --coverage -o "$cov/hobbit-ref" "$cov"/*.o || exit 1
{ for f in "$root"/tests/scripts/$pattern.txt; do echo "$f"; done
  [ -n "${EXTRA_SCRIPTS:-}" ] && cat "$EXTRA_SCRIPTS"; } |
while IFS= read -r script; do
  opts=$(sed -n '1s/^#opts //p' "$script")
  (cd "$cov/run" && rm -f hobbit-save.tap &&
   "$cov/hobbit-ref" ${rom:+--rom "$rom"} $opts --check ${only:+--only "$only"} --max-idle 60000000 \
     "$tape" "$script" > /dev/null 2> err) ||
    { echo "$(basename "$script"): failed"; grep -v '^\[ref\]' "$cov/run/err" | head; }
done
# Report from inside $cov (sources reached through a link), so that runs in
# different build directories do not write over each other's reports.
cd "$cov" || exit 1
ln -sf "$root/port" port
for f in $files; do
  base=$(basename "$f" .c)
  xcrun llvm-cov gcov -o "$cov" "$f" 2>/dev/null | grep -A1 "^File '$f'" | tail -1 | sed "s|^|$f: |"
  [ -f "$base.c.gcov" ] &&
    awk -F: -v f="$f" '$1 ~ /#####/ {sub(/^ +/, "", $2); printf "  %s:%s:%s\n", f, $2, $3}' "$base.c.gcov"
done
rm -f ./*.gcov
