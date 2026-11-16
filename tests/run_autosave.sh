#!/bin/sh
# Autosave rest rule host check (app/project.c: groove_storage_touch / _rest):
# 20 s without controls, transport stopped, 1.5 s quiet, counter wrap.
# project.c is linked whole; symbols the rule never reaches stay unresolved.
# Any clang, no C library.
# Usage (Git Bash): CLANG=... sh tests/run_autosave.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_autosave.exe
rm -f "$OUT"
"$CLANG" -std=c11 -O1 -Wall -Wextra -Werror -ffreestanding -fno-builtin -DFM1_HOST_TEST \
  -Itests/hostinc -Iapp -Iapp/synth -Isdk/include -nostdlib -fuse-ld=lld \
  -Wl,/entry:main,/subsystem:console,/force:unresolved \
  tests/t_autosave.c app/project.c -o "$OUT" > "$OUT.log" 2>&1 || true
grep -i "error" "$OUT.log" || true
[ -f "$OUT" ] || { echo "autosave: build failed (log: $OUT.log)"; exit 1; }
"$OUT" && echo "autosave: all checks passed" || { echo "autosave: case $? failed"; exit 1; }
