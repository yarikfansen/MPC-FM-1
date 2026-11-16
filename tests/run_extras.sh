#!/bin/sh
# Extras host check (app/extras.c): versions, unchanged saves, power loss at
# every step of a save and of its recovery. Any clang, no C library.
# Usage (Git Bash): CLANG=... sh tests/run_extras.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_extras.exe
"$CLANG" -std=c11 -O1 -Wall -Wextra -Werror -ffreestanding -fno-builtin -DFM1_HOST_TEST \
  -Itests/hostinc -Iapp -Iapp/synth -Isdk/include -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console \
  tests/t_extras.c app/extras.c -o "$OUT"
"$OUT" && echo "extras: all checks passed" || { echo "extras: check $? failed"; exit 1; }
