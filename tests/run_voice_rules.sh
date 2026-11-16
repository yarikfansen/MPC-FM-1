#!/bin/sh
# 303 = MONO always (app/voice_rules.h): which tracks the control task
# switches to MONO.
# Usage (Git Bash): CLANG=... sh tests/run_voice_rules.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_voice_rules
FLAGS="-std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -DFM1_HOST_TEST -Itests/hostinc -Iapp -Iapp/synth -Isdk/include -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console"
"$CLANG" $FLAGS tests/t_voice_rules.c -o "$OUT.exe"
"$OUT.exe" && echo "voice rules: all checks passed" || { echo "voice rules: case $? failed"; exit 1; }
