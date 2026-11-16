/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_SEQ_STEPS_H
#define FM1_SEQ_STEPS_H
/* SEQ rework C2 step logic (mockup docs/mockups/FM1-seq-rework.html a, b, c): pure functions over one track's
 * cells (struct groove_step[64], as groove_sequence_read gives them). screen.c reads the track, calls these, sets one
 * undo checkpoint and writes the cells back with groove_sequence_replace. Host test: tests/t_seq_steps.c.
 * Cell bits (instrument.h): 0-4 chord, 5-8 gate, 9-11 probability, 12 tie, 13 alt; 14-15 are NEVER written: the engine's
 * project check refuses them (open copy web/engine/sequencer.c:82), so the ratchet split lives in a RAM table. */
#include <stdint.h>
#include "instrument.h"
#include "keyscale.h"

enum { SEQ_CELLS = 64, SEQ_TIE = 1U << 12, SEQ_SPLIT_SHIFT = 14, SEQ_SPLIT_MASK = 3U << 14 };
/* C3: a long note may last the whole sequence: up to 63 tail steps (still stops before the next lit step / its head) */
enum { SEQ_LEN_FULL = 14, SEQ_LEN_MAX_TAILS = SEQ_CELLS - 1 };

/* MUTATE in key (C3): the jump `pick` (0..3 = +12, -12, +7, -5) from `note`, kept in 0..127, then moved into the
 * scale `mask` (keyscale.h key_snap). Mask 0 (KEY AUTO, no chord yet, his decision 12.11): octave jumps only
 * (pick 0 / 2 = +12, 1 / 3 = -12). */
static inline int seq_mutate_note(int note, unsigned pick, unsigned mask)
{
    static const int8_t jump[4] = {12, -12, 7, -5};
    int j = (mask & 0xfffU) ? jump[pick & 3U] : (pick & 1U) ? -12 : 12, n = note + j;
    n = n < 0 ? 0 : n > 127 ? 127 : n;
    return key_snap(n, mask);
}

/* CHANCE rhythm patterns (C3, page 2 K4; drafts approved 12.11): 0 = OFF (every step plays), 1..12 = HOUSE 1-3,
 * TECHNO 1-3, TRANCE 1-3, DNB 1-3. A 16-step mask ('x' = the step may play), repeated over the sequence length.
 * Non-destructive: variation.c mutes the steps in its per-loop copy; the stored pattern is untouched. */
enum { SEQ_CHANCE_PATTERNS = 12 };
extern const char seq_chance_masks[SEQ_CHANCE_PATTERNS][17];
static inline unsigned seq_chance_plays(unsigned pattern, unsigned s)
{
    return !pattern || pattern > SEQ_CHANCE_PATTERNS || seq_chance_masks[pattern - 1U][s % 16U] == 'x';
}
/* Name: long "HOUSE 1" (page 2 window), short "HS1" (knob value, mockup c); OFF for 0. out: 12 bytes. */
void seq_chance_name(unsigned pattern, unsigned short_form, char out[12]);
/* Steps of `c` (len cells) that the pattern mutes, as a bit mask. A lit step whose bit is '.' is muted; on a melodic
 * track (pads = 0) the mask counts note starts: a long note's tail steps (tied in from the step before, same note)
 * follow their head - muted with a muted head, played with a playing one. */
uint64_t seq_chance_muted(const struct groove_step *c, unsigned len, unsigned pattern, unsigned pads);
/* Saved CHANCE byte (variation record, no format change): pattern n is stored as SEQ_CHANCE_STORED | n, so the old
 * percent levels 1..7 (no flag) load as OFF and old firmware (limit 7) reads a new value as its 0 (= always). */
enum { SEQ_CHANCE_STORED = 0x80 };
static inline unsigned seq_chance_load(unsigned stored)
{
    unsigned n = stored & 0x7fU;
    return (stored & SEQ_CHANCE_STORED) && n >= 1 && n <= SEQ_CHANCE_PATTERNS ? n : 0U;
}
static inline unsigned seq_chance_store(unsigned pattern) { return pattern && pattern <= SEQ_CHANCE_PATTERNS ? SEQ_CHANCE_STORED | pattern : 0U; }

/* Live REC long note (screen.c long_note_end): steps the held note covers, head included, from its length in 1/16
 * steps: rounded up, at most the sequence length (the tail never reaches its own head). */
static inline unsigned seq_rec_steps(unsigned len16, unsigned length)
{
    unsigned n = (len16 + 15U) / 16U;
    if (length > SEQ_CELLS) length = SEQ_CELLS;
    return n > length ? length : n;
}
/* Fill copies fade like a delay (user 12.11): each tap is SEQ_FADE_NUM / SEQ_FADE_DEN of the tap before, never below
 * SEQ_FADE_FLOOR (a copy stays a lit step). */
enum { SEQ_FADE_NUM = 3, SEQ_FADE_DEN = 4, SEQ_FADE_FLOOR = 8 };
static inline unsigned seq_fade(unsigned velocity)
{
    unsigned v = velocity * SEQ_FADE_NUM / SEQ_FADE_DEN;
    return v < SEQ_FADE_FLOOR ? SEQ_FADE_FLOOR : v;
} /* LENGTH position 14 = one full step; 14 + k = k tail steps */

