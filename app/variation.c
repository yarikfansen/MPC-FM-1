/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "variation.h"
#include "instrument.h"
#include "seq_steps.h"
#include <string.h>

enum { TRACKS = 8, STEPS = 64, PENDING = 48, FOLLOW_BIT = 4, CHORD_HITS = 4 };
/* C2: the old per-track RATCHET (byte 2) is gone; byte 2 keeps only FOLLOW_BIT (old firmware reads 4 as 0). */
/* C3: CHANCE = OFF + 12 rhythm patterns (seq_steps.h); saved as seq_chance_store(n), loaded with seq_chance_load. */
static const uint8_t limit[VAR_PARAMS] = {100, 100, FOLLOW_BIT, SEQ_CHANCE_PATTERNS};
static struct var_track {
    uint8_t amount[VAR_PARAMS];
    uint8_t have, prev_step, length, was_playing, rebuild;
    uint64_t muted; /* C3: steps the CHANCE pattern mutes in the shown copy (ratchet() skips them) */
    struct groove_step base[STEPS], shown[STEPS];
} vt[TRACKS];
static struct { uint32_t due; uint8_t status, note, velocity; } pending[PENDING];
/* Ratchet split per step (C2, decision A 12.11): RAM only, never in the cells (the engine's project check refuses
 * bits 14-15). Lost on save / reboot / project load. Behind var_split_get / var_split_set, so a later reviewed change
 * can keep it in the extras record. */
