#!/bin/sh
# Deferred CLOCK switch host check (app/project.c: groove_settings_clock,
# clock_poll): STOP first, groove_clock_set only on a later poll with the
# transport stopped and settled; plus mutation checks (each rule removed from a
# copy of project.c must make the test fail).
# project.c is linked whole; symbols the rule never reaches stay unresolved.
# Usage (Git Bash): CLANG=... sh tests/run_clock_switch.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
TMP=${TMPDIR:-/tmp}
OUT=$TMP/t_clock_switch.exe
build() { # $1 = project.c to link
  rm -f "$OUT"
  "$CLANG" -std=c11 -O1 -Wall -Wextra -Werror -ffreestanding -fno-builtin -DFM1_HOST_TEST \
    -Itests/hostinc -Iapp -Iapp/synth -Isdk/include -nostdlib -fuse-ld=lld \
    -Wl,/entry:main,/subsystem:console,/force:unresolved \
    tests/t_clock_switch.c "$1" -o "$OUT" > "$OUT.log" 2>&1 || true
  grep -i "error" "$OUT.log" || true
  [ -f "$OUT" ] || { echo "clock switch: build failed (log: $OUT.log)"; exit 1; }
}
build app/project.c
"$OUT" || { echo "clock switch: case $? failed"; exit 1; }
MUT=$TMP/t_clock_switch_mut.c
mutate() { # $1 = name, $2 = sed script; the mutated copy must differ and must fail
  sed "$2" app/project.c > "$MUT"
  if cmp -s app/project.c "$MUT"; then echo "clock switch: mutation '$1' did not apply"; exit 1; fi
  build "$MUT"
  if "$OUT"; then echo "clock switch: mutation '$1' NOT caught"; exit 1; fi
}
mutate "clock_set at once" 's|^        clock_pending = 1;$|        clock_pending = 1; groove_clock_set(mode);|'
mutate "no skipped poll" 's|^    if (clock_pending == 1) { clock_pending = 2; return; }$||'
mutate "no playing check" 's|^    if (clock_playing()) { /\* STOP not run yet.*$|    if (0) {|'
mutate "no settle time" 's|^    if (now - clock_busy_at < CLOCK_SETTLE_MS) return;$||'
mutate "no STOP" 's|^        if (clock_playing()) clock_stop(now);$||'
mutate "no repeated STOP" 's|^        if (now - clock_stop_at >= CLOCK_RESTOP_MS) clock_stop(now);$||'
mutate "pending never ends" 's|^    clock_pending = 0;$||'
rm -f "$MUT" "$OUT"
echo "clock switch: all checks passed (7 mutations caught)"
