#!/bin/sh
# Boot chime host check (user 10.11, F10): onsets 0 / 533 / 1600 / 2400 ms, the chord of all four at 3200 ms,
# each pitch present, peak under 0.89 FS. Usage (Git Bash): CLANG=... sh tests/run_chime.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_chime.exe
SRC=${MFX_SRC:-app/master_fx.c}
"$CLANG" -std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -DFM1_HOST_TEST \
  -Itests/hostinc -Iapp -Isdk/include -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console \
  tests/t_chime.c "$SRC" -o "$OUT"
"$OUT" && echo "chime: all checks passed" || { echo "chime: case $? failed"; exit 1; }
