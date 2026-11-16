#!/bin/sh
# MASTER FX REVERB (FDN-8, app/master_fx.c run_verb): impulse / burst metrics per type, full-scale sine sweep
# without clamps, fuzz (finite, no subnormal states), exact silence after the tail, TYPE change / shed without a
# step (also REVERB alone: the 20 Hz filter fades out on the integer path), MIX 0 bit-identical to the code
# before; then mutations of master_fx.c that the test must catch.
# Usage (Git Bash): CLANG=... TMPDIR=<Windows path> sh tests/run_reverb.sh
set -e
cd "$(dirname "$0")/.."
CLANG=${CLANG:-clang}
TMP=${TMPDIR:-/tmp}
FLAGS="-std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin -DFM1_HOST_TEST -Itests/hostinc -Iapp -Isdk/include -nostdlib -fuse-ld=lld -Wl,/entry:main,/subsystem:console"
"$CLANG" $FLAGS tests/t_reverb.c -o "$TMP/t_reverb.exe"
"$TMP/t_reverb.exe" || { echo "reverb: case $? failed"; exit 1; }
# Mutations: each line = "name :: sed expression"; the mutated copy must compile and the test must fail.
caught=0; total=0
tr -d "\r" < app/master_fx.c > "$TMP/mfx_orig.c" # sed here may drop CR: compare like with like
SRC="$(printf '%s' "$TMP" | tr '\134' '/')/mfx_mut.c"
while IFS= read -r line; do
  [ -z "$line" ] && continue
  name=${line%% :: *}; expr=${line#* :: }
  total=$((total + 1))
  sed "$expr" "$TMP/mfx_orig.c" > "$TMP/mfx_mut.c" || { echo "mutation '$name': bad sed"; exit 1; }
  if cmp -s "$TMP/mfx_orig.c" "$TMP/mfx_mut.c"; then echo "mutation '$name' did not apply"; exit 1; fi
  "$CLANG" $FLAGS "-DMFX_SRC=\"$SRC\"" tests/t_reverb.c -o "$TMP/t_reverb_mut.exe" 2>/dev/null || { echo "mutation '$name' does not compile"; exit 1; }
  if "$TMP/t_reverb_mut.exe"; then echo "mutation '$name' NOT caught"; exit 1; fi
  caught=$((caught + 1))
done <<'MUT'
no Householder mix :: s/        s \*= 0.25f;/        s *= 0.0f;/
no diffusers :: s/            x = b - gd \* w;/            x = x + 0.0f * b;/
no damping filter :: s/rv_a\[k\] = g \* (1.0f - b); rv_b\[k\] = b;/rv_a[k] = g; rv_b[k] = 0;/
no modulation :: s/float d = (float)len0 - md \*/float d = (float)len0 - 0.0f */
input gain not per DECAY :: s/    float nrm2 = 1.0f - g \* g;/    float nrm2 = 0.5f;/
rounding instead of truncation :: s/    int32_t q = (int32_t)s;/    int32_t q = (int32_t)(s + (s > 0 ? 0.5f : -0.5f));/
no denormal guard in damping :: s/f\[k\] = (ca\[k\] \* v\[k\] + cb\[k\] \* f\[k\] + RV_DN) - RV_DN;/f[k] = ca[k] * v[k] + cb[k] * f[k];/
no block-end flush :: s/    if (fabsf_(hp) < 1e-15f) hp = 0.0f;/    if (hp == 12345.0f) hp = 0.0f;/
mono output :: s/const float gm = 0.5f \* rv_gout, gs = 0.5f \* rv_gout \* t->wid;/const float gm = 0.5f * rv_gout, gs = 0.0f * t->wid;/
HALL preset in the PLATE slot :: s/0.4f, 2.4854268f, 17.4612703f/1.1111111f, 2.7369656f, 9.0664288f/
MIX 0 still runs the reverb :: s/#define FX_RUN(fx, call) do { if (ep\[fx\]\[3\])/#define FX_RUN(fx, call) do { if (ep[fx][3] || fx == MFX_VERB)/
clear slice too big :: s/enum { FX_CLEAR_BUDGET = 4096 };/enum { FX_CLEAR_BUDGET = 40960 };/
20 Hz filter dropped on the integer path (the old step) :: s/        if (sub_g > 0.0f) subcut_fade_out(buffer);/        if (sub_g > 0.0f) { sub_g = 0.0f; subcut_rest(); }/
20 Hz filter restarts at full on return :: s/        if (from <= 0.0f) from = to = 1.0f;/        if (from < 1.0f) from = to = 1.0f;/
20 Hz filter never back to rest :: s/    if (to <= 0.0f) subcut_rest();/    if (to < -1.0f) subcut_rest();/
20 Hz filter fade-out without its correction :: s/            float corr = g [*] (z - x) [*] 8388608.0f;/            float corr = 0.0f * (z - x);/
24-bit clamp ahead of the limiter in the fade :: s/            buffer\[2 \* i + c\] = v + (int32_t)corr;/            v += (int32_t)corr; buffer[2 * i + c] = v > 8388607 ? 8388607 : v < -8388607 ? -8388607 : v;/
MUT
echo "reverb: all checks passed, $caught / $total mutations caught"
