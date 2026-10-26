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
FM1_AUDIO_CODE void mfx_process(int32_t *buffer); /* FM1_BLOCK_FRAMES stereo Q23 */
void mfx_scope_enable(unsigned on);          /* SCOPE page open: copy the output */
void mfx_scope_read(int16_t *out, unsigned n); /* latest n mono samples at 22 kHz, oldest first */
#endif
