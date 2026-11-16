#!/bin/sh
# Sampler recording host check (app/recorder.c + app/sea_rec.c): take length
# limits, heap reserve, early stop, a full-bank take cut into 8 slices and
# encoded in place. Any clang, no C library.
# Usage (Git Bash): CLANG=... sh tests/run_recorder.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_recorder.exe
"$CLANG" -std=c11 -O1 -Wall -Wextra -Werror -ffreestanding -fno-builtin -DFM1_HOST_TEST \
  -Itests/hostinc -Iapp -Iapp/synth -Isdk/include -Iweb/synth -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console \
  tests/t_recorder.c app/sea_rec.c web/synth/sea_decode.c -o "$OUT"
"$OUT" && echo "recorder: all checks passed" || { echo "recorder: check $? failed"; exit 1; }
