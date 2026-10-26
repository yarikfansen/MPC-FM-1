/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_VARIATION_H
#define FM1_VARIATION_H
#include <stdint.h>
/* SEQ variations per track (v7, agreed screen 21b). Control task only.
 * MUTATE 0..100 %: each loop some notes jump an octave or a fifth (drums: a
 *   hit moves to the next empty step).
 * HUMAN 0..100 %: each loop the velocities wander (up to +-40 at 100 %).
 * RATCHET 0..3 (OFF, 2X, 3X, 4X): about one note in three repeats inside its step.
 * CHANCE 0..7: the engine's own step probability for every note of the track
 *   (0 = always, n = n/8), written into the pattern.
 * MUTATE and HUMAN rewrite the playing track once per loop from a kept
 * original; edits made meanwhile go into the original. The original returns
 * when both are 0, when playback stops and before every save. */
enum { VAR_MUTATE, VAR_HUMAN, VAR_RATCHET, VAR_CHANCE, VAR_PARAMS };
unsigned var_get(unsigned track, unsigned param);
void var_edit(unsigned track, unsigned param, int delta);
void var_value(unsigned track, unsigned param, char out[12]);
void var_restore_all(void); /* put the originals back (before a save) */
void var_forget(void);      /* a new pattern was loaded: originals are void */
typedef int (*var_post_fn)(uint8_t status, uint8_t a, uint8_t b);
void var_tick(uint32_t now, var_post_fn post);
void var_state_get(uint8_t out[32]);
void var_state_put(const uint8_t in[32]);
#endif
