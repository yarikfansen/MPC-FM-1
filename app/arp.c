/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "arp.h"
#include <string.h>

enum { TRACKS = 8, MAX_NOTES = 32, RATES = 6, MODES = 6 };
static const char *const rate_names[RATES] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const float rate_beats[RATES] = {1.0f, 0.5f, 0.3333333f, 0.25f, 0.1666667f, 0.125f};
static const char *const mode_names[MODES] = {"UP", "DOWN", "UPDN IN", "UPDN EX", "RANDOM", "PLAYED"};
static const char *const param_names[ARP_PARAMS] = {"RATE", "MODE", "OCTAVES", "GATE", "HUMANIZE", "HOLD"};
static const uint8_t limits[ARP_PARAMS] = {RATES - 1, MODES - 1, 3, 15, 100, 1};

static struct arp_track {
    uint8_t on, p[ARP_PARAMS];
    uint8_t latched[16], latched_count, sounding;
    uint32_t epoch, next_ms, off_ms;
    unsigned step, was_playing, had_keys;
} arps[TRACKS];
static unsigned seed = 2463534242U;
static unsigned rnd(unsigned n) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return n ? seed % n : 0; }

static struct arp_track *get(unsigned t)
{
    struct arp_track *a = &arps[t & 7U];
    if (!a->p[ARP_GATE] && !a->p[ARP_RATE] && !a->p[ARP_MODE]) { /* defaults on first use */
        a->p[ARP_RATE] = 3; a->p[ARP_GATE] = 9; a->sounding = 255;
    }
    return a;
}
unsigned arp_enabled(unsigned t) { return t < TRACKS && arps[t].on; }
void arp_set(unsigned t, unsigned on) { if (t < TRACKS) { struct arp_track *a = get(t); a->on = on != 0; a->step = 0; a->latched_count = 0; a->epoch = 0; } }
unsigned arp_get(unsigned t, unsigned p) { return t < TRACKS && p < ARP_PARAMS ? get(t)->p[p] : 0; }
void arp_edit(unsigned t, unsigned p, int d)
{
    if (t >= TRACKS || p >= ARP_PARAMS || !d) return;
    struct arp_track *a = get(t);
    int v = (int)a->p[p] + (p == ARP_HUMAN ? d * 2 : d);
    a->p[p] = (uint8_t)(v < 0 ? 0 : v > limits[p] ? limits[p] : v);
    if (p == ARP_HOLD && !a->p[p]) a->latched_count = 0;
}
const char *arp_param_name(unsigned p) { return p < ARP_PARAMS ? param_names[p] : ""; }
void arp_value_name(unsigned t, unsigned p, char out[12])
{
    unsigned v = arp_get(t, p);
    out[0] = 0;
    switch (p) {
    case ARP_RATE: strcpy(out, rate_names[v % RATES]); break;
    case ARP_MODE: strcpy(out, mode_names[v % MODES]); break;
    case ARP_OCT: out[0] = (char)('1' + v); strcpy(out + 1, " OCT"); break;
    case ARP_GATE: { unsigned g = (v + 1) * 100 / 16; out[0] = (char)('0' + g / 100 % 10); out[1] = (char)('0' + g / 10 % 10); out[2] = (char)('0' + g % 10); out[3] = '%'; out[4] = 0;
        if (g < 100) memmove(out, out + 1, 5); if (g < 10) memmove(out, out + 1, 4); break; }
    case ARP_HUMAN: { if (!v) { strcpy(out, "OFF"); break; }
        char *o = out; if (v >= 100) *o++ = '1'; if (v >= 10) *o++ = (char)('0' + v / 10 % 10); *o++ = (char)('0' + v % 10); *o++ = '%'; *o = 0; break; }
    default: strcpy(out, v ? "ON" : "OFF");
    }
}
/* Build the ordered note cycle for the mode. */
static unsigned build(const struct arp_track *a, const uint8_t *held, unsigned count, uint8_t *cyc, unsigned max)
{
    uint8_t base[16]; unsigned n = count > 16 ? 16 : count;
    memcpy(base, held, n);
    if (a->p[ARP_MODE] != 5) /* sort ascending for every mode except AS PLAYED */
        for (unsigned i = 1; i < n; ++i) for (unsigned j = i; j && base[j - 1] > base[j]; --j) { uint8_t t = base[j]; base[j] = base[j - 1]; base[j - 1] = t; }
    uint8_t up[MAX_NOTES]; unsigned m = 0;
    for (unsigned o = 0; o <= a->p[ARP_OCT]; ++o)
        for (unsigned i = 0; i < n && m < MAX_NOTES; ++i) { unsigned note = base[i] + 12U * o; if (note < 128) up[m++] = (uint8_t)note; }
    unsigned k = 0;
    switch (a->p[ARP_MODE]) {
    case 1: for (unsigned i = m; i-- && k < max;) cyc[k++] = up[i]; break;
    case 2: for (unsigned i = 0; i < m && k < max; ++i) cyc[k++] = up[i];          /* inclusive: top and bottom repeat */
            for (unsigned i = m; i-- && k < max;) cyc[k++] = up[i]; break;
    case 3: for (unsigned i = 0; i < m && k < max; ++i) cyc[k++] = up[i];          /* exclusive: no repeats */
            for (unsigned i = m >= 2 ? m - 2 : 0; i >= 1 && i < m && k < max; --i) cyc[k++] = up[i]; break;
    default: for (unsigned i = 0; i < m && k < max; ++i) cyc[k++] = up[i];
    }
    return k;
}
unsigned arp_pattern(unsigned t, const uint8_t *held, unsigned count, uint8_t *notes, unsigned max)
{
    static const uint8_t demo[3] = {60, 64, 67};
    if (!count) { held = demo; count = 3; }
    return build(get(t), held, count, notes, max);
}
static unsigned emit(struct arp_event *out, unsigned n, unsigned max, uint8_t st, uint8_t note, uint8_t vel)
{
    if (n < max) { out[n].status = st; out[n].note = note; out[n].velocity = vel; ++n; }
    return n;
}
unsigned arp_stop(unsigned t, unsigned ch, struct arp_event *out, unsigned max)
{
    if (t >= TRACKS) return 0;
    struct arp_track *a = get(t);
    unsigned n = 0;
    if (a->sounding != 255) { n = emit(out, n, max, (uint8_t)(0x80 | ch), a->sounding, 0); a->sounding = 255; }
    return n;
}
unsigned arp_tick(unsigned t, unsigned ch, uint32_t now, unsigned tempo, unsigned playing,
                  const uint8_t *held, unsigned count, struct arp_event *out, unsigned max)
{
    if (t >= TRACKS) return 0;
    struct arp_track *a = get(t);
    unsigned n = 0;
    if (!a->on) return arp_stop(t, ch, out, max);
    /* Hold latches the last chord until a new one is played. */
    if (a->p[ARP_HOLD]) {
        if (count) {
            if (!a->had_keys) a->latched_count = 0;
            for (unsigned i = 0; i < count && a->latched_count < 16; ++i) {
                unsigned dup = 0;
                for (unsigned j = 0; j < a->latched_count; ++j) dup |= a->latched[j] == held[i];
                if (!dup) a->latched[a->latched_count++] = held[i];
            }
        }
        a->had_keys = count != 0;
        held = a->latched; count = a->latched_count;
    }
    float step_ms = 60000.0f / (float)(tempo ? tempo : 120) * rate_beats[a->p[ARP_RATE] % RATES];
    if (a->sounding != 255 && (int32_t)(now - a->off_ms) >= 0) n = emit(out, n, max, (uint8_t)(0x80 | ch), a->sounding, 0), a->sounding = 255;
    if (!count) { a->step = 0; a->epoch = 0; return n; }
    if (!a->epoch || (playing && !a->was_playing)) { a->epoch = now; a->step = 0; a->next_ms = now; }
    a->was_playing = playing;
    if ((int32_t)(now - a->next_ms) < 0) return n;
    uint8_t cyc[MAX_NOTES * 2];
    unsigned len = build(a, held, count, cyc, sizeof cyc);
    if (!len) return n;
    unsigned idx = a->step % len;
    uint8_t note = a->p[ARP_MODE] == 4 ? cyc[rnd(len)] : cyc[idx];
    /* Accent: first note of the cycle. Anchor: notes on the beat. Both stay exact. */
    unsigned beat_steps = (unsigned)(1.0f / rate_beats[a->p[ARP_RATE] % RATES] + 0.5f);
    unsigned accent = idx == 0, anchor = beat_steps ? a->step % beat_steps == 0 : 1;
    unsigned vel = accent ? 112 : anchor ? 100 : 88;
    if (a->p[ARP_HUMAN] && !accent && !anchor) {
        int jitter = (int)rnd(2U * a->p[ARP_HUMAN] / 4U + 1U) - (int)(a->p[ARP_HUMAN] / 4U); /* velocity +-25 */
        int v = (int)vel + jitter; vel = (unsigned)(v < 30 ? 30 : v > 127 ? 127 : v);
    }
    if (a->sounding != 255) n = emit(out, n, max, (uint8_t)(0x80 | ch), a->sounding, 0);
    n = emit(out, n, max, (uint8_t)(0x90 | ch), note, (uint8_t)vel);
    a->sounding = note;
    a->off_ms = now + (uint32_t)(step_ms * (float)(a->p[ARP_GATE] + 1) / 16.0f);
    ++a->step;
    /* Next step on the grid; off-beat notes may land a little late (humanize). */
    uint32_t grid = a->epoch + (uint32_t)(step_ms * (float)a->step);
    unsigned late = 0;
    if (a->p[ARP_HUMAN]) {
        unsigned next_accent = len && a->step % len == 0, next_anchor = beat_steps ? a->step % beat_steps == 0 : 1;
        if (!next_accent && !next_anchor) late = rnd((unsigned)(step_ms * 0.18f * a->p[ARP_HUMAN] / 100.0f) + 1U);
    }
    a->next_ms = grid + late;
    return n;
}

/* Pattern extras: per track on + parameters (7 bytes x 8). */
void arp_state_get(uint8_t out[56])
{
    for (unsigned t = 0; t < TRACKS; ++t) {
        struct arp_track *a = get(t);
        out[t * 7] = a->on;
        memcpy(out + t * 7 + 1, a->p, ARP_PARAMS);
    }
}
void arp_state_put(const uint8_t in[56])
{
    for (unsigned t = 0; t < TRACKS; ++t) {
        const uint8_t *s = in + t * 7;
        struct arp_track *a = get(t);
        if (s[0] == 0xff) { a->p[ARP_RATE] = 3; a->p[ARP_MODE] = 0; a->p[ARP_OCT] = 0; a->p[ARP_GATE] = 9; a->p[ARP_HUMAN] = 0; a->p[ARP_HOLD] = 0; arp_set(t, 0); continue; }
        for (unsigned p = 0; p < ARP_PARAMS; ++p) a->p[p] = s[1 + p] > limits[p] ? limits[p] : s[1 + p];
        arp_set(t, s[0] == 1);
    }
}
