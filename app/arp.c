/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "arp.h"
#include "keyscale.h"
#include <string.h>

enum { TRACKS = 8, MAX_NOTES = 32, RATES = 6, MODES = 6 };
static const char *const rate_names[RATES] = {"1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32"};
static const float rate_beats[RATES] = {1.0f, 0.5f, 0.3333333f, 0.25f, 0.1666667f, 0.125f};
static const char *const mode_names[MODES] = {"UP", "DOWN", "UPDN IN", "UPDN EX", "RANDOM", "PLAYED"};
static const char *const param_names[ARP_ALL] = {"RATE", "MODE", "OCTAVES", "GATE", "HUMANIZE", "HOLD", "MUTATE", "VAR", "CHANCE"};
static const uint8_t limits[ARP_ALL] = {RATES - 1, MODES - 1, 3, 15, 100, 1, 100, 3, 4};
/* v13 CHANCE 1..4: the cycle is played through a pattern of 3, 4, 5 or 7 steps (indices into the note
 * cycle, wrapped to its length). The pattern's length against the cycle's makes the polyrhythm. */
static const uint8_t poly_len[4] = {3, 4, 5, 7};
static const uint8_t poly_pat[4][7] = {{0, 2, 1}, {3, 1, 2, 0}, {1, 3, 0, 2, 3}, {2, 0, 3, 1, 0, 3, 1}};
unsigned arp_poly_pattern(unsigned variant, uint8_t out[7])
{
    if (variant > 3) return 0;
    memcpy(out, poly_pat[variant], poly_len[variant]);
    return poly_len[variant];
}

