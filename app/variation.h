/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_VARIATION_H
#define FM1_VARIATION_H
#include <stdint.h>
/* SEQ variations per track (v7, agreed screen 21b). Control task only.
 * MUTATE 0..100 %: each loop some notes jump an octave or a fifth (drums: a
 *   hit moves to the next empty step).
 * HUMAN 0..100 %: each loop the velocities wander (up to +-40 at 100 %).
 * RATCHET: gone in C2 (the split is per step, RAM table below, played by var_tick on every pass). Its saved byte
 *   now holds only FOLLOW (4 = ON, page 3 K3); an old amount 1..3 loads as 0. var_edit refuses this parameter.
 * CHANCE (C3): 0 = OFF, 1..12 = rhythm patterns (seq_steps.h, HOUSE 1 .. DNB 3): the pattern's '.' steps are muted
 *   in the per-loop copy (not written into the pattern; their ratchet split does not fire). Saved as 0x80 | n;
 *   an old percent level 1..7 loads as OFF. MUTATE jumps stay in the key (keyscale.h; AUTO without a chord: octaves).
 * MUTATE, HUMAN and CHANCE rewrite the playing track once per loop from a kept
 * original; edits made meanwhile go into the original. The original returns
 * when all three are 0, when playback stops and before every save. */
enum { VAR_MUTATE, VAR_HUMAN, VAR_RATCHET, VAR_CHANCE, VAR_PARAMS };
unsigned var_get(unsigned track, unsigned param);
void var_edit(unsigned track, unsigned param, int delta);
void var_value(unsigned track, unsigned param, char out[12]);
void var_restore_all(void); /* put the originals back (before a save) */
void var_forget(void);      /* a new pattern was loaded: originals are void */
void var_restore(unsigned track); /* this track's original back before a read / rewrite (C2 step edits) */
unsigned var_follow(unsigned track);           /* FOLLOW (C2): saved with the variations */
void var_follow_set(unsigned track, unsigned on);
/* Ratchet split per step (C2): RAM only, cleared by var_forget (project load) and var_split_clear (engine change). */
unsigned var_split_get(unsigned track, unsigned step);
void var_split_set(unsigned track, unsigned step, unsigned split);
void var_split_fill(unsigned track, uint64_t was, uint64_t now, unsigned split);
void var_split_clear(unsigned track);
typedef int (*var_post_fn)(uint8_t status, uint8_t a, uint8_t b);
void var_tick(uint32_t now, var_post_fn post);
void var_state_get(uint8_t out[32]);
void var_state_put(const uint8_t in[32]);
#endif
