#!/bin/sh
# Master FX host check: every filter/ring type stays finite and bounded at
# extreme settings. Usage (Git Bash): CLANG=... sh tests/run_master_fx.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_master_fx.exe
"$CLANG" -std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -DFM1_HOST_TEST \
  -Itests/hostinc -Iapp -Isdk/include -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console \
  tests/t_master_fx.c app/master_fx.c -o "$OUT"
"$OUT" && echo "master fx: all checks passed" || { echo "master fx: case $? failed"; exit 1; }
