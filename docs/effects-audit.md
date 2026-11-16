# FM-1 effects DSP audit (read-only, 28.10.2026)

Scope: app/master_fx.c (MASTER FX rows 1-7), engine delay/reverb (MASTER FX rows DELAY/SENDS + TRACK FX),
app/binaural.c (B1 = 3D SOUND), app/micro_d.c ladder/drive. Nothing built or edited.
Engine FX are judged from the open browser copy `web/engine/effects.c` + `web/synth/mixer.c` + `web/engine/sound.c`
(notes.md: "the same instrument for the browser simulator"); the device `libfm1_engine.a` is assumed equal, not verified.
ISR/Flash check done on `build/dx7/sdk.elf` (28.10 05:26) with `C:\JL\pi32\bin\llvm-objdump` + `llvm-nm`.
Block = 128 frames (2.9 ms). Chain: micro_d_render -> mfx (DRIVE, FILTER, CHORUS, PHASER, RING, REVERB, EQ, LOW CUT, OUTPUT limiter) -> rec -> binaural -> output gain/clip.

## 0. Audio ISR calling Flash (rule from sdk/include/fm1.h:15)
| Where | Calls (Flash addr) | Severity | Fix |
|---|---|---|---|
| fm1_output_mix (audio_output.c:29,31) | `__sync_synchronize` @0x0203b358, 2x per block, also while Flash is written | HIGH | single core: replace with `__asm__ volatile("" ::: "memory")` (compiler barrier) or own FM1_AUDIO_CODE barrier |
| fx_clear (master_fx.c:623-639) via fx_smooth | `memset` @0x0203a4c8, 6 call sites | HIGH | own FM1_AUDIO_CODE word-clear loop; fx_clear runs on every effect enable / type change, also when an LFO pushes MIX to 0 and back (see 2) |
| micro_d_render (micro_d.c:550) | whole MICRO D from Flash | known/by design | guarded by md_hold (audio_output.c:31); OK only if every Flash write goes through flash_write_safe/erase_safe |
No other Flash calls from run_*/eq_design/binaural (all float div/sqrt inline, no soft-float helpers).
Side effect of fx_clear: it memsets up to 16 KB (chorus modbuf) or ~17 KB (reverb lines) inside one ISR block -> CPU spike at enable/type change.

## 1. FILTER (master_fx.c:170-251)
| Knob | Range (code) | Issue | Sev | Fix |
|---|---|---|---|---|
| TYPE | ACID, SEM, LADDER, COMB, DJ | type change: 8-block fade out/clear/fade in (618-657) - clean | ok | - |
| CUTOFF | ACID 40 Hz-9 kHz, LADDER 30 Hz-12 kHz, SEM 30 Hz-18 kHz, COMB 40 Hz-1.9 kHz, DJ LP 18k..~35 Hz / off 63-65 / HP 23 Hz..14 kHz | g ramped per block (186, 240) - good. 7-bit knob = 0.07 oct (~0.8 semitone) steps; LFO "FLT CUT" arrives at UI frame rate (lfo.c:164) -> stair-step sweeps at high reso | med | smooth target in DSP: one-pole on fc per block (like cut_smooth) instead of jump to new fc; or fractional LFO offset |
| CUTOFF (COMB) | delay 22-1102 samples | delay length constant per block, steps every block (205-208) -> zipper/pitch steps when sweeping | low | interpolate d per sample across block |
| RESO | ladder k 0..3.9/4.1, SEM Q 0.5..16.7, COMB fb 0..0.96 | k / fb not smoothed (181, 208, 233) -> zipper with LFO "FLT RES"; LADDER self-oscillates at top (by design) | med | ramp k across block (k0->k1) |
| RESO (LADDER) | comp 1+0.55k = 3.25 | output gain grows with RESO up to ~2.8x (192) -> clips when OUTPUT step 0 (no limiter, hard clamp at 698) | med | cap comp (e.g. 1+0.3k) or soft-clip y |
| MIX | 0..1 crossfade | not smoothed per block (689); fast knob = small clicks | low | ramp mix across block (store prev per fx) |
| - | ACID/LADDER | run at 1x, but comments (41,166) and notes.md:99 say 2x; tanh x1.9 drive at 44.1 kHz aliases on bright input | med | fix comments; optional cheap 2x (halfband) for ACID only, CPU allows ~1 more tanh set |
| - | DJ | dead zone (227) returns with stale svf state; on leaving it the old state can click | low | clear svf1/svf2 when entering dead zone |
| - | dead code | svf3/svf4 (jc1/jc2), ld_in, BP mode 1 unused (238-249) | low | remove |
Missing vs similar boxes: HP/BP for SEM (code exists, mode 1 unused), key tracking, envelope follower on cutoff, filter drive knob.

