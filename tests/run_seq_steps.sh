#!/bin/sh
# SEQ rework C2 step logic (app/seq_steps.c): LENGTH tails, ratchet fill (skip, wrap, snapshot), split bits kept,
# transpose range refusal. Drawing, knobs and the engine are not covered.
# Usage (Git Bash): CLANG=... sh tests/run_seq_steps.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_seq_steps
FLAGS="-std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -DFM1_HOST_TEST -Itests/hostinc -Iapp -Iapp/synth -Isdk/include -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console"
"$CLANG" $FLAGS tests/t_seq_steps.c app/seq_steps.c -o "$OUT.exe"
"$OUT.exe" && echo "seq_steps: all checks passed" || { echo "seq_steps: check at line $? failed"; exit 1; }
