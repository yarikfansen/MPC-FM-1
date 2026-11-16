/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "keyscale.h"

static uint8_t key = 0, mode = 0;              /* GLOBAL KEY / MODE */
static uint8_t auto_valid, auto_root, auto_minor; /* AUTO: the first chord */

unsigned key_get(void) { return key; }
unsigned key_mode_get(void) { return mode; }
void key_edit(unsigned param, int delta)
{
    if (!delta) return;
    if (param == 0) {
        int v = (int)key + delta;
        key = (uint8_t)(v < 0 ? 0 : v > 12 ? 12 : v);
    } else if (param == 1 && key) mode = delta > 0;
}
void key_auto_reset(void) { auto_valid = 0; }
unsigned key_state_get(void) { return (unsigned)key | (unsigned)mode << 4; }
void key_state_put(unsigned stored)
{
    if (stored > 0xffU || stored == 0xffU || (stored & 15U) > 12U || (stored >> 5)) { key = 0; mode = 0; return; }
    key = (uint8_t)(stored & 15U);
    mode = (uint8_t)((stored >> 4) & 1U);
}
static unsigned rotate(unsigned mask, unsigned root) /* pitch classes relative to root */
{
    root %= 12U;
    return ((mask >> root) | (mask << (12U - root))) & 0xfffU;
}
unsigned key_scale_mask(void)
{
    static const uint16_t scales[2] = {0xab5 /* 0 2 4 5 7 9 11 */, 0x5ad /* 0 2 3 5 7 8 10 */};
    unsigned root, minor;
    if (key) { root = key - 1U; minor = mode; }
    else if (auto_valid) { root = auto_root; minor = auto_minor; }
    else return 0;
    return rotate(scales[minor], 12U - root); /* scale degrees moved up to the root */
}
void key_chord_seen(const uint8_t *notes, unsigned count)
{
    if (auto_valid || !notes) return;
    char name[8]; unsigned root, minor;
    if (chord_name(notes, count, name, &root, &minor) && minor < 2U) { /* a third tells MAJ / MIN */ auto_root = (uint8_t)root; auto_minor = (uint8_t)minor; auto_valid = 1; }
}

/* Chords as pitch-class masks relative to the root (bit 0 = root). */
#define PC(a) (1U << (a))
static const struct { uint16_t mask; char suffix[6]; uint8_t minor; /* 0 major, 1 minor third, 2 none */ } chords[] = {
    {PC(0) | PC(4) | PC(7), "", 0},
    {PC(0) | PC(3) | PC(7), "m", 1},
    {PC(0) | PC(4) | PC(7) | PC(10), "7", 0},
    {PC(0) | PC(4) | PC(7) | PC(11), "maj7", 0},
    {PC(0) | PC(3) | PC(7) | PC(10), "m7", 1},
    {PC(0) | PC(4) | PC(10), "7", 0},           /* no fifth */
    {PC(0) | PC(4) | PC(11), "maj7", 0},
    {PC(0) | PC(3) | PC(10), "m7", 1},
    {PC(0) | PC(3) | PC(6), "dim", 1},
    {PC(0) | PC(3) | PC(6) | PC(9), "dim7", 1},
    {PC(0) | PC(3) | PC(6) | PC(10), "m7b5", 1},
    {PC(0) | PC(4) | PC(8), "aug", 0},
    {PC(0) | PC(2) | PC(7), "sus2", 2},
    {PC(0) | PC(5) | PC(7), "sus4", 2},
    {PC(0) | PC(5) | PC(7) | PC(10), "7sus", 2},
    {PC(0) | PC(7), "5", 2},
    {PC(0) | PC(2) | PC(4) | PC(7), "add9", 0},
    {PC(0) | PC(2) | PC(3) | PC(7), "madd9", 1},
    {PC(0) | PC(4) | PC(7) | PC(9), "6", 0},
    {PC(0) | PC(3) | PC(7) | PC(9), "m6", 1},
    {PC(0) | PC(2) | PC(4) | PC(7) | PC(10), "9", 0},
    {PC(0) | PC(2) | PC(4) | PC(7) | PC(11), "maj9", 0},
    {PC(0) | PC(2) | PC(3) | PC(7) | PC(10), "m9", 1},
};
static unsigned put_root(char *out, unsigned pc)
{
    static const char names[] = "CCDDEFFGGAAB";
    out[0] = names[pc];
    if ((0x54aU >> pc) & 1U) { out[1] = '#'; return 2; } /* C# D# F# G# A# */
    return 1;
}
unsigned chord_name(const uint8_t *notes, unsigned count, char out[8], unsigned *root, unsigned *minor)
{
    out[0] = 0;
    if (!notes || !count) return 0;
    if (count > 8) count = 8;
    unsigned mask = 0, low = 255;
    for (unsigned i = 0; i < count; ++i) {
        if (notes[i] > 127) continue;
        mask |= 1U << (notes[i] % 12U);
        if (notes[i] < low) low = notes[i];
    }
    if (low > 127) return 0;
    unsigned bass = low % 12U;
    if (!(mask & (mask - 1U))) { /* one pitch class: the note with its octave */
        unsigned n = put_root(out, bass);
        int oct = (int)(low / 12U) - 1;
        if (oct < 0) { out[n++] = '-'; oct = -oct; }
        out[n++] = (char)('0' + oct);
        out[n] = 0;
        return 0;
    }
    /* The bass note as the root first (root position, C6 rather than Am7/C),
     * then the other pitch classes upwards from it. */
    for (unsigned k = 0; k < 12; ++k) {
        unsigned r = (bass + k) % 12U;
        if (!(mask & (1U << r))) continue;
        unsigned rel = rotate(mask, r);
        for (unsigned c = 0; c < sizeof chords / sizeof chords[0]; ++c)
            if (rel == chords[c].mask) {
                unsigned n = put_root(out, r);
                for (unsigned i = 0; chords[c].suffix[i] && i < 5; ++i) out[n++] = chords[c].suffix[i];
                out[n] = 0;
                if (root) *root = r;
                if (minor) *minor = chords[c].minor;
                return 1;
            }
    }
    out[0] = '-'; out[1] = '-'; out[2] = 0;
    return 0;
}
