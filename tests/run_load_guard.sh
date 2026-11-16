#!/bin/sh
# Master FX load guard (app/audio_output.c) and sliced effect clearing
# (app/master_fx.c): one trip under a lasting overload, come-back with
# hysteresis, windowed CPU peak, <= ~4 KB cleared per block, no output step.
# Usage (Git Bash): CLANG=... sh tests/run_load_guard.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_load_guard
FLAGS="-std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -DFM1_HOST_TEST -Itests/hostinc -Iapp -Isdk/include -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console"
"$CLANG" $FLAGS tests/t_load_guard.c app/audio_output.c -o "$OUT.exe"
"$OUT.exe" || { echo "load guard: case $? failed"; exit 1; }
# Mutation checks (fix 11.11: shed only after 3 blocks in a row, costs learned
# min-biased, the cut immediate): each rule broken in a copy of audio_output.c
# must make the test fail.
MUT=${TMPDIR:-/tmp}/t_load_guard_mut.c
mutate() { # $1 = name, $2 = sed script
  sed "$2" app/audio_output.c > "$MUT"
  if cmp -s app/audio_output.c "$MUT"; then echo "load guard: mutation '$1' did not apply"; exit 1; fi
  rm -f "$OUT.exe"
  "$CLANG" $FLAGS -w tests/t_load_guard.c "$MUT" -o "$OUT.exe"
  if "$OUT.exe"; then echo "load guard: mutation '$1' NOT caught"; exit 1; fi
}
mutate "shed on the first block" 's|OVER_BLOCKS = 3, |OVER_BLOCKS = 1, |'
mutate "over count not reset" 's|^    } else over_blocks = 0;|    }|'
mutate "never shed" 's|^        if (over_blocks >= OVER_BLOCKS) {|        if (0) {|'
mutate "cut waits too" 's|> limit \&\& worth(kept \| fading, block)) {|> limit \&\& worth(kept \| fading, block) \&\& over_blocks >= OVER_BLOCKS) {|'
mutate "higher cost at once" 's|^    else \*cost += ((seen - \*cost) >> 3) + 1U;|    else *cost = seen;|'
mutate "higher cost never" 's|^    else \*cost += ((seen - \*cost) >> 3) + 1U;|    else {}|'
mutate "lower cost slowly" 's|^    if (!\*cost \|\| seen <= \*cost) \*cost = seen;|    if (!*cost) *cost = seen; else if (seen <= *cost) *cost -= (*cost - seen) >> 3;|'
# Diagnostics of command 23 version 2 (audit A1)
mutate "guard peak not held" 's|^    if (now > guard_now_peak) guard_now_peak = now;||'
mutate "every run end is a lone spike" 's|if (over_before == 1U \&\& !over_blocks)|if (over_before \&\& !over_blocks)|'
mutate "guard peak not cleared" 's|^    guard_now_peak = 0; fm1_panel_irq_peak = 0;|    fm1_panel_irq_peak = 0;|'
rm -f "$MUT" "$OUT.exe"
echo "load guard: all checks passed (10 mutations caught)"
