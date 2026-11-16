#!/bin/sh
# F8 screen timing hook (app/ui_meter.c, transfer command 25) host check. Usage (Git Bash): CLANG=... sh tests/run_ui_meter.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_ui_meter.exe
MSYS_NO_PATHCONV=1 "$CLANG" -std=c11 -O1 -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -DFM1_HOST_TEST -Iapp tests/t_ui_meter.c app/ui_meter.c -o "$OUT"
"$OUT"
