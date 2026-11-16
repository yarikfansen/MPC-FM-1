#!/bin/sh
# MICRO D host volume guard host check (app/micro_d.c micro_d_host_guard,
# fix 11.11): an audible PSG host goes silent again within 250 ms, by a plain
# CC 7 on a MICRO D-only channel, by groove_control_edit otherwise; other
# tracks are never touched. Plus mutation checks on copies of micro_d.c and a
# check that groove_project_poll calls the guard.
# Usage (Git Bash): CLANG=... sh tests/run_md_host_guard.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
TMP=${TMPDIR:-/tmp}
OUT=$TMP/t_md_host_guard.exe
FLAGS="-std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -DFM1_HOST_TEST -Itests/hostinc -Iapp -Iapp/synth -Isdk/include -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console"
build() { rm -f "$OUT"; MSYS_NO_PATHCONV=1 "$CLANG" $FLAGS $2 tests/t_md_host_guard.c "$1" -o "$OUT"; }
build app/micro_d.c
"$OUT" || { echo "md host guard: case $? failed"; exit 1; }
grep -q "micro_d_host_guard(fm1_millis())" app/project.c || { echo "md host guard: groove_project_poll does not call the guard"; exit 1; }
MUT=$TMP/t_md_host_guard_mut.c
mutate() { # $1 = name, $2 = sed script; the mutated copy must differ and must fail
  sed "$2" app/micro_d.c > "$MUT"
  if cmp -s app/micro_d.c "$MUT"; then echo "md host guard: mutation '$1' did not apply"; exit 1; fi
  build "$MUT" -w
  if "$OUT"; then echo "md host guard: mutation '$1' NOT caught"; exit 1; fi
}
mutate "guard does nothing" 's|^    if (now_ms - checked < MD_GUARD_MS) return;|    return;|'
mutate "every poll" 's|^    if (now_ms - checked < MD_GUARD_MS) return;||'
mutate "CC on a shared channel" 's|^        if (!shared \&\& !(soft \& bit)) {|        if (!(soft \& bit)) {|'
mutate "always control_edit" 's|^        if (!shared \&\& !(soft \& bit)) {|        if (0) {|'
mutate "no fallback" 's|if (!fm1_midi_post(m, 3)) { soft \|= bit; continue; }|(void)fm1_midi_post(m, 3); continue;|'
mutate "any engine" 's/ || v.type != GROOVE_PSG//'
mutate "switched-off track" 's/ || groove_track_role(t) == GROOVE_TRACK_OFF) {/) {/'
mutate "tracks without MICRO D" 's/if (!owner\[t\] || !v.volume/if (!v.volume/; s/if (owner\[t\]) groove_get_view/groove_get_view/'
rm -f "$MUT" "$OUT"
echo "md host guard: all checks passed (8 mutations caught)"
