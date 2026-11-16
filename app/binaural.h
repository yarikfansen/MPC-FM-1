/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_BINAURAL_H
#define FM1_BINAURAL_H
#include "build_config.h"
#include "fm1.h"
#include <stdint.h>
/* BINAURAL (headphones only), docs/binaural-research.md.
 * A: two sines, left f and right f + beat, with a noise bed (pink / rain),
 *    a session timer and slow fades. Added to the output after rec_capture,
 *    so it neither reaches the sampler recording nor the master FX.
 * B1: crossfeed + width on the instrument mix, before A is added (the
 *    crossfeed would smear the L/R phase difference of the tones).
 * Everything is off (zero cost) until switched on. Control task unless noted. */
enum { BIN_STATE_BYTES = 12 };
enum { BIN_TIMER, BIN_PRESET, BIN_BEAT, BIN_CARRIER, BIN_NOISE, BIN_MODE, BIN_WIDTH, BIN_FEED, BIN_MIX,
       BIN_NLEVEL /* noise level 0..10, same type */, BIN_NTYPE /* <0 pink, >0 rain */, BIN_PARAMS };
enum { BIN_PRESETS = 5 /* DELTA THETA ALPHA BETA FOCUS; 0 = OFF, 6 = CUSTOM */, BIN_CUSTOM = 6 };
#if FM1_BINAURAL
void binaural_edit(unsigned param, int delta);
const char *binaural_label(unsigned param);
void binaural_text(unsigned param, char out[12]);
unsigned binaural_running(void);     /* a tone / noise session is on */
unsigned binaural_remaining(void);   /* seconds left of the timer, 0 = no timer */
unsigned binaural_beat10(void);      /* beat in 0.1 Hz */
unsigned binaural_carrier(void);     /* left carrier in Hz */
unsigned binaural_xfeed(void);       /* crossfeed mode on */
unsigned binaural_feed(void);        /* 0..100 */
unsigned binaural_width(void);       /* 0..100, 50 = as it is */
unsigned binaural_mix(void);         /* 0..100 */
unsigned binaural_noise(void);       /* 0 off, 1..10 pink, 11..20 rain */
unsigned binaural_preset(void);      /* 0 OFF, 1..5, BIN_CUSTOM */
unsigned binaural_timer_min(void);   /* session length in minutes, 0 = no timer */
void binaural_state_get(uint8_t out[BIN_STATE_BYTES]);
void binaural_state_put(const uint8_t in[BIN_STATE_BYTES]); /* 0xff bytes = defaults; never starts a session */
FM1_AUDIO_CODE void binaural_render(int32_t *buffer); /* in place, FM1_BLOCK_FRAMES stereo Q23 */
#else
static inline void binaural_state_get(uint8_t out[BIN_STATE_BYTES]) { for (unsigned i = 0; i < BIN_STATE_BYTES; ++i) out[i] = 0xff; }
static inline void binaural_state_put(const uint8_t in[BIN_STATE_BYTES]) { (void)in; }
static inline void binaural_render(int32_t *buffer) { (void)buffer; }
#endif
#endif
