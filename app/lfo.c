/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Two global LFOs for the control task. Master-effect targets modulate the
 * DSP smoothly through offsets; track targets send MIDI CC around a base value
 * captured when the target is chosen, and restore that base when released. */
#include "lfo.h"
#include "master_fx.h"
#include "instrument.h"
#include <string.h>

enum { MASTER_TARGETS = 7, TRACK_PARAMS = 6, TARGETS = 1 + MASTER_TARGETS + GROOVE_TRACKS * TRACK_PARAMS };
static const uint8_t master_map[MASTER_TARGETS][2] = {
    {MFX_FILTER, 1}, {MFX_FILTER, 2}, {MFX_DRIVE, 1}, {MFX_CHORUS, 2}, {MFX_PHASER, 2}, {MFX_RING, 1}, {MFX_VERB, 3}};
static const char *const master_names[MASTER_TARGETS] = {"FLT CUT", "FLT RES", "DRIVE", "CHO DEPTH", "PHA FDBK", "RING FREQ", "REV MIX"};
static const uint8_t track_cc[TRACK_PARAMS] = {11, 10, 74, 71, 1, 91};
static const char *const track_names[TRACK_PARAMS] = {"VOL", "PAN", "CUT", "RES", "ENV", "REV"};
static const char *const waves[LFO_WAVES] = {"SINE", "TRI", "SAW UP", "SAW DN", "SQUARE", "RANDOM"};
/* Synced rates in beats per cycle (quarter note = 1), then free rates. */
enum { SYNC_RATES = 14 };
static const float sync_beats[SYNC_RATES] = {32, 16, 8, 4, 2, 1.5f, 1, 0.75f, 0.6666667f, 0.5f, 0.3333333f, 0.25f, 0.1666667f, 0.125f};
static const char *const sync_names[SYNC_RATES] = {"8/1", "4/1", "2/1", "1/1", "1/2", "1/4D", "1/4", "1/8D", "1/4T", "1/8", "1/8T", "1/16", "16T", "1/32"};

static struct lfo {
    uint8_t target, rate, depth, wave, base, last_cc;
    float phase, out, held, prev_held;
} lfos[LFO_COUNT];
static unsigned was_playing, seed = 12345;
static float beats;

static float exp2f_(float x)
{
    float r = 1.0f;
    if (!(x > -40.0f)) x = -40.0f; /* also catches NaN: loops below stay bounded */
    if (x > 40.0f) x = 40.0f;
    while (x >= 1.0f) { r *= 2.0f; x -= 1.0f; }
    while (x < 0.0f) { r *= 0.5f; x += 1.0f; }
    return r * (1.0f + x * (0.6931472f + x * (0.2402265f + x * 0.0555041f)));
}
static float sin_cycles(float c)
{
    c -= (float)(int)c; if (c < 0) c += 1;
    float a = (c > 0.5f ? c - 1.0f : c) * 6.2831853f;
    if (a > 1.5707963f) a = 3.1415927f - a; else if (a < -1.5707963f) a = -3.1415927f - a;
    float q = a * a;
    return a * (1 - q * (0.1666667f - q * (0.0083333f - q * 0.0001984f)));
}
static float random11(void) { seed = seed * 1103515245U + 12345U; return (float)((seed >> 9) & 0xffffU) / 32767.5f - 1.0f; }

