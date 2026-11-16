/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_VOICE_RULES_H
#define FM1_VOICE_RULES_H
#include "instrument.h"
/* 303 = MONO always (user 10.11, stable sound): its 4 POLY voices cost more
 * than one core's whole budget in the closed engine, which then cuts a voice
 * at every chord. A 303 track starts POLY after an engine change (the engine
 * zeroes its controls) and a project or an undo may bring POLY back, so the
 * control task checks every frame. type[t] = groove_source_type(t), mono[t]
 * = the track's MONO flag. Returns the tracks (bit t) to switch to MONO. */
static inline unsigned voice_303_poly_mask(const uint8_t *type, const uint8_t *mono, unsigned tracks)
{
    unsigned mask = 0;
    for (unsigned t = 0; t < tracks && t < 32U; ++t)
        if (type[t] == GROOVE_303 && !mono[t]) mask |= 1U << t;
    return mask;
}
#endif
