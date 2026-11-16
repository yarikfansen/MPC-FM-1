/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "seq_steps.h"

enum { GATE_MASK = 15U << 5, GATE_FULL = 15U << 5 };

unsigned seq_len_tails(const struct groove_step *c, unsigned len, unsigned s)
{
    if (!len || len > SEQ_CELLS || s >= len || !c[s].velocity) return 0;
    unsigned k = 0, at = s;
    while (k < SEQ_LEN_MAX_TAILS && (c[at].drums & SEQ_TIE)) {
        unsigned next = (at + 1U) % len;
        if (next == s || !c[next].velocity || c[next].note != c[s].note) break;
        ++k; at = next;
    }
    return k;
}
unsigned seq_len_pos(const struct groove_step *c, unsigned len, unsigned s)
{
    if (!len || len > SEQ_CELLS || s >= len) return 7;
    if (c[s].drums & SEQ_TIE) { unsigned k = seq_len_tails(c, len, s); return SEQ_LEN_FULL + (k ? k : 1U); }
    unsigned g = (c[s].drums >> 5) & 15U;
    return (g ? g : 8U) - 1U;
}
unsigned seq_len_set(struct groove_step *c, unsigned len, unsigned s, unsigned pos)
{
    if (!len || len > SEQ_CELLS || s >= len || !c[s].velocity) return seq_len_pos(c, len, s);
    unsigned have = seq_len_tails(c, len, s), want = pos > SEQ_LEN_FULL ? pos - SEQ_LEN_FULL : 0;
    if (want > SEQ_LEN_MAX_TAILS) want = SEQ_LEN_MAX_TAILS;
    if (want > len - 1U) want = len - 1U;
    /* grow: the next step must be free (a tail never overwrites a lit step, nor the note's own head) */
    unsigned k = want < have ? want : have;
    while (k < want) {
        unsigned at = (s + k + 1U) % len;
        if (at == s || c[at].velocity) break;
        ++k;
    }
    /* shorten: the removed tail steps go empty (velocity 0, tie off) */
    for (unsigned j = k + 1U; j <= have; ++j) {
        struct groove_step *t = &c[(s + j) % len];
        t->velocity = 0; t->drums = (uint16_t)(t->drums & ~SEQ_TIE);
    }
    if (!k) {
        c[s].drums = (uint16_t)(c[s].drums & ~SEQ_TIE);
        if (pos > SEQ_LEN_FULL) pos = SEQ_LEN_FULL;
        c[s].drums = (uint16_t)((c[s].drums & ~GATE_MASK) | ((pos + 1U) << 5));
        return pos;
    }
    c[s].drums = (uint16_t)((c[s].drums & ~GATE_MASK) | GATE_FULL | SEQ_TIE);
    uint16_t keep = (uint16_t)(c[s].drums & (31U | GROOVE_STEP_ALT)); /* chord and alt tuning go with the note; never 14-15 */
    for (unsigned j = 1; j <= k; ++j) {
        struct groove_step *t = &c[(s + j) % len];
        t->note = c[s].note; t->velocity = c[s].velocity;
        t->drums = (uint16_t)(keep | GATE_FULL | (j < k ? SEQ_TIE : 0U));
    }
    return SEQ_LEN_FULL + k;
}
uint64_t seq_fill(const struct groove_step *snap, struct groove_step *out, unsigned len, unsigned cur,
                  unsigned space, unsigned repeat, int pad)
{
    unsigned n = len < SEQ_CELLS ? len : SEQ_CELLS;
    for (unsigned i = 0; i < SEQ_CELLS; ++i) out[i] = snap[i];
    if (!n || cur >= n || !space || !seq_lit(&snap[cur], pad)) return 0;
    uint64_t placed = 0;
    const struct groove_step src = snap[cur];
    unsigned vel = src.velocity;
    for (unsigned j = 1; j <= repeat; ++j) {
        if (vel) vel = seq_fade(vel); /* tap j, counted even when the tap is skipped */
        unsigned at = (unsigned)((cur + (uint64_t)space * j) % n);
        struct groove_step *t = &out[at];
        if (seq_lit(t, pad)) continue; /* a lit step (or an earlier copy) stays as it is */
        if (pad < 0) { *t = src; t->drums = (uint16_t)(t->drums & ~(SEQ_TIE | SEQ_SPLIT_MASK)); t->velocity = (uint8_t)vel; }
        else {
            if (!t->drums) { t->note = src.note; t->velocity = (uint8_t)vel; }
            t->drums = (uint16_t)(t->drums | (1U << (unsigned)pad));
        }
        placed |= 1ULL << at;
    }
    return placed;
}
int seq_transpose(struct groove_step *c, unsigned count, int semis, const struct groove_sequence_options *o)
{
    if (count > SEQ_CELLS) count = SEQ_CELLS;
    for (unsigned i = 0; i < count; ++i) {
        if (!c[i].velocity) continue;
        unsigned chord = c[i].drums & 31U, top = 0;
        if (chord && chord <= GROOVE_CHORDS && o->chord[chord - 1][0] > 1) top = o->chord[chord - 1][o->chord[chord - 1][0] - 1];
        int root = (int)c[i].note + semis;
        if (root < 0 || root + (int)top > 127) return -1;
    }
    for (unsigned i = 0; i < count; ++i) if (c[i].velocity) c[i].note = (uint8_t)((int)c[i].note + semis);
    return 0;
}
/* C3 CHANCE patterns, exactly as drafted and approved 12.11 (docs/coord-notes/seq-rework-1211.md) */
const char seq_chance_masks[SEQ_CHANCE_PATTERNS][17] = {
    "..x...x...x...x.", "..x...xx..x...xx", "x..x..x...x..x..", /* HOUSE 1-3 */
    "x.xxx.xxx.xxx.xx", ".x.x.x.x.x.x.x.x", "x..x..x.x..x..x.", /* TECHNO 1-3 */
    ".xxx.xxx.xxx.xxx", "..xx..xx..xx..xx", "x.xx.xx.xx.xx.x.", /* TRANCE 1-3 */
    "x.....x...x..x..", "x..x...x..x.....", "x......x.x....x.", /* DNB 1-3 */
};
void seq_chance_name(unsigned pattern, unsigned short_form, char out[12])
{
    static const char *const style[4] = {"HOUSE ", "TECHNO ", "TRANCE ", "DNB "}, *const brief[4] = {"HS", "TC", "TR", "DB"};
    if (!pattern || pattern > SEQ_CHANCE_PATTERNS) { out[0] = 'O'; out[1] = 'F'; out[2] = 'F'; out[3] = 0; return; }
    const char *s = short_form ? brief[(pattern - 1U) / 3U] : style[(pattern - 1U) / 3U];
    unsigned i = 0;
    while (s[i]) { out[i] = s[i]; ++i; }
    out[i++] = (char)('1' + (pattern - 1U) % 3U); out[i] = 0;
}
uint64_t seq_chance_muted(const struct groove_step *c, unsigned len, unsigned pattern, unsigned pads)
{
    if (len > SEQ_CELLS) len = SEQ_CELLS;
    if (!len || !pattern || pattern > SEQ_CHANCE_PATTERNS) return 0;
    uint64_t muted = 0, tail = 0;
    if (!pads) /* tail = tied in from the step before (wrapping, as long notes do) with the same note */
        for (unsigned s = 0; s < len; ++s) {
            const struct groove_step *p = &c[(s + len - 1U) % len];
            if (len > 1 && c[s].velocity && p->velocity && (p->drums & SEQ_TIE) && p->note == c[s].note) tail |= 1ULL << s;
        }
    for (unsigned s = 0; s < len; ++s) {
        unsigned lit = pads ? (c[s].drums & 0x3fffU) != 0 : c[s].velocity != 0;
        if (!lit || (tail >> s & 1U) || seq_chance_plays(pattern, s)) continue;
        muted |= 1ULL << s;
        for (unsigned k = 1, at = s; k < len; ++k) { /* the muted head's tail goes with it */
            if (!(c[at].drums & SEQ_TIE)) break;
            at = (at + 1U) % len;
            if (!(tail >> at & 1U)) break;
            muted |= 1ULL << at;
        }
    }
    return muted;
}