void lfo_init(void)
{
    memset(lfos, 0, sizeof lfos);
    for (unsigned i = 0; i < LFO_COUNT; ++i) { lfos[i].rate = 6; lfos[i].depth = 63; lfos[i].last_cc = 255; }
    beats = 0; was_playing = 0;
}
unsigned lfo_target_count(void) { return TARGETS; }
const char *lfo_target_name(unsigned t, char out[12])
{
    if (!t) { strcpy(out, "OFF"); return out; }
    if (t <= MASTER_TARGETS) { strcpy(out, master_names[t - 1]); return out; }
    unsigned i = t - 1 - MASTER_TARGETS, track = i / TRACK_PARAMS, p = i % TRACK_PARAMS;
    out[0] = 'T'; out[1] = (char)('1' + track); out[2] = ' '; strcpy(out + 3, track_names[p]);
    return out;
}
const char *lfo_wave_name(unsigned w) { return waves[w % LFO_WAVES]; }
void lfo_rate_name(unsigned r, char out[12])
{
    if (r < SYNC_RATES) { strcpy(out, sync_names[r]); return; }
    float hz = 0.05f * exp2f_((float)(r - SYNC_RATES) * (8.6f / (LFO_RATES - SYNC_RATES - 1)));
    unsigned c = (unsigned)(hz * 100.0f + 0.5f); /* centi-Hz */
    char *o = out;
    if (c < 100) { *o++ = '.'; *o++ = (char)('0' + c / 10); *o++ = (char)('0' + c % 10); }
    else if (c < 1000) { *o++ = (char)('0' + c / 100); *o++ = '.'; *o++ = (char)('0' + (c / 10) % 10); }
    else { unsigned w = (c + 50) / 100; *o++ = (char)('0' + w / 10); *o++ = (char)('0' + w % 10); }
    strcpy(o, "HZ");
}
unsigned lfo_get(unsigned l, unsigned p)
{
    if (l >= LFO_COUNT) return 0;
    return p == 0 ? lfos[l].target : p == 1 ? lfos[l].rate : p == 2 ? lfos[l].depth : lfos[l].wave;
}
static unsigned cc_base(unsigned target)
{
    unsigned i = target - 1 - MASTER_TARGETS, track = i / TRACK_PARAMS, p = i % TRACK_PARAMS;
    unsigned type = groove_source_type(track);
    uint8_t tone[GROOVE_TONE_PARAMS]; memset(tone, 0, sizeof tone);
    if (p >= 2 && p <= 4) groove_tone_copy(track, 0, tone);
    switch (p) {
    case 0: return 127;
    case 1: return 64;
    case 2: return type == GROOVE_303 ? tone[0] : type == GROOVE_PLUCK ? tone[1] : type == GROOVE_PSG ? tone[1] * 4U : 64U;
    case 3: return type == GROOVE_303 ? tone[1] : 64U;
    case 4: return type == GROOVE_303 ? tone[2] : 0U;
    default: { uint8_t s[2]; groove_sends_get(track, s); return s[0] * 127U / 15U; }
    }
}
static int pending_restore[LFO_COUNT][2]; /* target, value; target 0 = none */
void lfo_edit(unsigned l, unsigned p, int d)
{
    if (l >= LFO_COUNT || !d) return;
    struct lfo *f = &lfos[l];
    if (p == 0) {
        int t = (int)f->target + d;
        t = t < 0 ? 0 : t >= TARGETS ? TARGETS - 1 : t;
        if ((unsigned)t == f->target) return;
        if (f->target > MASTER_TARGETS) { pending_restore[l][0] = f->target; pending_restore[l][1] = f->base; }
        f->target = (uint8_t)t;
        f->last_cc = 255;
        if (f->target > MASTER_TARGETS) f->base = (uint8_t)cc_base(f->target);
    } else if (p == 1) { int v = f->rate + d; f->rate = (uint8_t)(v < 0 ? 0 : v >= LFO_RATES ? LFO_RATES - 1 : v); }
    else if (p == 2) { int v = f->depth + d; f->depth = (uint8_t)(v < 0 ? 0 : v > 126 ? 126 : v); }
    else { int v = (int)f->wave + (d > 0 ? 1 : -1); f->wave = (uint8_t)(v < 0 ? 0 : v >= LFO_WAVES ? LFO_WAVES - 1 : v); }
}
float lfo_shape(unsigned wave, float ph)
{
    ph -= (float)(int)ph; if (ph < 0) ph += 1;
    switch (wave % LFO_WAVES) {
    case 0: return sin_cycles(ph);
    case 1: return ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph;
    case 2: return 2 * ph - 1;
    case 3: return 1 - 2 * ph;
    case 4: return ph < 0.5f ? 1.0f : -1.0f;
    default: return sin_cycles(ph * 3.0f) * 0.6f + sin_cycles(ph * 7.0f + 0.3f) * 0.4f; /* picture only */
    }
}
float lfo_phase(unsigned l) { return l < LFO_COUNT ? lfos[l].phase : 0; }
float lfo_output(unsigned l) { return l < LFO_COUNT ? lfos[l].out : 0; }
unsigned lfo_tick(unsigned dt_ms, unsigned tempo, unsigned playing, unsigned allow_cc, struct lfo_cc *out, unsigned max)
{
    unsigned n = 0;
    float dt = (float)dt_ms * 0.001f, bpm = tempo ? (float)tempo : 120.0f;
    if (playing && !was_playing) { beats = 0; for (unsigned i = 0; i < LFO_COUNT; ++i) if (lfos[i].rate < SYNC_RATES) lfos[i].phase = 0; }
    was_playing = playing;
    beats += dt * bpm / 60.0f;
    int mods[MFX_COUNT][4]; memset(mods, 0, sizeof mods);
    for (unsigned l = 0; l < LFO_COUNT; ++l) {
        if (pending_restore[l][0] && n < max && allow_cc) {
            unsigned i = (unsigned)pending_restore[l][0] - 1 - MASTER_TARGETS;
            out[n].channel = (uint8_t)groove_midi_input_channel(i / TRACK_PARAMS); out[n].cc = track_cc[i % TRACK_PARAMS];
            out[n].value = (uint8_t)pending_restore[l][1]; ++n; pending_restore[l][0] = 0;
        }
        struct lfo *f = &lfos[l];
        float prev = f->phase;
        if (f->rate < SYNC_RATES) f->phase += dt * bpm / 60.0f / sync_beats[f->rate];
        else f->phase += dt * 0.05f * exp2f_((float)(f->rate - SYNC_RATES) * (8.6f / (LFO_RATES - SYNC_RATES - 1)));
        f->phase -= (float)(int)f->phase;
        if (f->wave == 5) { if (f->phase < prev) { f->prev_held = f->held; f->held = random11(); } f->out = f->held; }
        else f->out = lfo_shape(f->wave, f->phase);
        float amount = ((float)f->depth - 63.0f) / 63.0f; /* -1..1 */
        if (!f->target) continue;
        if (f->target <= MASTER_TARGETS) {
            mods[master_map[f->target - 1][0]][master_map[f->target - 1][1]] += (int)(f->out * amount * 127.0f);
            continue;
        }
        unsigned i = f->target - 1 - MASTER_TARGETS, p = i % TRACK_PARAMS;
        int v;
        if (p == 0) { float a = amount < 0 ? -amount : amount, s = amount < 0 ? -f->out : f->out; v = (int)(127.0f * (1.0f - a * (0.5f - 0.5f * s))); }
        else v = (int)f->base + (int)(f->out * amount * 127.0f);
        v = v < 0 ? 0 : v > 127 ? 127 : v;
        if (allow_cc && (unsigned)v != f->last_cc && n < max) {
            out[n].channel = (uint8_t)groove_midi_input_channel(i / TRACK_PARAMS); out[n].cc = track_cc[p]; out[n].value = (uint8_t)v; ++n;
            f->last_cc = (uint8_t)v;
        }
    }
    for (unsigned fx = 0; fx < MFX_COUNT; ++fx)
        for (unsigned p = 0; p < 4; ++p) mfx_mod_set(fx, p, mods[fx][p]);
    return n;
}

static void state_get(unsigned l, uint8_t out[4])
{
    out[0] = lfos[l].target; out[1] = lfos[l].rate; out[2] = lfos[l].depth; out[3] = lfos[l].wave;
}
void lfo_state_get(uint8_t out[4]) { state_get(0, out); }
void lfo2_state_get(uint8_t out[4]) { state_get(1, out); }
static void state_put(unsigned l, const uint8_t in[4])
{
    struct lfo *f = &lfos[l];
    if (f->target > MASTER_TARGETS) { pending_restore[l][0] = f->target; pending_restore[l][1] = f->base; }
    f->target = in[0] < TARGETS ? in[0] : 0;
    f->rate = in[1] < LFO_RATES ? in[1] : 6;
    f->depth = in[2] <= 126 ? in[2] : 63;
    f->wave = in[3] < LFO_WAVES ? in[3] : 0;
    f->last_cc = 255;
    if (f->target > MASTER_TARGETS) f->base = (uint8_t)cc_base(f->target);
}
void lfo_state_put(const uint8_t in[4]) { state_put(0, in); }
void lfo2_state_put(const uint8_t in[4]) { state_put(1, in); }