static uint8_t split_rows[TRACKS][SEQ_SPLIT_ROW];
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
    unsigned v = var_get(t, p), pct = v;
    if (p == VAR_RATCHET) { strcpy(out, ratchet[v & 3U]); return; }
    if (p == VAR_CHANCE) { seq_chance_name(v, 1, out); return; } /* knob: OFF, HS1 .. DB3 (mockup c) */
    char *o = out;
    if (pct >= 100) *o++ = '1';
    if (pct >= 10) *o++ = (char)('0' + (pct / 10U) % 10U);
    *o++ = (char)('0' + pct % 10U); *o++ = '%'; *o = 0;
}
static void restore(unsigned t)
{
    struct var_track *v = &vt[t];
    if (!v->have) return;
    static struct groove_step cur[STEPS];
    groove_sequence_read(t, 0, cur, v->length);
    for (unsigned s = 0; s < v->length; ++s) if (same(&cur[s], &v->shown[s])) cur[s] = v->base[s];
    groove_sequence_replace(t, 0, cur, v->length);
    v->have = 0; v->muted = 0;
}
void var_edit(unsigned t, unsigned p, int d)
{
    if (t >= TRACKS || p >= VAR_PARAMS || p == VAR_RATCHET || !d) return; /* C2: no per-track RATCHET any more */
    /* C3: CHANCE walks OFF, HOUSE 1 .. DNB 3 one position per click (OFF leftmost, mockup c) */
    int n = clampi((int)vt[t].amount[p] + (p >= VAR_RATCHET ? (d > 0 ? 1 : -1) : d), 0, limit[p]);
    if ((unsigned)n == vt[t].amount[p]) return;
    vt[t].amount[p] = (uint8_t)n;
    if (p == VAR_CHANCE) { restore(t); vt[t].rebuild = 1; } /* the new pattern from the next step, not the next loop */
}
void var_restore_all(void) { for (unsigned t = 0; t < TRACKS; ++t) restore(t); }
void var_restore(unsigned t) { if (t < TRACKS) restore(t); }
unsigned var_follow(unsigned t) { return t < TRACKS && (vt[t].amount[VAR_RATCHET] & FOLLOW_BIT) != 0; }
void var_follow_set(unsigned t, unsigned on) { if (t < TRACKS) vt[t].amount[VAR_RATCHET] = on ? FOLLOW_BIT : 0; }
void var_forget(void) { for (unsigned t = 0; t < TRACKS; ++t) { vt[t].have = 0; vt[t].muted = 0; vt[t].rebuild = 0; } pending_count = 0; memset(split_rows, 0, sizeof split_rows); }
unsigned var_split_get(unsigned t, unsigned s) { return t < TRACKS ? seq_split_at(split_rows[t], s) : 0; }
void var_split_set(unsigned t, unsigned s, unsigned split) { if (t < TRACKS) seq_split_put(split_rows[t], s, split); }
void var_split_fill(unsigned t, uint64_t was, uint64_t now, unsigned split) { if (t < TRACKS) seq_split_fill(split_rows[t], was, now, split); }
void var_split_clear(unsigned t) { if (t < TRACKS) memset(split_rows[t], 0, sizeof split_rows[t]); }
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
    unsigned mask = mutate && !pads ? key_scale_mask() : 0U; /* C3: MUTATE in key; 0 = AUTO without a chord: octaves only */
    for (unsigned s = 0; s < len; ++s) {
        struct groove_step *c = &out[s];
        if (pads ? !c->drums : !c->velocity) continue;
        if (mutate && rnd(100) < mutate) {
            if (!pads) c->note = (uint8_t)seq_mutate_note(c->note, rnd(4), mask);
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
    /* C3 CHANCE: the pattern mutes steps in this copy only (the original stays in base) */
    v->muted = seq_chance_muted(out, len, v->amount[VAR_CHANCE], pads);
    for (unsigned s = 0; s < len; ++s) {
        if (!(v->muted >> s & 1U)) continue;
        if (pads) out[s].drums = (uint16_t)(out[s].drums & ~0x3fffU); else out[s].velocity = 0;
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
/* Ratchet split (C2, page 1 K2): a step with split n (1..3 = 2X..4X) plays n extra hits inside the step on every
 * pass, with gates of half a hit; melodic steps repeat their chord notes (root and up to three more). */
static void ratchet(unsigned t, unsigned step, unsigned tempo, uint32_t now)
{
    unsigned hits = var_split_get(t, step) + 1U, pads = groove_pad_count(t) != 0;
    if (hits < 2 || (step < STEPS && (vt[t].muted >> step & 1U))) return; /* C3: a step CHANCE mutes keeps its split, plays no hits */
    struct groove_step c; groove_sequence_read(t, step, &c, 1);
    if (pads ? !(c.drums & 0x3fffU) : !c.velocity) { var_split_set(t, step, 0); return; } /* the step was cleared elsewhere */
    uint8_t notes[CHORD_HITS]; unsigned count = 0;
    if (pads) {
        for (unsigned p = 0; p < groove_pad_count(t) && p < 14 && count < CHORD_HITS; ++p)
            if (c.drums >> p & 1U) notes[count++] = (uint8_t)groove_pad_note(t, p);
    } else {
        static struct groove_sequence_options o; /* control task only: off the stack */
        unsigned chord = c.drums & 31U;
        notes[count++] = c.note;
        if (chord && chord <= GROOVE_CHORDS) {
            groove_sequence_options_copy(&o);
            for (unsigned i = 1; i < o.chord[chord - 1][0] && i < 8 && count < CHORD_HITS; ++i)
                if (c.note + o.chord[chord - 1][i] < 128U) notes[count++] = (uint8_t)(c.note + o.chord[chord - 1][i]);
        }
    }
    uint32_t dur = 15000U / (tempo ? tempo : 120U), ch = groove_midi_input_channel(t) & 15U;
    for (unsigned k = 1; k < hits; ++k) {
        uint32_t at = now + dur * k / hits, off = at + dur / (2U * hits);
        for (unsigned i = 0; i < count; ++i) {
            if (!pads) queue(at - 1U, (uint8_t)(0x80 | ch), notes[i], 0);
            queue(at, (uint8_t)(0x90 | ch), notes[i], pads ? (c.velocity ? c.velocity : 100) : c.velocity);
            queue(off, (uint8_t)(0x80 | ch), notes[i], 0);
        }
    }
}
void var_tick(uint32_t now, var_post_fn post)
{
    for (unsigned t = 0; t < TRACKS; ++t) {
        struct var_track *v = &vt[t];
        struct groove_view view; memset(&view, 0, sizeof view);
        groove_get_view(t, 0, &view);
        unsigned live = v->amount[VAR_MUTATE] || v->amount[VAR_HUMAN] || v->amount[VAR_CHANCE];
        if (!view.playing) { if (v->have) restore(t); v->was_playing = 0; v->prev_step = 0; continue; }
        if (!live && v->have) restore(t);
        unsigned step = view.play_step;
        if (step != v->prev_step || !v->was_playing) {
            if (live && (step < v->prev_step || !v->was_playing || v->rebuild)) build(t, view.sequence_length);
            v->rebuild = 0;
            if (view.audible) ratchet(t, step, view.tempo, now);
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
    for (unsigned t = 0; t < TRACKS; ++t) { memcpy(out + t * 4, vt[t].amount, 4); out[t * 4 + VAR_CHANCE] = (uint8_t)seq_chance_store(vt[t].amount[VAR_CHANCE]); }
}
void var_state_put(const uint8_t in[32])
{
    var_forget();
    for (unsigned t = 0; t < TRACKS; ++t)
        for (unsigned p = 0; p < VAR_PARAMS; ++p) vt[t].amount[p] = in[t * 4 + p] <= limit[p] ? in[t * 4 + p] : 0;
    /* C2: an old project's RATCHET amount (1..3) loads as 0, so the old random ratchet stops; 4 = FOLLOW ON */
    for (unsigned t = 0; t < TRACKS; ++t) vt[t].amount[VAR_RATCHET] &= FOLLOW_BIT;
    /* C3: CHANCE = pattern n stored as 0x80 | n; an old percent level 1..7 loads as OFF */
    for (unsigned t = 0; t < TRACKS; ++t) vt[t].amount[VAR_CHANCE] = (uint8_t)seq_chance_load(in[t * 4 + VAR_CHANCE]);
}
