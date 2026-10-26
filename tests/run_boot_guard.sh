#!/bin/sh
# Builds and runs the boot journal host test with any clang (no C library needed).
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_boot_guard.exe
"$CLANG" -std=c11 -O1 -Wall -Wextra -Werror -ffreestanding -fno-builtin -DFM1_HOST_TEST \
  -Iapp -Isdk/include -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console \
  tests/t_boot_guard.c app/boot_guard.c -o "$OUT"
"$OUT" && echo "boot guard: all checks passed" || { echo "boot guard: check $? failed"; exit 1; }
