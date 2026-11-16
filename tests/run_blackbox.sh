#!/bin/sh
# Black box / load log host check. Usage (Git Bash): CLANG=... sh tests/run_blackbox.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_blackbox.exe
MSYS_NO_PATHCONV=1 "$CLANG" -std=c11 -O1 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -DFM1_HOST_TEST -Iapp tests/t_blackbox.c app/blackbox.c -o "$OUT"
"$OUT"
