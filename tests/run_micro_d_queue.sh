#!/bin/sh
# MICRO D note queue (app/micro_d.c): one lane per engine core, no note lost
# when both cores queue at once, order per track, queue-full rule.
# Usage (Git Bash): CLANG=... sh tests/run_micro_d_queue.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_micro_d_queue
FLAGS="-std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -DFM1_HOST_TEST -Itests/hostinc -Iapp -Iapp/synth -Isdk/include -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console"
"$CLANG" $FLAGS tests/t_micro_d_queue.c -o "$OUT.exe"
"$OUT.exe" && echo "micro d queue: all checks passed" || { echo "micro d queue: case $? failed"; exit 1; }
