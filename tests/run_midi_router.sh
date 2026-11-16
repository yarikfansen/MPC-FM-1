#!/bin/sh
# MIDI router host check (app/midi_router.c, W8): live notes for a channel that
# only reaches MICRO D tracks do not start the silent PSG host voices; mixed
# channels, recording and the instrument ARP keep today's path; note offs and
# all other messages always reach the instrument; MICRO D gets every note.
# Usage (Git Bash): CLANG=... sh tests/run_midi_router.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_midi_router.exe
MSYS_NO_PATHCONV=1 "$CLANG" -std=c11 -O1 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -DFM1_HOST_TEST -Iapp -Iapp/synth -Isdk/include tests/t_midi_router.c -o "$OUT"
"$OUT"
