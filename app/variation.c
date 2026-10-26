/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "variation.h"
#include "instrument.h"
#include <string.h>

enum { TRACKS = 8, STEPS = 64, PENDING = 24 };
static const uint8_t limit[VAR_PARAMS] = {100, 100, 3, 7};
static struct var_track {
    uint8_t amount[VAR_PARAMS];
    uint8_t have, prev_step, length, was_playing;
    struct groove_step base[STEPS], shown[STEPS];
} vt[TRACKS];
static struct { uint32_t due; uint8_t status, note, velocity; } pending[PENDING];
static unsigned pending_count;
static uint32_t seed = 0x2545f491U;

static unsigned rnd(unsigned n) { seed = seed * 1664525U + 1013904223U; return (seed >> 8) % n; }
static int same(const struct groove_step *a, const struct groove_step *b)
{
    return a->note == b->note && a->velocity == b->velocity && a->drums == b->drums;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

unsigned var_get(unsigned t, unsigned p) { return t < TRACKS && p < VAR_PARAMS ? vt[t].amount[p] : 0; }
void var_value(unsigned t, unsigned p, char out[12])
{
    static const char *const ratchet[4] = {"OFF", "2X", "3X", "4X"};
    unsigned v = var_get(t, p), pct = p == VAR_CHANCE ? (v ? (v * 100U + 4U) / 8U : 100U) : v;
    if (p == VAR_RATCHET) { strcpy(out, ratchet[v & 3U]); return; }
    char *o = out;
    if (pct >= 100) *o++ = '1';
    if (pct >= 10) *o++ = (char)('0' + (pct / 10U) % 10U);
    *o++ = (char)('0' + pct % 10U); *o++ = '%'; *o = 0;
}
/* CHANCE lives in the pattern: every note of the track gets the probability. */
static void apply_chance(unsigned t)
{
    unsigned len = groove_sequence_length(t), want = vt[t].amount[VAR_CHANCE];
    unsigned pads = groove_pad_count(t) != 0;
    for (unsigned s = 0; s < len && s < STEPS; ++s) {
        struct groove_step c; groove_sequence_read(t, s, &c, 1);
        if (pads ? !c.drums : !c.velocity) continue;
        int now = (int)groove_step_setting(t, s, 2);
        if ((unsigned)now != want) (void)groove_step_setting_edit(t, s, 2, (int)want - now);
    }
}
static void restore(unsigned t)
{
    struct var_track *v = &vt[t];
    if (!v->have) return;
    static struct groove_step cur[STEPS];
    groove_sequence_read(t, 0, cur, v->length);
    for (unsigned s = 0; s < v->length; ++s) if (same(&cur[s], &v->shown[s])) cur[s] = v->base[s];
    groove_sequence_replace(t, 0, cur, v->length);
    v->have = 0;
}
void var_edit(unsigned t, unsigned p, int d)
{
    if (t >= TRACKS || p >= VAR_PARAMS || !d) return;
    int n = clampi((int)vt[t].amount[p] + (p >= VAR_RATCHET ? (d > 0 ? 1 : -1) : d), 0, limit[p]);
    if ((unsigned)n == vt[t].amount[p]) return;
    vt[t].amount[p] = (uint8_t)n;
    if (p == VAR_CHANCE) { restore(t); apply_chance(t); }
}
void var_restore_all(void) { for (unsigned t = 0; t < TRACKS; ++t) restore(t); }
void var_forget(void) { for (unsigned t = 0; t < TRACKS; ++t) vt[t].have = 0; pending_count = 0; }
/* One loop's version of the track, from the original. */
static void build(unsigned t, unsigned len)
{
    struct var_track *v = &vt[t];
    static struct groove_step cur[STEPS], out[STEPS]; /* control task only: off the stack */
    if (len > STEPS) len = STEPS;
    if (v->have && v->length != len) restore(t);
    groove_sequence_read(t, 0, cur, len);
    if (!v->have) { memcpy(v->base, cur, len * sizeof cur[0]); v->have = 1; v->length = (uint8_t)len; }
    else for (unsigned s = 0; s < len; ++s) if (!same(&cur[s], &v->shown[s])) v->base[s] = cur[s]; /* edited */
    memcpy(out, v->base, len * sizeof out[0]);
    unsigned pads = groove_pad_count(t) != 0, mutate = v->amount[VAR_MUTATE], human = v->amount[VAR_HUMAN];
    static const int8_t jump[4] = {12, -12, 7, -5};
    for (unsigned s = 0; s < len; ++s) {
        struct groove_step *c = &out[s];
        if (pads ? !c->drums : !c->velocity) continue;
        if (mutate && rnd(100) < mutate) {
            if (!pads) c->note = (uint8_t)clampi((int)c->note + jump[rnd(4)], 0, 127);
            else {
                unsigned n = (s + 1) % len;
                if (n > s && !out[n].drums && !v->base[n].drums) { out[n] = *c; memset(c, 0, sizeof *c); ++s; continue; }
            }
        }
        if (human && c->velocity) {
            int h = (int)human * 40 / 100;
            c->velocity = (uint8_t)clampi((int)c->velocity + (int)rnd((unsigned)(2 * h + 1)) - h, 1, 127);
        }
    }
    groove_sequence_replace(t, 0, out, len);
    memcpy(v->shown, out, len * sizeof out[0]);
}
static void queue(uint32_t due, uint8_t status, uint8_t note, uint8_t velocity)
{
    if (pending_count >= PENDING) return;
    pending[pending_count].due = due; pending[pending_count].status = status;
    pending[pending_count].note = note; pending[pending_count].velocity = velocity; ++pending_count;
}
/* RATCHET: repeat the step's note(s) inside the step, half-step gates. */
static void ratchet(unsigned t, unsigned step, unsigned tempo, uint32_t now)
{
    unsigned hits = vt[t].amount[VAR_RATCHET] + 1U;
    if (hits < 2 || rnd(3)) return;
    struct groove_step c; groove_sequence_read(t, step, &c, 1);
    unsigned pads = groove_pad_count(t) != 0;
    if (pads ? !c.drums : !c.velocity) return;
    uint32_t dur = 15000U / (tempo ? tempo : 120U), ch = groove_midi_input_channel(t) & 15U;
    for (unsigned k = 1; k < hits; ++k) {
        uint32_t at = now + dur * k / hits, off = at + dur / (2U * hits);
        if (pads) {
            for (unsigned p = 0; p < groove_pad_count(t) && p < 16; ++p) if (c.drums >> p & 1U) {
                unsigned note = groove_pad_note(t, p);
                queue(at, (uint8_t)(0x90 | ch), (uint8_t)note, 100); queue(off, (uint8_t)(0x80 | ch), (uint8_t)note, 0);
            }
        } else {
            queue(at - 1U, (uint8_t)(0x80 | ch), c.note, 0);
            queue(at, (uint8_t)(0x90 | ch), c.note, c.velocity);
            queue(off, (uint8_t)(0x80 | ch), c.note, 0);
        }
    }
}
void var_tick(uint32_t now, var_post_fn post)
{
    for (unsigned t = 0; t < TRACKS; ++t) {
        struct var_track *v = &vt[t];
        struct groove_view view; memset(&view, 0, sizeof view);
        groove_get_view(t, 0, &view);
        unsigned live = v->amount[VAR_MUTATE] || v->amount[VAR_HUMAN];
        if (!view.playing) { if (v->have) restore(t); v->was_playing = 0; v->prev_step = 0; continue; }
        if (!live && v->have) restore(t);
        unsigned step = view.play_step;
        if (step != v->prev_step || !v->was_playing) {
            if (live && (step < v->prev_step || !v->was_playing)) build(t, view.sequence_length);
            if (v->amount[VAR_RATCHET] && view.audible) ratchet(t, step, view.tempo, now);
        }
        v->prev_step = (uint8_t)step; v->was_playing = 1;
    }
    for (unsigned i = 0; i < pending_count;) {
        if ((int32_t)(now - pending[i].due) >= 0) {
            if (!post(pending[i].status, pending[i].note, pending[i].velocity)) break; /* busy: next tick */
            pending[i] = pending[--pending_count];
        } else ++i;
    }
}
void var_state_get(uint8_t out[32])
{
    for (unsigned t = 0; t < TRACKS; ++t) memcpy(out + t * 4, vt[t].amount, 4);
}
void var_state_put(const uint8_t in[32])
{
    var_forget();
    for (unsigned t = 0; t < TRACKS; ++t)
        for (unsigned p = 0; p < VAR_PARAMS; ++p) vt[t].amount[p] = in[t * 4 + p] <= limit[p] ? in[t * 4 + p] : 0;
}
