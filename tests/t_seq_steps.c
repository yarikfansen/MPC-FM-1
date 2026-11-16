/* SEQ rework C2 step logic host check (app/seq_steps.c): LENGTH tails (grow, stop before a lit step, wrap, shrink),
 * ratchet fill (skip lit steps, wrap, drums, re-apply from the snapshot = turning down removes copies), the RAM split
 * table (copies carry it, removed copies lose it), no path of ours ever writes cell bits 14-15 (the engine refuses
 * them), every other bit kept, transpose range refusal. Not covered: screen.c drawing and knobs, the engine (replace,
 * checkpoint / undo, save / load / copy of bits 14-15: device only). */
#include "seq_steps.h"

void *memset(void *d, int c, unsigned long long n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned long long n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)

static struct groove_step cells[SEQ_CELLS], snap[SEQ_CELLS], out[SEQ_CELLS];
static void clear(struct groove_step *c) { memset(c, 0, sizeof(struct groove_step) * SEQ_CELLS); }
static void note(struct groove_step *c, unsigned s, unsigned n, unsigned v, unsigned bits) { c[s].note = (uint8_t)n; c[s].velocity = (uint8_t)v; c[s].drums = (uint16_t)bits; }
static unsigned gate(const struct groove_step *c) { return (c->drums >> 5) & 15U; }

