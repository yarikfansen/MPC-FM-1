#!/bin/sh
# KEY / chord names (app/keyscale.c): names and inversions, AUTO first chord,
# chosen KEY and MODE, scale masks, stored byte.
# Usage (Git Bash): CLANG=... sh tests/run_keyscale.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_keyscale
FLAGS="-std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -DFM1_HOST_TEST -Itests/hostinc -Iapp -Isdk/include -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console"
"$CLANG" $FLAGS tests/t_keyscale.c app/keyscale.c -o "$OUT.exe"
"$OUT.exe" && echo "keyscale: all checks passed" || { echo "keyscale: check $? failed"; exit 1; }
