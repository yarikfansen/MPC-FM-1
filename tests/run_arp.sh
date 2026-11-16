#!/bin/sh
# ARP host check: plain timing, swing, polyrhythm / mutate ranges, stored state.
# Usage (Git Bash): CLANG=... sh tests/run_arp.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_arp.exe
MSYS_NO_PATHCONV=1 "$CLANG" -std=c11 -O1 -Wall -Wextra -D_CRT_SECURE_NO_WARNINGS -Iapp tests/t_arp.c app/arp.c app/keyscale.c -o "$OUT"
"$OUT"
