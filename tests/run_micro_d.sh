#!/bin/sh
# MICRO D host check: renders every preset to WAV and checks levels, then
# compares the code with a reference build of app/micro_d.c from before the
# W4 CPU diet (commit MD_REF, default 3898260 = HEAD when W4 started; the
# reference gets today's 2-voice limit): level, octave bands and peak per
# preset, KEY mode idle, operation counts; then the 2-voice limit against
# the code before it (MD_PREV, default e7bb82c).
# Usage (Git Bash): CLANG=... TMPDIR=... sh tests/run_micro_d.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
REF=${MD_REF:-3898260}
TMP=${TMPDIR:-/tmp}
OUT=$TMP/t_micro_d.dll
REF_OUT=$TMP/t_micro_d_ref.dll
FLAGS="-std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -DFM1_HOST_TEST -Itests/hostinc -Iapp -Iapp/synth -Isdk/include -nostdlib -shared -fuse-ld=lld -Wl,/noentry"
MSYS_NO_PATHCONV=1 "$CLANG" $FLAGS tests/t_micro_d.c app/micro_d.c -o "$OUT"
git show "$REF:app/micro_d.c" > "$TMP/md_ref_src.c"
python tests/t_micro_d.py --make-ref "$TMP/md_ref_src.c" "$TMP/md_ref.c"
MSYS_NO_PATHCONV=1 "$CLANG" $FLAGS tests/t_micro_d.c "$TMP/md_ref.c" -o "$REF_OUT"
# The code just before the 2-voice limit (user 10.11): 1 / 2 notes must render bit-identical.
PREV=${MD_PREV:-e7bb82c}
PREV_OUT=$TMP/t_micro_d_prev.dll
git show "$PREV:app/micro_d.c" > "$TMP/md_prev.c"
MSYS_NO_PATHCONV=1 "$CLANG" $FLAGS tests/t_micro_d.c "$TMP/md_prev.c" -o "$PREV_OUT"
python tests/t_micro_d.py "$OUT" "$REF_OUT" "$PREV_OUT"