/* Ratchet split of a step (page 1 K2): 0 OFF, 1 2X, 2 3X, 3 4X. RAM only (coordinator decision A, 12.11): one row of
 * 16 bytes per track, 2 bits per step; variation.c keeps the rows behind var_split_get / var_split_set. */
enum { SEQ_SPLIT_ROW = SEQ_CELLS / 4 };
static inline unsigned seq_split_at(const uint8_t row[SEQ_SPLIT_ROW], unsigned s)
{
    return s < SEQ_CELLS ? (row[s >> 2] >> ((s & 3U) * 2U)) & 3U : 0U;
}
static inline void seq_split_put(uint8_t row[SEQ_SPLIT_ROW], unsigned s, unsigned split)
{
    if (s >= SEQ_CELLS) return;
    unsigned at = (s & 3U) * 2U;
    row[s >> 2] = (uint8_t)((row[s >> 2] & ~(3U << at)) | ((split & 3U) << at));
}
/* A fill moved its copies from `was` to `now` (seq_fill masks): the copies carry the cursor step's split, the steps
 * no longer copied lose theirs. */
static inline void seq_split_fill(uint8_t row[SEQ_SPLIT_ROW], uint64_t was, uint64_t now, unsigned split)
{
    for (unsigned s = 0; s < SEQ_CELLS; ++s) {
        if (now >> s & 1U) seq_split_put(row, s, split);
        else if (was >> s & 1U) seq_split_put(row, s, 0);
    }
}
/* A step that plays: melodic = velocity set; drums (pad >= 0) = that pad's bit. */
static inline unsigned seq_lit(const struct groove_step *c, int pad)
{
    return pad < 0 ? c->velocity != 0 : (c->drums >> (unsigned)pad) & 1U;
}

/* LENGTH (grid K2, melodic tracks). Positions: 0..13 = gate 1/16..14/16, 14 = one full step (gate 15), 14 + k =
 * the note tied over k tail steps (k = 1..15): the step gets full gate + tie, each tail step the same note, velocity,
 * chord and alt bit with full gate, tied on except the last (the shape the live REC long note writes).
 * seq_len_tails: tail steps of the long note starting at `s` (0 when it is not tied; a bare tie counts 0).
 * seq_len_pos: the knob position of step `s` (a bare tie reads as 15). */
unsigned seq_len_tails(const struct groove_step *c, unsigned len, unsigned s);
unsigned seq_len_pos(const struct groove_step *c, unsigned len, unsigned s);
/* Sets step `s` to position `pos`. A tail never overwrites: it stops before the next lit step (or before the step
 * itself after a wrap; tails wrap at the sequence end as REC does). Shortening clears the removed tail steps
 * (velocity 0, tie off, the rest of the cell kept, as break_long_note does). The step's own bits other than gate and
 * tie (chord, probability, alt, split) are kept. Returns the position reached (pos, or less when the tail is
 * blocked); returns the old position when `s` is empty or out of range. */
unsigned seq_len_set(struct groove_step *c, unsigned len, unsigned s, unsigned pos);

/* Ratchet fill (page 1 K3 SPACE, K4 REPEAT). From `snap` (the track before the fill) writes into `out` the track
 * with up to `repeat` copies of step `cur`, at cur + space * j (j = 1..repeat), wrapping past `len` to the start.
 * Melodic (pad < 0): the copy is the whole cell (note, velocity, chord, gate, probability, alt) without the tie (and
 * never bits 14-15); a copy landing on a lit step is skipped. Drums (pad = the cursor's pad): the copy sets that pad's
 * bit; a step where the pad is already set is skipped; an empty drum cell also takes the cursor cell's note
 * (probability) and velocity. The split goes with the copies through seq_split_fill.
 * Fade: tap j has the velocity of tap j-1 faded once (seq_fade), counted per tap like a delay, so a skipped tap still
 * counts; the source step keeps its velocity. Drums: the fade lands only in an empty cell (a cell holding other pads
 * keeps its velocity, shared by its pads); a drum cursor with velocity 0 (= the engine's 100) gives equal copies. Earlier copies count as lit for later ones. Returns the steps written as a bit mask
 * (0 when the cursor step is not lit, space is 0 or len is 0). */
uint64_t seq_fill(const struct groove_step *snap, struct groove_step *out, unsigned len, unsigned cur,
                  unsigned space, unsigned repeat, int pad);

/* TRANSPOSE (page 2 K3): moves every lit melodic cell of `c` (all `count` cells) by `semis`. All or nothing: when any
 * note or chord top (root + the chord's last offset from `o`) would leave 0..127, nothing changes and -1 returns. */
int seq_transpose(struct groove_step *c, unsigned count, int semis, const struct groove_sequence_options *o);

/* FOLLOW (page 3 K3): the shift for a held key, relative to C, in the nearest direction (-5..+6; F# goes up). */
static inline int seq_follow_shift(unsigned note) { int pc = (int)(note % 12U); return pc <= 6 ? pc : pc - 12; }
#endif
