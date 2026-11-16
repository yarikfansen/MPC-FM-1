/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_KEYSCALE_H
#define FM1_KEYSCALE_H
#include <stdint.h>
/* Key (tonality) for the soft key glow, and chord names for the ARP and
 * SEQ STEP chord plates (user 09.11, mockup round 4). Control task only.
 * GLOBAL KEY: 0 = AUTO (default) or 1..12 = C..B; MODE: 0 MAJ, 1 MIN.
 * AUTO: the first chord played after power-on or a project load (ARP: the
 * first held chord; SEQ STEP: the track's first chord) fixes the key until
 * KEY is chosen. A single note or an unknown chord does not fix it. */
unsigned key_get(void);
unsigned key_mode_get(void);
void key_edit(unsigned param, int delta); /* 0 KEY, 1 MODE (MODE only when KEY is chosen) */
void key_auto_reset(void);                /* power-on, project load */
void key_chord_seen(const uint8_t *notes, unsigned count);
/* 12-bit pitch-class mask of the scale (bit 0 = C); 0 while AUTO has no chord yet. */
unsigned key_scale_mask(void);
/* MUTATE in key (SEQ rework C3, SEQ and ARP): a note whose pitch class is not in `mask` moves to the nearest scale
 * note, up first (a tie goes up; never past 0..127). Mask 0 (AUTO, no chord yet): the note stays. */
static inline int key_snap(int note, unsigned mask)
{
    if (!(mask & 0xfffU) || note < 0 || note > 127) return note;
    for (int d = 0; d < 12; ++d) {
        if (note + d <= 127 && (mask >> ((note + d) % 12) & 1U)) return note + d;
        if (note - d >= 0 && (mask >> ((note - d) % 12) & 1U)) return note - d;
    }
    return note;
}
/* Stored byte (extras record): KEY in bits 0-3, MODE in bit 4; 0xff = defaults. */
unsigned key_state_get(void);
void key_state_put(unsigned stored);
/* Name of the chord of up to 8 MIDI notes, root first ("D", "Gm7", "C#maj7").
 * One pitch class: the lowest note with its octave ("C3"); no notes: "".
 * Unknown chord: "--". Returns 1 for a known chord (root 0..11; minor: 0 major
 * third, 1 minor third, 2 no third, e.g. C5 or Csus4), else 0. */
unsigned chord_name(const uint8_t *notes, unsigned count, char out[8], unsigned *root, unsigned *minor);
#endif
