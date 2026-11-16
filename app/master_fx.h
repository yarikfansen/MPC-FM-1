/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_MASTER_FX_H
#define FM1_MASTER_FX_H
#include "fm1.h"
/* Insert effects on the final stereo mix (all tracks together), after the
 * engine's own reverb/delay sends. Each effect has four 0..127 controls:
 * 0 = type / variant, 1 and 2 = effect controls, 3 = mix. Mix 0 bypasses. */
enum { MFX_FILTER, MFX_DRIVE, MFX_CHORUS, MFX_PHASER, MFX_RING, MFX_VERB, MFX_EQ, MFX_COUNT };
/* EQ is different: control 0 = mid frequency, 1..3 = LOW / MID / HIGH gain
 * (64 = 0 dB, +-12 dB); it is active when any gain is off centre. */
unsigned mfx_active(unsigned fx);
/* Output boost after all effects, 0..4 = +0/+3/+6/+9/+12 dB, with a peak
 * limiter so the louder signal does not clip. */
void mfx_output_set(unsigned step);
unsigned mfx_output(void);
void mfx_reset(void);
void mfx_edit(unsigned fx, unsigned param, int delta); /* control task */
unsigned mfx_value(unsigned fx, unsigned param);
const char *mfx_name(unsigned fx);
const char *mfx_label(unsigned fx, unsigned param);
const char *mfx_type_name(unsigned fx);
/* Filter cutoff in Hz as the DSP uses it. DJ: >0 low-pass, <0 high-pass, 0 open. */
int mfx_cutoff_hz(void);
/* 100 Hz low cut on the whole mix. */
void mfx_lowcut_set(unsigned on);
/* LFO offsets added to controls 1..3 inside the DSP (-127..127). */
void mfx_mod_set(unsigned fx, unsigned param, int offset);
unsigned mfx_lowcut(void);
/* COMP on the whole mix: 0 THRESH 0..15 (0..-30 dB), 1 RATIO 0..3 (2/4/8/20:1),
 * 2 SPEED 0..2 (fast/med/slow), 3 MIX 0..127 (0 = off). */
void mfx_comp_edit(unsigned param, int delta);
unsigned mfx_comp_value(unsigned param);
int mfx_comp_reduction_db(void); /* last block's gain reduction, <= 0 */
FM1_AUDIO_CODE void mfx_process(int32_t *buffer); /* FM1_BLOCK_FRAMES stereo Q23 */
/* Audio ISR, after mfx_process: nonzero when that block zeroed effect memory or ran two FILTER instances
 * (sliced clearing), so its time is not the effects' running cost. */
FM1_AUDIO_CODE unsigned mfx_clearing(void);
/* Load guard units (audio_output.c sheds them one by one under load): the
 * seven effects above (bit = MFX_*), then COMP and LOW CUT. The output
 * boost / limiter, the 20 Hz sub filter and the boot chime are not units:
 * they always run. */
enum { MFX_U_COMP = MFX_COUNT, MFX_U_LOWCUT, MFX_UNITS };
/* Audio ISR, before mfx_process: units switched on by the user (MIX, EQ
 * gains with their LFO offsets, COMP MIX, LOW CUT). */
FM1_AUDIO_CODE unsigned mfx_units_on(void);
/* Audio ISR, before mfx_process: `fade` = units to take out (each fades to
 * silence over MFX_SHED_FADE blocks, then stops running and keeps no
 * memory); `cut` (a subset) = stop at once, no fade (only when the block
 * would otherwise be dropped). A unit that leaves `fade` comes back: an
 * effect from clean memory with its normal fade-in, EQ / COMP / LOW CUT
 * fade in over 16 blocks. */
FM1_AUDIO_CODE void mfx_shed(unsigned fade, unsigned cut);
enum { MFX_SHED_FADE = 4 };
/* Blocks (~46 ms) the fixed 20 Hz filter takes to fade out on the integer path after the last unit stopped. */
enum { MFX_SUB_FADE = 16 };
/* Audio ISR, after mfx_process: units that are fully out (silent, not run
 * next block while still in `fade`). */
FM1_AUDIO_CODE unsigned mfx_units_silent(void);
/* Audio ISR, after mfx_process: units whose time in that block is their
 * honest running cost (they ran, no FILTER TYPE crossfade), and that time
 * in the clock given to mfx_clock_set (0 when no clock is set). */
FM1_AUDIO_CODE unsigned mfx_units_timed(void);
FM1_AUDIO_CODE uint32_t mfx_unit_ticks(unsigned unit);
/* Clock for the unit times: lap(&mark) returns the ticks since *mark and
 * sets *mark to now. Set before the audio ISR starts; NULL = no timing. */
void mfx_clock_set(uint32_t (*lap)(uint32_t *mark));
#ifdef FM1_HOST_TEST
/* Host tests: called after each unit runs (to spend test time). */
extern void (*mfx_test_spend)(unsigned unit);
#endif
void mfx_chime_start(void); /* control task: boot chime, plays once */
void mfx_scope_enable(unsigned on);          /* SCOPE page open: copy the output */
void mfx_scope_read(int16_t *out, unsigned n); /* latest n mono samples at 22 kHz, oldest first */
#endif