## 2. DRIVE (master_fx.c:252-287)
| Knob | Range | Issue | Sev | Fix |
|---|---|---|---|---|
| TYPE | SOFT, HARD (+-0.7), FOLD (sin), CRUSH | "2x oversampling" (275-277) only averages two shaped samples: aliasing of HARD/FOLD at gain 41 is not removed | med | real 2x: shape mid and x, then halfband (or ADAA 1st order for HARD/SOFT) |
| DRIVE | gain 1..41, makeup 1..0.25; CRUSH levels 2^15..2^3, hold 1..16 | gain jumps per block (261); LFO "DRIVE" target -> zipper | med | ramp gain/makeup across block |
| TONE | one-pole LP 500 Hz..19.7 kHz | 6 dB/oct only | low | - |
| MIX | 0..1 | as FILTER | low | ramp |
DC blocker present (281). CPU: SOFT 2 div/frame/ch.

## 3. CHORUS / FLANGE (master_fx.c:295-323)
| Knob | Range | Issue | Sev | Fix |
|---|---|---|---|---|
| TYPE | CHORUS (2 voices/side, 13.6-27 ms), FLANGE (1 voice, 1-5 ms, fb fixed 0.65) | flanger feedback fixed, no negative fb / manual | low | feature |
| RATE | 0.05-6.4 Hz | phase-continuous - ok | ok | - |
| DEPTH | swing 0..320 / 0..176 samples | delay jumps per block when DEPTH changes (299): 1 knob step = 2.5 samples jump -> clicks; LFO "CHO DEPTH" at UI rate -> crackle | HIGH-med | one-pole smooth `swing` per sample (or ramp across block) |
| MIX | 0..1 crossfade to wet | at 100 % output is wet only = vibrato, no chorus/flange notches (318); strongest effect is at ~50 % | med (UX) | map MIX to wet 0..0.5 with dry kept (x*(1-m/2)+wet*m/2) |
CPU: 4 fsin + 4 Hermite per frame (stereo chorus); LFO sin could be per 16 frames.

## 4. PHASER (master_fx.c:324-354)
| Knob | Range | Issue | Sev | Fix |
|---|---|---|---|---|
| STAGES | 4/6/8 | ok | - | - |
| RATE | 0.05-6.4 Hz | sweep recomputed every 16 frames (stepped, 128 ftan_pi/block at 8 st) | low | fine; CPU ~128 div + 256 sin per block |
| FEEDBK | 0..0.85, positive only | stable (<1); unsmoothed (LFO "PHA FDBK") | low | ramp |
| MIX | 0..1 to 0.5(x+ap) | ok | - | - |
Missing: centre/range, negative feedback, stereo phase knob (fixed 90 deg).

## 5. RING MOD (master_fx.c:372-427)
| Knob | Range | Issue | Sev | Fix |
|---|---|---|---|---|
| WAVE | SINE, SQUARE (BLEP), TRI (naive), SAW (BLEP), NOISE, TRACK, SHIFT, ENV | x*carrier creates sum tones above 22 kHz -> aliasing on bright input with 2-8 kHz carriers (inherent, no oversampling) | low-med | optional: LP input at ~10 kHz when FREQ > 2 kHz |
| FREQ | 1 Hz-8.2 kHz (2^13 over 127 = 1.2 semitone/step); TRACK x0.25..x4 | coarse for tuned ring; TRACK estimate jumps every 2048 samples, zero-cross without hysteresis (383-391) -> octave errors | low | hysteresis + glide trk_freq |
| SPREAD | R = f*(1..1.5) | ok | - | - |
| MIX | 0..1 | ok | - | - |
Missing: SHIFT only upward (no down / bipolar shift); fine-tune.

