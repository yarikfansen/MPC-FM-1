/* Host check for app/voice_rules.h: which tracks the control task switches
 * to MONO (303 = MONO always, user 10.11). Exit 0 = every check passed;
 * otherwise the number of the failing case. */
#include "voice_rules.h"

int main(void)
{
    /* 1: a POLY 303 is switched, a MONO 303 is not; other engines never
     * (DX7 keeps its own MONO / POLY choice) */
    uint8_t type[GROOVE_TRACKS] = {GROOVE_303, GROOVE_303, GROOVE_DX7, GROOVE_DX7, GROOVE_303, GROOVE_808, GROOVE_303, GROOVE_PSG};
    uint8_t mono[GROOVE_TRACKS] = {0, 1, 0, 1, 0, 0, 1, 0};
    if (voice_303_poly_mask(type, mono, GROOVE_TRACKS) != ((1U << 0) | (1U << 4))) return 1;
    /* 2: once switched, nothing is left to do (no call every frame) */
    mono[0] = mono[4] = 1;
    if (voice_303_poly_mask(type, mono, GROOVE_TRACKS)) return 2;
    /* 3: an engine change to 303 (controls zeroed = POLY) on the last track */
    type[7] = GROOVE_303; mono[7] = 0;
    if (voice_303_poly_mask(type, mono, GROOVE_TRACKS) != (1U << 7)) return 3;
    /* 4: only the given tracks are read */
    if (voice_303_poly_mask(type, mono, 7)) return 4;
    return 0;
}