static int length_checks(void)
{
    clear(cells);
    note(cells, 3, 50, 90, 2U | (5U << 9) | GROOVE_STEP_ALT); /* chord 2, prob 5, alt, default gate */
    note(cells, 8, 60, 100, 0);
    CHECK(seq_len_pos(cells, 16, 3) == 7);               /* gate 0 = 8/16 */
    CHECK(seq_len_set(cells, 16, 3, 0) == 0 && gate(&cells[3]) == 1);
    CHECK(seq_len_set(cells, 16, 3, 14) == 14 && gate(&cells[3]) == 15 && !(cells[3].drums & SEQ_TIE));
    /* three tail steps: 4, 5, 6 */
    CHECK(seq_len_set(cells, 16, 3, 17) == 17);
    CHECK(seq_len_tails(cells, 16, 3) == 3 && seq_len_pos(cells, 16, 3) == 17);
    CHECK(cells[3].drums & SEQ_TIE && cells[4].drums & SEQ_TIE && cells[5].drums & SEQ_TIE && !(cells[6].drums & SEQ_TIE));
    for (unsigned s = 4; s <= 6; ++s) CHECK(cells[s].note == 50 && cells[s].velocity == 90 && (cells[s].drums & 31U) == 2 && gate(&cells[s]) == 15);
    CHECK((cells[3].drums & GROOVE_STEP_ALT) && ((cells[3].drums >> 9) & 7U) == 5 && (cells[3].drums & 31U) == 2); /* own bits kept */
    for (unsigned s = 0; s < SEQ_CELLS; ++s) CHECK(!(cells[s].drums & SEQ_SPLIT_MASK));                         /* never 14-15 */
    /* the tail stops before the next lit step (8): at most 4 tails (4..7), never overwrites */
    CHECK(seq_len_set(cells, 16, 3, 30) == 18);
    CHECK(cells[8].note == 60 && cells[8].velocity == 100 && !cells[8].drums && cells[7].note == 50 && !(cells[7].drums & SEQ_TIE));
    /* back: the last tail goes, the cells after it are empty */
    CHECK(seq_len_set(cells, 16, 3, 16) == 16 && seq_len_tails(cells, 16, 3) == 2);
    CHECK(!cells[6].velocity && !cells[7].velocity && !(cells[5].drums & SEQ_TIE) && cells[4].drums & SEQ_TIE);
    CHECK(seq_len_set(cells, 16, 3, 13) == 13 && !cells[4].velocity && !cells[5].velocity && !(cells[3].drums & SEQ_TIE) && gate(&cells[3]) == 14);
    /* wrap at the sequence end: step 14 of 16 grows into 15, 0, 1; step 2 is lit, so 15, 0, 1 is the most */
    clear(cells); note(cells, 14, 40, 80, 0); note(cells, 2, 41, 80, 0);
    CHECK(seq_len_set(cells, 16, 14, 14 + 9) == 17);
    CHECK(cells[15].note == 40 && cells[0].note == 40 && cells[1].note == 40 && !(cells[1].drums & SEQ_TIE) && cells[2].note == 41);
    /* a full loop never reaches its own head; on 16 steps 15 tails is the most */
    clear(cells); note(cells, 0, 30, 70, 0);
    CHECK(seq_len_set(cells, 16, 0, 40) == 14 + 15 && !(cells[15].drums & SEQ_TIE) && cells[0].note == 30 && cells[0].drums & SEQ_TIE);
    CHECK(seq_len_set(cells, 64, 0, 40) == 40 && seq_len_tails(cells, 64, 0) == 26); /* C3: on 64 steps past the old 15 */
    /* an empty step does nothing; a bare tie reads as 15 */
    clear(cells);
    CHECK(seq_len_set(cells, 16, 5, 20) == 7 && !cells[6].velocity);
    note(cells, 5, 60, 100, SEQ_TIE);
    CHECK(seq_len_pos(cells, 16, 5) == 15 && seq_len_tails(cells, 16, 5) == 0);
    return 0;
}
static int fill_checks(void)
{
    /* melodic: cursor 2, space 2, repeat 3 -> 4, 6, 8; 6 is lit and stays */
    clear(snap);
    note(snap, 2, 62, 99, 3U | (9U << 5) | (4U << 9) | SEQ_TIE | GROOVE_STEP_ALT | (2U << SEQ_SPLIT_SHIFT)); /* 14-15 set only to prove they are dropped */
    note(snap, 6, 70, 50, 0);
    uint64_t m = seq_fill(snap, out, 16, 2, 2, 3, -1);
    CHECK(m == ((1ULL << 4) | (1ULL << 8)));
    CHECK(out[4].note == 62 && out[4].velocity == 74 && out[8].velocity == 41 && out[2].velocity == 99 && out[4].drums == (snap[2].drums & ~(SEQ_TIE | SEQ_SPLIT_MASK))); /* chord, gate, prob, alt; no tie, no 14-15 */
    CHECK(!(out[8].drums & SEQ_SPLIT_MASK) && out[6].note == 70 && out[6].velocity == 50 && !out[6].drums);
    CHECK(!snap[4].velocity); /* the snapshot itself is not touched */
    /* wrap: cursor 12, space 3, repeat 3 on 16 steps -> 15, 2, 5 */
    clear(snap); note(snap, 12, 48, 100, 0);
    CHECK(seq_fill(snap, out, 16, 12, 3, 3, -1) == ((1ULL << 15) | (1ULL << 2) | (1ULL << 5)));
    /* copies landing on each other or on the cursor are skipped: space 8, repeat 5 on 16 -> 4 only */
    clear(snap); note(snap, 12, 48, 100, 0);
    CHECK(seq_fill(snap, out, 16, 12, 8, 5, -1) == (1ULL << 4));
    /* snapshot re-apply: REPEAT 3 then 1 leaves one copy (turning down removes copies) */
    clear(snap); note(snap, 0, 60, 100, 0);
    CHECK(seq_fill(snap, out, 16, 0, 4, 3, -1) == ((1ULL << 4) | (1ULL << 8) | (1ULL << 12)));
    CHECK(seq_fill(snap, out, 16, 0, 4, 1, -1) == (1ULL << 4) && !out[8].velocity && !out[12].velocity);
    CHECK(seq_fill(snap, out, 16, 0, 4, 0, -1) == 0 && !out[4].velocity);
    /* fade like a delay: each tap 3/4 of the one before, floor 8, the source unchanged; a skipped tap still counts */
    clear(snap); note(snap, 0, 60, 100, 0);
    (void)seq_fill(snap, out, 16, 0, 1, 12, -1);
    { static const uint8_t want[13] = {100, 75, 56, 42, 31, 23, 17, 12, 9, 8, 8, 8, 8};
      for (unsigned j = 0; j < 13; ++j) CHECK(out[j].velocity == want[j]); }
    CHECK(seq_fade(9) == 8 && seq_fade(8) == 8 && seq_fade(127) == 95);
    note(snap, 1, 70, 20, 0); /* tap 1 lit: skipped, tap 2 still gets 56 */
    (void)seq_fill(snap, out, 16, 0, 1, 2, -1);
    CHECK(out[1].velocity == 20 && out[2].velocity == 56);
    /* drums: a cursor cell without its own velocity (0 = the engine's 100) gives equal copies */
    clear(snap); snap[0].drums = 1U << 3;
    (void)seq_fill(snap, out, 16, 0, 4, 2, 3);
    CHECK(out[4].drums == (1U << 3) && !out[4].velocity && !out[8].velocity);
    /* empty cursor, space 0: nothing */
    clear(snap);
    CHECK(seq_fill(snap, out, 16, 0, 2, 3, -1) == 0);
    note(snap, 0, 60, 100, 0);
    CHECK(seq_fill(snap, out, 16, 0, 0, 3, -1) == 0);
    /* drums: pad 2 set on the cursor; step 4 already has pad 2 (skipped), step 8 has pad 0 only (bit added, its
     * probability / velocity kept), step 12 empty (takes the cursor's probability, velocity and split) */
    clear(snap);
    snap[0].note = 0x30; snap[0].velocity = 90; snap[0].drums = (uint16_t)((1U << 2) | (1U << 5));
    snap[4].drums = 1U << 2; snap[4].note = 0x10;
    snap[8].drums = 1U << 0; snap[8].note = 0x20; snap[8].velocity = 70;
    m = seq_fill(snap, out, 16, 0, 4, 3, 2);
    CHECK(m == ((1ULL << 8) | (1ULL << 12)));
    CHECK(out[4].drums == (1U << 2) && out[4].note == 0x10);
    CHECK(out[8].drums == ((1U << 0) | (1U << 2)) && out[8].note == 0x20 && out[8].velocity == 70);
    CHECK(out[12].drums == (1U << 2) && out[12].note == 0x30 && out[12].velocity == 37); /* tap 3: 90 -> 67 -> 50 -> 37 */
    /* drums: the cursor pad must be set on the cursor step */
    CHECK(seq_fill(snap, out, 16, 0, 4, 3, 1) == 0);
    return 0;
}
static int split_and_transpose_checks(void)
{
    /* the RAM split table: 2 bits per step, neighbours untouched; a fill moves the split with its copies */
    uint8_t row[SEQ_SPLIT_ROW]; memset(row, 0, sizeof row);
    for (unsigned s = 0; s < SEQ_CELLS; ++s) seq_split_put(row, s, s % 4U);
    for (unsigned s = 0; s < SEQ_CELLS; ++s) CHECK(seq_split_at(row, s) == s % 4U);
    seq_split_put(row, 64, 3); CHECK(seq_split_at(row, 64) == 0 && seq_split_at(row, 63) == 3);
    memset(row, 0, sizeof row); seq_split_put(row, 0, 2);
    seq_split_fill(row, 0, (1ULL << 4) | (1ULL << 8) | (1ULL << 12), 2);        /* REPEAT 3 */
    CHECK(seq_split_at(row, 4) == 2 && seq_split_at(row, 8) == 2 && seq_split_at(row, 12) == 2 && seq_split_at(row, 0) == 2);
    seq_split_fill(row, (1ULL << 4) | (1ULL << 8) | (1ULL << 12), 1ULL << 4, 2); /* REPEAT 1: the removed copies lose it */
    CHECK(seq_split_at(row, 4) == 2 && !seq_split_at(row, 8) && !seq_split_at(row, 12) && seq_split_at(row, 0) == 2);
    seq_split_fill(row, 1ULL << 4, 1ULL << 4, 3);                                /* K2 during a fill */
    CHECK(seq_split_at(row, 4) == 3);
    /* transpose keeps every bit but the note, moves only lit cells, refuses out of range (no clamp) */
    struct groove_sequence_options o; memset(&o, 0, sizeof o);
    o.chord[0][0] = 3; o.chord[0][1] = 4; o.chord[0][2] = 7; /* chord 1: root, +4, +7 */
    clear(cells);
    note(cells, 0, 60, 100, 1U | GROOVE_STEP_ALT | (6U << 9));
    note(cells, 1, 100, 0, 0);            /* unlit: untouched */
    note(cells, 40, 10, 80, 0);           /* beyond a 16-step length: still moved */
    CHECK(seq_transpose(cells, SEQ_CELLS, 5, &o) == 0 && cells[0].note == 65 && cells[40].note == 15 && cells[1].note == 100);
    CHECK(cells[0].drums == (1U | GROOVE_STEP_ALT | (6U << 9)));
    CHECK(seq_transpose(cells, SEQ_CELLS, 56, &o) == -1 && cells[0].note == 65); /* 121 + 7 > 127: chord top */
    CHECK(seq_transpose(cells, SEQ_CELLS, 55, &o) == 0 && cells[0].note == 120);  /* 120 + 7 = 127 fits */
    CHECK(seq_transpose(cells, SEQ_CELLS, -71, &o) == -1 && cells[40].note == 70); /* 70 - 71 < 0: refused */
    CHECK(seq_transpose(cells, SEQ_CELLS, -70, &o) == 0 && cells[40].note == 0 && cells[0].note == 50);
    /* follow: nearest direction from C */
    CHECK(seq_follow_shift(60) == 0 && seq_follow_shift(62) == 2 && seq_follow_shift(66) == 6 && seq_follow_shift(67) == -5 && seq_follow_shift(71) == -1);
    return 0;
}
/* C3: MUTATE in key, the 12 CHANCE masks, muting with long notes, old CHANCE bytes, 63-step tails, REC long notes */
static int c3_checks(void)
{
    /* key_snap: D major = D E F# G A B C# (bits 2 4 6 7 9 11 1) */
    const unsigned dmaj = (1U << 2) | (1U << 4) | (1U << 6) | (1U << 7) | (1U << 9) | (1U << 11) | (1U << 1);
    CHECK(key_snap(62, dmaj) == 62);                       /* D stays */
    CHECK(key_snap(60, dmaj) == 61);                       /* C -> C# (up first: B and C# are both 1 away) */
    CHECK(key_snap(65, dmaj) == 66);                       /* F -> F# */
    CHECK(key_snap(63, dmaj) == 64);                       /* D# -> E (up first, D also 1 away) */
    CHECK(key_snap(65, 0) == 65 && key_snap(127, 1U) == 120 && key_snap(0, 1U << 11) == 11); /* mask 0: unchanged; never past 0..127 */
    /* seq_mutate_note with a fixed key: every jump lands in the scale */
    const unsigned cmaj = 0xab5U;
    for (int n = 0; n < 128; ++n)
        for (unsigned pick = 0; pick < 4; ++pick) {
            int m = seq_mutate_note(n, pick, dmaj), c = seq_mutate_note(n, pick, cmaj);
            CHECK(m >= 0 && m <= 127 && (dmaj >> (m % 12) & 1U) && c >= 0 && c <= 127 && (cmaj >> (c % 12) & 1U));
        }
    CHECK(seq_mutate_note(60, 2, cmaj) == 67 && seq_mutate_note(60, 3, cmaj) == 55); /* fifths in key stay */
    CHECK(seq_mutate_note(62, 2, dmaj) == 69 && seq_mutate_note(64, 2, cmaj) == 71); /* E + 7 = B in C */
    CHECK(seq_mutate_note(65, 3, cmaj) == 60);             /* F - 5 = C */
    CHECK(seq_mutate_note(66, 2, cmaj) == 74);             /* F# + 7 = C#, not in C -> D */
    /* mask 0 (AUTO, no chord yet): octave jumps only */
    for (unsigned pick = 0; pick < 4; ++pick) {
        int m = seq_mutate_note(61, pick, 0);
        CHECK(m == 73 || m == 49);
    }
    CHECK(seq_mutate_note(61, 2, 0) == 73 && seq_mutate_note(61, 3, 0) == 49 && seq_mutate_note(120, 0, 0) == 127);
    /* the 12 masks as approved (typed again here on purpose), their names, OFF = all play */
    static const char *const want[12] = {"..x...x...x...x.", "..x...xx..x...xx", "x..x..x...x..x..",
        "x.xxx.xxx.xxx.xx", ".x.x.x.x.x.x.x.x", "x..x..x.x..x..x.", ".xxx.xxx.xxx.xxx", "..xx..xx..xx..xx",
        "x.xx.xx.xx.xx.x.", "x.....x...x..x..", "x..x...x..x.....", "x......x.x....x."};
    static const char *const names[12] = {"HOUSE 1", "HOUSE 2", "HOUSE 3", "TECHNO 1", "TECHNO 2", "TECHNO 3",
        "TRANCE 1", "TRANCE 2", "TRANCE 3", "DNB 1", "DNB 2", "DNB 3"};
    static const char *const briefs[12] = {"HS1", "HS2", "HS3", "TC1", "TC2", "TC3", "TR1", "TR2", "TR3", "DB1", "DB2", "DB3"};
    char nm[12];
    for (unsigned p = 1; p <= 12; ++p) {
        for (unsigned st = 0; st < 64; ++st) CHECK(seq_chance_plays(p, st) == (want[p - 1][st % 16] == 'x')); /* repeats */
        seq_chance_name(p, 0, nm); for (unsigned i = 0; ; ++i) { CHECK(nm[i] == names[p - 1][i]); if (!nm[i]) break; }
        seq_chance_name(p, 1, nm); for (unsigned i = 0; ; ++i) { CHECK(nm[i] == briefs[p - 1][i]); if (!nm[i]) break; }
        /* a full melodic track of single steps: muted = exactly the '.' steps, over a 64-step sequence */
        clear(cells);
        for (unsigned st = 0; st < 64; ++st) note(cells, st, 60, 100, 15U << 5);
        uint64_t m = seq_chance_muted(cells, 64, p, 0);
        for (unsigned st = 0; st < 64; ++st) CHECK((unsigned)(m >> st & 1U) == (want[p - 1][st % 16] != 'x'));
        /* drums: only cells holding pads count */
        clear(cells); cells[1].drums = 1; cells[2].drums = 4; cells[5].drums = 2;
        m = seq_chance_muted(cells, 16, p, 1);
        CHECK((m & ~((1ULL << 1) | (1ULL << 2) | (1ULL << 5))) == 0);
        for (unsigned st = 0; st < 16; ++st) if (st == 1 || st == 2 || st == 5) CHECK((unsigned)(m >> st & 1U) == (want[p - 1][st] != 'x'));
    }
    for (unsigned st = 0; st < 64; ++st) CHECK(seq_chance_plays(0, st));
    seq_chance_name(0, 0, nm); CHECK(nm[0] == 'O' && nm[1] == 'F' && nm[2] == 'F' && !nm[3]);
    CHECK(seq_chance_muted(cells, 16, 0, 0) == 0);
    /* long notes follow their head: HOUSE 1 plays steps 2, 6, 10, 14 */
    clear(cells);
    note(cells, 2, 50, 90, 0); CHECK(seq_len_set(cells, 16, 2, 17) == 17);   /* head 2 (plays), tail 3..5 (muted steps) */
    note(cells, 7, 52, 90, 0); CHECK(seq_len_set(cells, 16, 7, 16) == 16);   /* head 7 (muted), tail 8..9 (8 muted, 9 muted) */
    note(cells, 11, 55, 90, 0); CHECK(seq_len_set(cells, 16, 11, 15) == 15); /* head 11 (muted), tail 12 */
    uint64_t m = seq_chance_muted(cells, 16, 1, 0);
    CHECK(m == ((1ULL << 7) | (1ULL << 8) | (1ULL << 9) | (1ULL << 11) | (1ULL << 12)));
    /* saved byte: OFF 0, pattern n = 0x80 | n; old percent levels 1..7 load as OFF; junk loads as OFF */
    for (unsigned p = 0; p <= 12; ++p) CHECK(seq_chance_load(seq_chance_store(p)) == p);
    for (unsigned old = 0; old <= 7; ++old) CHECK(seq_chance_load(old) == 0);
    CHECK(seq_chance_load(0x80) == 0 && seq_chance_load(0x8d) == 0 && seq_chance_load(0xff) == 0 && seq_chance_store(13) == 0);
    /* 63-step tail: a lone note in a 64-step sequence grows over the whole sequence and stops before its head */
    clear(cells); note(cells, 5, 48, 80, 0);
    CHECK(seq_len_set(cells, 64, 5, SEQ_LEN_FULL + 63) == SEQ_LEN_FULL + 63);
    CHECK(seq_len_tails(cells, 64, 5) == 63 && seq_len_pos(cells, 64, 5) == SEQ_LEN_FULL + 63);
    CHECK(!(cells[4].drums & SEQ_TIE) && cells[4].velocity == 80 && (cells[3].drums & SEQ_TIE));    /* last tail = step 4 */
    CHECK(seq_len_set(cells, 64, 5, SEQ_LEN_FULL + 70) == SEQ_LEN_FULL + 63);                       /* never past len - 1 */
    CHECK(seq_len_set(cells, 64, 5, SEQ_LEN_FULL + 20) == SEQ_LEN_FULL + 20 && cells[26].velocity == 0 && !(cells[25].drums & SEQ_TIE));
    clear(cells); note(cells, 0, 48, 80, 0); note(cells, 40, 50, 80, 0);
    CHECK(seq_len_set(cells, 64, 0, SEQ_LEN_FULL + 63) == SEQ_LEN_FULL + 39);                        /* stops before step 40 */
    /* live REC: steps covered = ceil(len16 / 16), at most the sequence length (was 16) */
    CHECK(seq_rec_steps(17, 64) == 2 && seq_rec_steps(16 * 40, 64) == 40 && seq_rec_steps(16 * 63 + 1, 64) == 64);
    CHECK(seq_rec_steps(16 * 100, 64) == 64 && seq_rec_steps(16 * 20, 8) == 8 && seq_rec_steps(16 * 20, 32) == 20);
    return 0;
}
int main(void)
{
    int r = length_checks();
    if (!r) r = fill_checks();
    if (!r) r = split_and_transpose_checks();
    if (!r) r = c3_checks();
    return r;
}
