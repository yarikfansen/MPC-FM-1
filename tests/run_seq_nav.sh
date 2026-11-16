#!/bin/sh
# SEQ rework C1 navigation (app/seq_nav.h): SEQ tap, SELECT pages 1-3 with stops, MIDI row, page indicator,
# VARIATION knob mapping, inactive knobs. Drawing and the screen.c button handler are not covered.
# Usage (Git Bash): CLANG=... sh tests/run_seq_nav.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_seq_nav
FLAGS="-std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -DFM1_HOST_TEST -Itests/hostinc -Iapp -Isdk/include -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console"
"$CLANG" $FLAGS tests/t_seq_nav.c -o "$OUT.exe"
"$OUT.exe" && echo "seq_nav: all checks passed" || { echo "seq_nav: check at line $? failed"; exit 1; }