## 6. REVERB (master, master_fx.c:430-492)
| Knob | Range | Issue | Sev | Fix |
|---|---|---|---|---|
| TYPE | ROOM, HALL, PLATE (comb scale 0.55/1/0.42, damping fixed 0.45/0.3/0.08) | only 4 combs + 2 allpasses per side at 22 kHz -> metallic/fluttery tail, esp. ROOM | med | 2 more allpasses or 8 combs (Freeverb), add slow comb modulation |
| DECAY | fb 0.70..0.97 | ok, stable | - | - |
| PREDELAY | 0..100 ms | tap jumps on change (461) -> glitch into tail | low | ramp tap 1 sample per frame |
| MIX | wet added on top of dry (487) - level rises, it is a send, not crossfade | low | document; or slight dry duck |
| - | 2-sample average decimation + linear upsampling (459, 487-488) -> some aliasing/imaging, audible on PLATE (damp 0.08) | low | 3-tap halfband in/out |
| - | LFO "REV MIX" (lfo.c:12) to 0 -> fx_was_on=0 -> fx_clear wipes the tail + 17 KB memset in ISR + 8-block fade-in, every LFO cycle (646) | HIGH | in fx_smooth decide on/off from prm (user value), not ep (modulated); or clear only after N silent blocks |
16-bit lines (1/16384 step) - noise floor ~-84 dBFS, ok. Missing: DAMP/TONE knob, size separate from decay, freeze.

## 7. EQ (master_fx.c:505-550)
| Knob | Range | Issue | Sev | Fix |
|---|---|---|---|---|
| MID HZ | 250/500/1k/2k/4k, Q 0.9 fixed | coefficients jump instantly (530-535) -> click when changing with big gain | low-med | crossfade old/new for 1 block, or SVF-form EQ with ramped g |
| LOW / MID / HIGH | +-12 dB, shelves 120 Hz / 8 kHz | same coefficient jump while turning; band state zeroed at 64 (538) | low | same |
LOW CUT (493-504): fixed 100 Hz 12 dB/oct, on/off instant (small LF click). OUTPUT step change: boost jumps 3 dB instantly (557).

## 8. OUTPUT limiter (master_fx.c:554-598)
| Item | Issue | Sev | Fix |
|---|---|---|---|
| attack | whole block gets the new lower gain (562-567, int 588-589): gain step at block edge -> click/crunch on every transient that limits, worse at +9/+12 dB | HIGH-med | ramp start->end over first 16-32 frames; soft knee (569) already catches overshoot |
| path switch | float path (lim_gain) and int path (lim_q12) keep separate states; reset on switch (678, 696) -> gain jump when last effect turns off/on while limiting | med-low | one shared gain state |
| boost 0 | no limiter: LADDER comp / reverb wet / EQ +12 dB hard-clip at 698 | med | keep limiter at step 0 too (ceiling only, no boost) |

## 9. Engine DELAY / REVERB (closed; MASTER FX rows DELAY+SENDS, TRACK FX pages)
Ours: UI only (screen.c:2853-2933, 3504-3513, 3515-3560). Engine: web/engine/effects.c:48-89, web/synth/mixer.c:8-14, sound.c:233-284.
| Knob | Range | Issue | Sev | Fix (ours?) |
|---|---|---|---|---|
| DLY TIME / SYNC | 166+14v ticks (30-354 ms), FREE 360-2040 ms, SYNC 1/32T..1/4; ticks = 8 samples | read pointer jumps on time/BPM change, no crossfade (effects.c:74) -> click | med | engine only |
| FEEDBACK | 0..0.8 | stable; only LP is the input LP | ok | - |
| REV TIME (DECAY) | comb fb 0.25..0.85 | reverb = 4 plain combs, no allpass -> metallic; L = combs 1+2, R = 3+4 | med | engine only |
| RETURN | 0..1, shared by delay and reverb (rows DELAY and SENDS show the same knob) | can't balance delay vs reverb | low | UI note |
| REV/DLY SEND (per track) | 0..15 linear (1->2 = +6 dB), mono sum, post-fader | coarse | low | engine only |
| - | both run at 5.5 kHz (every 8th sample) after 2 one-poles ~1.7 kHz -> dark lo-fi, aliasing grit; delay mono (no ping-pong, no stereo) | med (character) | ours: add an own DELAY insert in master_fx (ping-pong, LP/HP in loop, tempo sync from groove tempo); per-track sends impossible from app side |
| - | after 8 s input silence (or 24 repeats for long delays) state is wiped (effects.c:58-62): long tails cut at ~-46 dB | low | - |
Empty knobs: DELAY "-" and SENDS "-" (two knobs free for a future own delay).

