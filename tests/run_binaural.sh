#!/bin/sh
# BINAURAL host check (app/binaural.c): bounded output, exact beat, no clicks,
# timer fade to silence, no work when off; then x86 cycles per frame.
# Usage (Git Bash): CLANG=... sh tests/run_binaural.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_binaural
FLAGS="-std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -DFM1_HOST_TEST -Itests/hostinc -Iapp -Isdk/include -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console"
"$CLANG" $FLAGS tests/t_binaural.c app/binaural.c -o "$OUT.exe"
"$OUT.exe" && echo "binaural: all checks passed" || { echo "binaural: check $? failed"; exit 1; }
for c in 1 2 3 4 5; do
  "$CLANG" $FLAGS -DBIN_CYCLES=$c tests/t_binaural.c app/binaural.c -o "$OUT$c.exe"
  set +e; "$OUT$c.exe"; n=$?; set -e
  echo "cycles/frame case $c: $n  (1 tones+pink, 2 tones+rain, 3 crossfeed, 4 all, 5 off)"
done
