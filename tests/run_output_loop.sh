#!/bin/sh
# Audit A7: the 32-bit output loop of app/audio_output.c (output_scale) is
# bit-identical to the 64-bit loop it replaced, for every gain, USB state and
# int32 input (tests/t_output_loop.c). Mutations of the new loop must fail.
# Usage (Git Bash): CLANG=... sh tests/run_output_loop.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
OUT=${TMPDIR:-/tmp}/t_output_loop
FLAGS="-std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -DFM1_HOST_TEST -Itests/hostinc -Iapp -Isdk/include -Itests -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console"
"$CLANG" $FLAGS tests/t_output_loop.c -o "$OUT.exe"
"$OUT.exe" || { echo "output loop: part $? differs from the old loop"; exit 1; }
# The same at -O0 (no vectoriser between the two loops).
"$CLANG" $FLAGS -O0 tests/t_output_loop.c -o "$OUT.exe"
"$OUT.exe" || { echo "output loop (-O0): part $? differs from the old loop"; exit 1; }
# Mutations: t_output_loop.c includes "../app/audio_output.c" relative to its
# own folder, so a mutated copy of both goes to a scratch tree.
MUT=${TMPDIR:-/tmp}/t_output_loop_mut
mutate() { # $1 = name, $2 = sed script
  rm -rf "$MUT"; mkdir -p "$MUT/tests" "$MUT/app"
  cp tests/t_output_loop.c "$MUT/tests/"
  sed "$2" app/audio_output.c > "$MUT/app/audio_output.c"
  if cmp -s app/audio_output.c "$MUT/app/audio_output.c"; then echo "output loop: mutation '$1' did not apply"; exit 1; fi
  rm -f "$OUT.exe"
  "$CLANG" $FLAGS -w "$MUT/tests/t_output_loop.c" -o "$OUT.exe"
  if "$OUT.exe"; then echo "output loop: mutation '$1' NOT caught"; exit 1; fi
}
mutate "plain 32-bit multiply" 's|^        int32_t value = (x >> 7) \* (int32_t)gain + .*$|        int32_t value = (int32_t)((uint32_t)x * gain) >> 7;|'
mutate "low bits dropped" 's|+ (int32_t)(((uint32_t)(x \& 127) \* gain) >> 7);|;|'
mutate "clip one off" 's|value > 8388607 ? 8388607 : value < -8388608 ? -8388608 : value;$|value > 8388607 ? 8388607 : value < -8388608 ? -8388607 : value;|'
mutate "no upper clip" 's|usb_frames == -2 ? 0 : value > 8388607 ? 8388607 : value < -8388608|usb_frames == -2 ? 0 : value < -8388608|'
mutate "USB reset not silenced" 's|output\[i\] = usb_frames == -2 ? 0 : value > 8388607|output[i] = value > 8388607|'
mutate "USB input in 32 bits" 's|value += ((int64_t)usb_pcm\[i\] \* ((i \& 1U) ? right : left)) >> 7;|value += (int32_t)((uint32_t)usb_pcm[i] * ((i \& 1U) ? right : left)) >> 7;|'
rm -rf "$MUT" "$OUT.exe"
echo "output loop: old and new loops identical (6 mutations caught)"
