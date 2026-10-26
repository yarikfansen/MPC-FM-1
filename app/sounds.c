/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "sounds.h"
#include "instrument.h"
#include "sound_lib.h"
#include <string.h>

_Static_assert(sizeof lib_dx7 / sizeof lib_dx7[0] <= 40, "library size");
enum { TRACKS = 8, DX7_N = sizeof lib_dx7 / sizeof lib_dx7[0], TONE_N = sizeof lib_tone / sizeof lib_tone[0],
       KIT_N = sizeof lib_kit / sizeof lib_kit[0], KIT_PARTS = 12 };
/* Per track: engine * 40 + index + 1; 0 = none. */
static uint8_t current[TRACKS];

/* The index-th sound of an engine; an engine's sounds need not be adjacent
 * in lib_tone (new ones are appended at the end). */
static const struct lib_tone *tone_at(unsigned engine, unsigned index)
{
    for (unsigned i = 0; i < TONE_N; ++i)
        if (lib_tone[i].source == engine && !index--) return &lib_tone[i];
    return &lib_tone[0];
}
unsigned sounds_count(unsigned engine)
{
    if (engine == GROOVE_DX7) return DX7_N;
    if (engine == GROOVE_808) return KIT_N;
    unsigned n = 0;
    for (unsigned i = 0; i < TONE_N; ++i) n += lib_tone[i].source == engine;
    return n;
}
static void set_part(unsigned track, unsigned part, const uint8_t *value)
{
    uint8_t cur[GROOVE_TONE_PARAMS];
    unsigned count = groove_tone_count(track, part);
    if (count > GROOVE_TONE_PARAMS || groove_tone_copy(track, part, cur)) return;
    for (unsigned p = 0; p < count; ++p) {
        const fm1_param_t *info = groove_tone_parameter(track, part, p);
        unsigned want = value && value[p] != 255 ? value[p] : info ? info->def : cur[p];
        if (want != cur[p]) groove_tone_edit(track, part, p, (int)want - (int)cur[p]);
    }
}
int sounds_apply(unsigned track, unsigned index)
{
    if (track >= TRACKS) return -1;
    unsigned engine = groove_source_type(track), n = sounds_count(engine);
    if (!n) return -1;
    index %= n;
    if (engine == GROOVE_DX7) {
        if (groove_fm_import(track, lib_dx7[index])) return -1;
    } else if (engine == GROOVE_808) {
        const struct lib_kit *k = &lib_kit[index];
        for (unsigned part = 0; part < KIT_PARTS; ++part) {
            uint8_t v[GROOVE_TONE_PARAMS]; memset(v, 255, sizeof v);
            for (unsigned i = 0; i < k->count; ++i) if (k->set[i][0] == part) v[k->set[i][1] % GROOVE_TONE_PARAMS] = k->set[i][2];
            set_part(track, part, v);
        }
    } else {
        const struct lib_tone *t = tone_at(engine, index);
        set_part(track, 0, t->value);
        uint8_t lf[4]; groove_lfo_copy(track, lf);
        for (unsigned i = 0; i < 4; ++i) if (t->lfo[i] != lf[i]) groove_lfo_edit(track, i, (int)t->lfo[i] - (int)lf[i]);
    }
    current[track] = (uint8_t)(engine * 40U + index + 1U);
    return 0;
}
static int index_of(unsigned track)
{
    unsigned c = current[track], engine = groove_source_type(track);
    return c && (c - 1U) / 40U == engine ? (int)((c - 1U) % 40U) : -1;
}
/* Turning through the list only moves a pending choice (its name shows at
 * once); sounds_tick() loads it once the knob rests, so a fast turn does not
 * load every sound on the way (DX7 voices are heavy) and playing stays smooth. */
enum { SETTLE_MS = 150 };
static int pend_track = -1;
static unsigned pend_index, pend_engine;
static uint32_t pend_at;
int sounds_step(unsigned track, int delta, uint32_t now)
{
    if (track >= TRACKS || !delta) return -1;
    unsigned engine = groove_source_type(track), n = sounds_count(engine);
    if (!n) return -1;
    if (pend_track >= 0 && (unsigned)pend_track != track) sounds_tick(now + SETTLE_MS); /* another track: load it now */
    int i = pend_track == (int)track ? (int)pend_index : index_of(track);
    i = i < 0 ? (delta > 0 ? 0 : (int)n - 1) : i + delta;
    i %= (int)n; if (i < 0) i += (int)n;
    pend_track = (int)track; pend_index = (unsigned)i; pend_engine = engine; pend_at = now;
    return 0;
}
int sounds_tick(uint32_t now)
{
    if (pend_track < 0 || (int32_t)(now - pend_at) < SETTLE_MS) return 0;
    unsigned t = (unsigned)pend_track;
    pend_track = -1;
    if (groove_source_type(t) != pend_engine) return 0; /* the engine changed meanwhile */
    return sounds_apply(t, pend_index) == 0;
}
const char *sounds_name(unsigned track)
{
    if (track >= TRACKS) return 0;
    if (pend_track == (int)track && groove_source_type(track) == pend_engine) {
        unsigned e = pend_engine, k = pend_index;
        return e == GROOVE_DX7 ? lib_dx7_name[k] : e == GROOVE_808 ? lib_kit[k].name : tone_at(e, k)->name;
    }
    int i = index_of(track);
    if (i < 0) return 0;
    unsigned engine = groove_source_type(track);
    return engine == GROOVE_DX7 ? lib_dx7_name[i] : engine == GROOVE_808 ? lib_kit[i].name : tone_at(engine, (unsigned)i)->name;
}
void sounds_forget(unsigned track) { if (track < TRACKS) { current[track] = 0; if (pend_track == (int)track) pend_track = -1; } }
void sounds_state_get(uint8_t out[8]) { memcpy(out, current, TRACKS); }
void sounds_state_put(const uint8_t in[8])
{
    for (unsigned t = 0; t < TRACKS; ++t) current[t] = in[t] == 0xff ? 0 : in[t];
}
unsigned sounds_id(unsigned track)
{
    if (track < TRACKS && pend_track == (int)track) return 0x100U | (pend_engine * 40U + pend_index + 1U);
    return track < TRACKS && index_of(track) >= 0 ? current[track] : 0;
}
