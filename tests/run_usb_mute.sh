#!/bin/sh
# F12: the board output (speaker + headphones) is never muted while USB
# records (app/audio_output.c): it always equals the mix USB gets.
# Usage (Git Bash): CLANG=... sh tests/run_usb_mute.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_usb_mute
FLAGS="-std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -DFM1_HOST_TEST -Itests/hostinc -Iapp -Isdk/include -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console"
"$CLANG" $FLAGS tests/t_usb_mute.c app/audio_output.c -o "$OUT.exe"
"$OUT.exe" && echo "usb_mute: all checks passed" || { echo "usb_mute: check $? failed"; exit 1; }
