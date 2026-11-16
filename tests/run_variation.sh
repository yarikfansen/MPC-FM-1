#!/bin/sh
# SEQ rework C3: app/variation.c with a stub engine: CHANCE patterns mute steps (non-destructive), a muted step plays no
# ratchet hits, old CHANCE bytes load as OFF, MUTATE in key (fixed key, AUTO without a chord = octaves).
# Usage (Git Bash): CLANG=... sh tests/run_variation.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_variation
FLAGS="-std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -DFM1_HOST_TEST -Itests/hostinc -Iapp -Iapp/synth -Isdk/include -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console"
"$CLANG" $FLAGS tests/t_variation.c app/variation.c app/seq_steps.c app/keyscale.c -o "$OUT.exe"
"$OUT.exe" && echo "variation: all checks passed" || { echo "variation: check at line $? failed"; exit 1; }