static struct arp_track {
    uint8_t on, p[ARP_ALL];
    uint8_t latched[16], latched_count, sounding;
    uint32_t epoch, next_ms, off_ms;
    unsigned step, was_playing, had_keys;
    unsigned shift; /* note-cycle offset kept over a PLAY start while REC is on (arp_keep_note) */
    unsigned base;  /* the step that lies on epoch (the grid restarts there when RATE or the tempo changes) */
    float used_ms;  /* step length the waiting note was timed with */
} arps[TRACKS];
static unsigned seed = 2463534242U;
static unsigned swing_pct; /* 0..50, the sequencer's swing: every second note of the arpeggio starts that share of a step late */
void arp_swing_set(unsigned pct) { swing_pct = pct > 50U ? 50U : pct; }
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
unsigned arp_get(unsigned t, unsigned p) { return t < TRACKS && p < ARP_ALL ? get(t)->p[p] : 0; }
unsigned arp_press_latch(unsigned arp_was_on, unsigned latch) { return arp_was_on ? latch != 0 : 0U; }
unsigned arp_hold_toggle(unsigned arp_was_on) { return !arp_was_on; }
unsigned arp_rec_pause(unsigned writing, unsigned playing, unsigned arp_feeds) { return writing && !playing && arp_feeds; }
unsigned arp_grid_writes(unsigned arp_on, unsigned pads) { return !arp_on || pads; }
static unsigned keep_note;
void arp_keep_note(unsigned on) { keep_note = on != 0; }
static unsigned rec_was, rec_take; /* REC in the last call; tracks whose arp was on during the current take */
unsigned arp_rec_follow(unsigned writing, unsigned arp_mask)
{
    unsigned off = 0;
    if (writing) rec_take = (rec_was ? rec_take : 0U) | (arp_mask & 0xffU);
    else if (rec_was) { off = rec_take; rec_take = 0; }
    rec_was = writing != 0;
    return off;
}
void arp_edit(unsigned t, unsigned p, int d)
{
    if (t >= TRACKS || p >= ARP_ALL || !d) return;
    struct arp_track *a = get(t);
    int v = (int)a->p[p] + (p == ARP_HUMAN ? d * 2 : d);
    a->p[p] = (uint8_t)(v < 0 ? 0 : v > limits[p] ? limits[p] : v);
    if (p == ARP_HOLD && !a->p[p]) a->latched_count = 0;
}
const char *arp_param_name(unsigned p) { return p < ARP_ALL ? param_names[p] : ""; }
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
    case ARP_MUTATE: { char *o = out; if (v >= 100) *o++ = '1'; if (v >= 10) *o++ = (char)('0' + v / 10 % 10); *o++ = (char)('0' + v % 10); *o = 0; break; }
    case ARP_VAR: if (v) { out[0] = (char)('0' + v); out[1] = 0; } else strcpy(out, "RND"); break;
    case ARP_CHANCE: if (v && v <= 4) { out[0] = (char)('0' + poly_len[v - 1]); strcpy(out + 1, ":4"); } else strcpy(out, "OFF"); break;
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
    if (!a->epoch || (playing && !a->was_playing)) {
        /* User 11.11: REC on, PLAY while the arp runs: the take goes on from the next note of the cycle. */
        a->shift = keep_note && a->epoch ? a->shift + a->step : 0;
        a->epoch = now; a->step = a->base = 0; a->next_ms = now; a->used_ms = step_ms;
    }
    a->was_playing = playing;
    if (step_ms != a->used_ms) {
        /* RATE or the tempo changed while running: the grid goes on from the last note at the new
         * distance. Counted from the first note, the new step length would put the next note far in
         * the future (silence) or in the past (a note on every tick until it caught up). */
        if (a->step > a->base) {
            a->epoch += (uint32_t)(a->used_ms * (float)(a->step - 1U - a->base));
            if (!a->epoch) a->epoch = 1; /* 0 means "not running" */
            a->base = a->step - 1U;
            a->next_ms = a->epoch + (uint32_t)step_ms;
            if ((a->step & 1U) && swing_pct) a->next_ms += (uint32_t)(step_ms * (float)swing_pct / 100.0f);
            /* Already due with the shorter step: it plays now and the grid starts again here
             * (keeping the old grid would put the note after it only a few ms later). */
            if ((int32_t)(now - a->next_ms) >= 0) { a->epoch = now; a->base = a->step; a->next_ms = now; }
        }
        a->used_ms = step_ms;
    }
    if ((int32_t)(now - a->next_ms) < 0) return n;
    /* More than a step late (the control task was held up, or the step became much shorter): this
     * note starts a new grid. Never a burst of catch-up notes. */
    if ((int32_t)(now - a->next_ms) > (int32_t)step_ms + 20) { a->epoch = now; a->base = a->step; }
    uint8_t cyc[MAX_NOTES * 2];
    unsigned len = build(a, held, count, cyc, sizeof cyc);
    if (!len) return n;
    /* v13 CHANCE: a polyrhythm pattern of 3, 4, 5 or 7 steps picks the cycle's notes; its length is the cycle. */
    unsigned poly = a->p[ARP_CHANCE] >= 1 && a->p[ARP_CHANCE] <= 4 ? a->p[ARP_CHANCE] - 1U : 4U;
    unsigned cycle = poly < 4 ? poly_len[poly] : len;
    unsigned pos = a->step + a->shift; /* place in the note cycle; a->step alone is the place on the time grid */
    unsigned idx = poly < 4 ? poly_pat[poly][pos % cycle] % len : pos % len;
    uint8_t note = a->p[ARP_MODE] == 4 ? cyc[rnd(len)] : cyc[idx];
    /* v13 MUTATE: a share of the notes is replaced; VAR 1 = the next note of the cycle, 2 = an octave
     * away, 3 = any note of the cycle, RND = one of the three, drawn again for every mutated note. */
    if (a->p[ARP_MUTATE] && rnd(100U) < a->p[ARP_MUTATE]) {
        /* C3: in key like the SEQ's MUTATE: the new note moves into the scale; KEY AUTO without a chord = octaves only */
        unsigned mask = key_scale_mask(), how = a->p[ARP_VAR] ? a->p[ARP_VAR] : 1U + rnd(3U);
        if (!mask) how = 2;
        if (how == 1) note = cyc[(idx + 1U) % len];
        else if (how == 2) note = note + 12U < 128U && (note < 12U || rnd(2U)) ? (uint8_t)(note + 12U) : (uint8_t)(note - 12U);
        else note = cyc[rnd(len)];
        note = (uint8_t)key_snap(note, mask);
    }
    /* Accent: first note of the cycle. Anchor: notes on the beat. Both stay exact. */
    unsigned beat_steps = (unsigned)(1.0f / rate_beats[a->p[ARP_RATE] % RATES] + 0.5f);
    unsigned accent = pos % cycle == 0, anchor = beat_steps ? a->step % beat_steps == 0 : 1;
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
    uint32_t grid = a->epoch + (uint32_t)(step_ms * (float)(a->step - a->base));
    /* Swing, the sequencer's rule: of each pair of steps the first lasts (100 + swing) %, the second (100 - swing) %,
     * so the odd (off-beat) note starts swing % of a step late and the pair keeps its length. 0 = the old timing. */
    if ((a->step & 1U) && swing_pct) grid += (uint32_t)(step_ms * (float)swing_pct / 100.0f);
    unsigned late = 0;
    if (a->p[ARP_HUMAN]) {
        unsigned next_accent = cycle && (pos + 1U) % cycle == 0, next_anchor = beat_steps ? a->step % beat_steps == 0 : 1;
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
        a->p[ARP_MUTATE] = a->p[ARP_VAR] = a->p[ARP_CHANCE] = 0; /* the v13 randomisation page is not stored: every project starts plain */
        if (s[0] == 0xff) { a->p[ARP_RATE] = 3; a->p[ARP_MODE] = 0; a->p[ARP_OCT] = 0; a->p[ARP_GATE] = 9; a->p[ARP_HUMAN] = 0; a->p[ARP_HOLD] = 0; arp_set(t, 0); continue; }
        for (unsigned p = 0; p < ARP_PARAMS; ++p) a->p[p] = s[1 + p] > limits[p] ? limits[p] : s[1 + p];
        arp_set(t, s[0] == 1);
    }
}