## 10. BINAURAL / 3D SOUND B1 (binaural.c:202-287)
| Knob | Range | Issue | Sev | Fix |
|---|---|---|---|---|
| MODE | OFF / XFEED | fades via m (208-211) - good | ok | - |
| WIDTH | side x0..x2 (49) | runs after master limiter: width >50 raises peaks up to 1.5x -> hard clip in limit() (audio_output.c:20) | med | soft-clip or gain 1/(1+0.5*(w-1)) when w>1 |
| FEED | 0..0.35, 700 Hz LP, 0.32 ms delay | level kept by 1/(1+f) - ok; simple vs bs2b (no head-shadow on direct) | low | feature |
| MIX | 0..1 | all params ramped per sample - good | ok | - |
Tones (0.25 FS) + noise (0.44 FS) are added after the limiter onto a mix already at ~0.89 FS -> clipping when music plays loud | med | duck or soft-clip in run_tone. Integer, cheap (~31 x86 cycles/frame), no ISR issues.

## 11. MICRO D ladder / drive (micro_d.c:450-547)
| Knob | Range | Issue | Sev | Fix |
|---|---|---|---|---|
| CUT | 30 Hz-12 kHz + CONTOUR +-5 oct + 50 % key tracking + LFO | ramped per 16-frame sub-block - good | ok | - |
| PEAK (RESO) | k 0..3.7 (no self-osc) | k and comp not ramped (452-454) -> zipper when turning | low-med | ramp k across sub-block |
| DRIVE | 1..4 (x1.2) into tanh at 1x | aliasing at high drive on bright saws | low-med | ok for character; ADAA if needed |
| - | output (int)(s*0.8*2^23) added without clamp (543) | low (clamped later) | - |
Runs from Flash in the ISR (design, guarded by md_hold).

## CPU (per 128-frame block, rough)
Heaviest: LADDER/ACID (5 tanh-div per frame/ch = 1280 div), PHASER 8 st (128 ftan_pi), CHORUS (512 sin + Hermite), RING SHIFT (16 allpass + 2 sin per frame/ch), DRIVE SOFT (512 div). Reverb (half rate) and EQ are cheap. All seven on + MICRO D was never measured on device (notes: peak 52-66 %). Denormals: no FTZ set; EQ/SVF/phaser/DC states decay through subnormals after silence - unknown cost on pi32v2 FPU; check GLOBAL PEAK after a note release with EQ on.

## Fix first (ranked)
1. audio_output.c:29,31 - `__sync_synchronize` in Flash called by the ISR every block (also during Flash writes): use a compiler barrier.
2. master_fx.c:623-639 - fx_clear uses Flash `memset` and clears up to 17 KB in one ISR block: RAM loop, spread over blocks or rely on the fade.
3. master_fx.c:644-646 - decide effect on/off from prm, not LFO-modulated ep: LFO "REV MIX" to 0 wipes the reverb tail each cycle.
4. master_fx.c:562-567, 588-589 - limiter attack is a gain step at block edge: ramp over 16-32 frames.
5. master_fx.c:299,310 - smooth CHORUS DEPTH (delay jumps = clicks, worst with LFO).
6. master_fx.c:181,261,208 + mix at 688-693 - ramp RESO k, DRIVE gain, comb fb and every MIX across the block (pattern of 233-240).
7. master_fx.c:182,192 + 696 - cap LADDER comp / keep limiter at OUTPUT step 0 (hard clip at 698).
8. master_fx.c:318 - CHORUS/FLANGE MIX 100 % = vibrato; make 100 % = 50/50.
9. binaural.c:219-226, 265-266 - WIDTH>50 and tones/noise after the limiter can hard-clip: soft-clip or headroom.
10. Engine delay/reverb are 5.5 kHz mono-delay lo-fi and click on time change; consider an own master DELAY (ping-pong, filter, sync) on the free DELAY/SENDS knobs.
