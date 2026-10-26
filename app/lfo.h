/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_LFO_H
#define FM1_LFO_H
#include <stdint.h>
/* Two global LFOs (v7: PRESETS switches LFO 1 / LFO 2 on the page). Targets: master effects (smooth, inside the DSP) or one
 * track parameter through MIDI CC (stepped at the control rate, ~100 Hz). */
enum { LFO_COUNT = 2, LFO_WAVES = 6, LFO_RATES = 78 };
struct lfo_cc { uint8_t channel, cc, value; };
void lfo_init(void);
unsigned lfo_target_count(void);
const char *lfo_target_name(unsigned target, char out[12]);
const char *lfo_wave_name(unsigned wave);
void lfo_rate_name(unsigned rate, char out[12]);
unsigned lfo_get(unsigned lfo, unsigned param);       /* 0 target, 1 rate, 2 depth (0..126, 63 = 0), 3 wave */
void lfo_edit(unsigned lfo, unsigned param, int delta);
float lfo_shape(unsigned wave, float phase);          /* -1..1, for drawing */
float lfo_phase(unsigned lfo);
float lfo_output(unsigned lfo);                       /* last value -1..1 */
/* Advance by dt_ms. tempo in BPM; playing restarts synced phases on a new start.
 * Writes up to max CC messages that changed (track targets) and returns how many. */
unsigned lfo_tick(unsigned dt_ms, unsigned tempo, unsigned playing, unsigned allow_cc,
                  struct lfo_cc *out, unsigned max);
#endif
