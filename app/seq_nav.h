/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_SEQ_NAV_H
#define FM1_SEQ_NAV_H
/* SEQ pages after the rework C1 (mockup docs/mockups/FM1-seq-rework.html b-e), kept apart from screen.c so a host
 * test can check them (tests/t_seq_nav.c). seq_page: 0 = the grid, 1 STEP, 2 VARIATION, 7 TRACK; 4..6 = the GLOBAL
 * MIDI submenu (CLOCK / MIDI OUT / MIDI SYNC). The numbers stay: web shortcuts and the snapshot use them. */
enum { SEQ_GRID = 0, SEQ_STEP = 1, SEQ_RHYTHM = 2, SEQ_MIDI_CLOCK = 4, SEQ_MIDI_OUT = 5, SEQ_MIDI_SYNC = 6, SEQ_ROLE = 7 };
enum { SEQ_KNOB_NONE = 255 };

/* SELECT on a SEQ sub-page: STEP -> VARIATION -> TRACK (or CLOCK -> MIDI OUT -> MIDI SYNC); both ends stop. The grid and
 * unknown pages stay where they are (the grid uses SELECT for the step). */
static inline unsigned seq_nav_select(unsigned seq_page, int delta)
{
    static const unsigned char row_seq[3] = {SEQ_STEP, SEQ_RHYTHM, SEQ_ROLE}, row_midi[3] = {SEQ_MIDI_CLOCK, SEQ_MIDI_OUT, SEQ_MIDI_SYNC};
    const unsigned char *row = seq_page >= SEQ_MIDI_CLOCK && seq_page <= SEQ_MIDI_SYNC ? row_midi : row_seq;
    int at = -1;
    for (int i = 0; i < 3; ++i) if (row[i] == seq_page) at = i;
    if (at < 0 || !delta) return seq_page;
    at += delta > 0 ? 1 : -1;
    return row[at < 0 ? 0 : at > 2 ? 2 : at];
}
/* The plate lit in the three-page indicator (icon_row): 0 STEP, 1 VARIATION, 2 TRACK (MIDI: CLOCK, OUT, SYNC). */
static inline unsigned seq_nav_icon(unsigned seq_page)
{
    if (seq_page >= SEQ_MIDI_CLOCK && seq_page <= SEQ_MIDI_SYNC) return seq_page - SEQ_MIDI_CLOCK;
    return seq_page == SEQ_STEP ? 0U : seq_page == SEQ_RHYTHM ? 1U : 2U;
}
/* SEQ pressed (on release, a hold is CHAIN): the grid opens page 1 STEP; any SEQ sub-page goes back to the grid. */
static inline unsigned seq_nav_tap(unsigned seq_page) { return seq_page ? SEQ_GRID : SEQ_STEP; }
/* Page 2 VARIATION knobs K1..K4 -> variation.h parameter (VAR_MUTATE 0, VAR_HUMAN 1, VAR_CHANCE 3); K3 TRANSPOSE is not a
 * variation (screen.c transpose_turn, seq_steps.h). The per-track RATCHET (2) has no knob any more. */
static inline unsigned seq_nav_var_knob(unsigned knob)
{
    static const unsigned char map[4] = {0, 1, SEQ_KNOB_NONE, 3};
    return knob < 4 ? map[knob] : SEQ_KNOB_NONE;
}
/* C2: the C1 placeholders (page 1 K2-K4, page 2 K3, page 3 K3) all work now; which tracks show "--" is decided per
 * track in screen.c (seq_steps.h holds the step logic). */
#endif
