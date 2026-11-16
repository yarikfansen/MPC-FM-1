/* SEQ rework C1 navigation host check (app/seq_nav.h): SEQ tap, SELECT through the three pages with stops at both
 * ends, the MIDI submenu row, the page indicator, the VARIATION knob mapping onto variation.h and the inactive knobs.
 * Not covered here: drawing, the button handler in screen.c (hold = CHAIN, EDIT toggle), the engine. */
#include "seq_nav.h"
#include "variation.h"

_Static_assert(VAR_MUTATE == 0 && VAR_HUMAN == 1 && VAR_RATCHET == 2 && VAR_CHANCE == 3, "variation.h order");

static int fails;
#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)

static int run(void)
{
    /* SEQ tap: grid -> page 1 STEP; any sub-page (pages 2, 3, the MIDI submenu) -> grid */
    CHECK(seq_nav_tap(SEQ_GRID) == SEQ_STEP);
    CHECK(seq_nav_tap(SEQ_STEP) == SEQ_GRID);
    CHECK(seq_nav_tap(SEQ_RHYTHM) == SEQ_GRID);
    CHECK(seq_nav_tap(SEQ_ROLE) == SEQ_GRID);
    CHECK(seq_nav_tap(SEQ_MIDI_OUT) == SEQ_GRID);
    /* SELECT: 1 -> 2 -> 3 (TRACK = 7), right stops on TRACK, left stops on STEP; big deltas move one page */
    CHECK(seq_nav_select(SEQ_STEP, 1) == SEQ_RHYTHM);
    CHECK(seq_nav_select(SEQ_RHYTHM, 1) == SEQ_ROLE);
    CHECK(seq_nav_select(SEQ_ROLE, 1) == SEQ_ROLE);
    CHECK(seq_nav_select(SEQ_ROLE, -1) == SEQ_RHYTHM);
    CHECK(seq_nav_select(SEQ_RHYTHM, -3) == SEQ_STEP);
    CHECK(seq_nav_select(SEQ_STEP, -1) == SEQ_STEP);
    CHECK(seq_nav_select(SEQ_STEP, 5) == SEQ_RHYTHM);
    CHECK(seq_nav_select(SEQ_STEP, 0) == SEQ_STEP);
    /* never the grid, never the old VARS page, never page 3 (unused id) */
    for (unsigned p = 1; p < 8; ++p)
        for (int d = -2; d <= 2; ++d) {
            unsigned n = seq_nav_select(p, d);
            if (p == 3) { CHECK(n == 3); continue; } /* unknown page: stays */
            CHECK(n != SEQ_GRID && n != 3);
            CHECK((p >= SEQ_MIDI_CLOCK && p <= SEQ_MIDI_SYNC) == (n >= SEQ_MIDI_CLOCK && n <= SEQ_MIDI_SYNC));
        }
    /* MIDI submenu keeps its own row */
    CHECK(seq_nav_select(SEQ_MIDI_CLOCK, 1) == SEQ_MIDI_OUT);
    CHECK(seq_nav_select(SEQ_MIDI_SYNC, 1) == SEQ_MIDI_SYNC);
    CHECK(seq_nav_select(SEQ_MIDI_CLOCK, -1) == SEQ_MIDI_CLOCK);
    /* the three-page indicator */
    CHECK(seq_nav_icon(SEQ_STEP) == 0 && seq_nav_icon(SEQ_RHYTHM) == 1 && seq_nav_icon(SEQ_ROLE) == 2);
    CHECK(seq_nav_icon(SEQ_MIDI_CLOCK) == 0 && seq_nav_icon(SEQ_MIDI_SYNC) == 2);
    /* page 2 knobs: K1 MUTATE, K2 HUMAN, K3 none (TRANSPOSE, C2), K4 CHANCE; RATCHET has no knob */
    CHECK(seq_nav_var_knob(0) == VAR_MUTATE);
    CHECK(seq_nav_var_knob(1) == VAR_HUMAN);
    CHECK(seq_nav_var_knob(2) == SEQ_KNOB_NONE);
    CHECK(seq_nav_var_knob(3) == VAR_CHANCE);
    CHECK(seq_nav_var_knob(4) == SEQ_KNOB_NONE);
    for (unsigned k = 0; k < 8; ++k) CHECK(seq_nav_var_knob(k) != VAR_RATCHET);
    /* C2: the C1 placeholder table (seq_nav_inactive) is gone; the knobs work (tests/t_seq_steps.c) */
    return 0;
}

int main(void)
{
    fails = run();
    return fails;
}
