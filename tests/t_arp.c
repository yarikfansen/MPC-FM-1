/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Host check of app/arp.c: plain timing, v13 swing, polyrhythm / mutate ranges, stored state. */
#include "arp.h"
#include <stdio.h>
#include <string.h>

void arp_state_get(uint8_t out[56]);
void arp_state_put(const uint8_t in[56]);

static unsigned failures;
#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Runs the arpeggiator on 10 ms ticks over three held notes; records note-on times (ms after the first tick). */
struct run { unsigned n, t[256]; uint8_t note[256]; unsigned doubled; };
static void run_ticks(struct run *r, uint32_t *now, unsigned ticks, unsigned tempo)
{
    static const uint8_t held[3] = {60, 64, 67};
    struct arp_event ev[6];
    for (unsigned i = 0; i < ticks; ++i) {
        *now += 10;
        unsigned m = arp_tick(0, 0, *now, tempo, 1, held, 3, ev, 6), ons = 0;
        for (unsigned k = 0; k < m; ++k)
            if ((ev[k].status & 0xf0) == 0x90 && r->n < 256) { r->t[r->n] = *now; r->note[r->n++] = ev[k].note; ++ons; }
        if (ons > 1) ++r->doubled;
    }
}
static void reset(unsigned rate, unsigned swing)
{
    arp_set(0, 0);
    arp_edit(0, ARP_RATE, (int)rate - (int)arp_get(0, ARP_RATE));
    arp_edit(0, ARP_MODE, -10); arp_edit(0, ARP_OCT, -10); arp_edit(0, ARP_HUMAN, -100); arp_edit(0, ARP_HOLD, -1);
    arp_edit(0, ARP_MUTATE, -100); arp_edit(0, ARP_VAR, -10); arp_edit(0, ARP_CHANCE, -10);
    arp_swing_set(swing);
    arp_set(0, 1);
}
static unsigned abs_diff(unsigned a, unsigned b) { return a > b ? a - b : b - a; }

int main(void)
{
    /* swing 0: 120 BPM, 1/16 = 125 ms per note, the first note on the first tick */
    struct run r; uint32_t now = 1000;
    memset(&r, 0, sizeof r); reset(3, 0);
    run_ticks(&r, &now, 600, 120);
    CHECK(r.n >= 40, "swing 0 notes %u", r.n);
    for (unsigned k = 1; k < r.n && k < 40; ++k) {
        unsigned ideal = 125U * k, got = r.t[k] - r.t[0];
        CHECK(got >= ideal && got < ideal + 10U, "swing 0 note %u at %u, ideal %u", k, got, ideal);
        CHECK(r.note[k] == (k % 3 == 0 ? 60 : k % 3 == 1 ? 64 : 67), "swing 0 note %u is %u", k, r.note[k]);
    }
    CHECK(!r.doubled, "swing 0 doubled notes");

    /* swing 40 %: odd notes 50 ms late, even notes unchanged, same number of notes, same order */
    memset(&r, 0, sizeof r); now = 1000; reset(3, 40);
    run_ticks(&r, &now, 600, 120);
    CHECK(abs_diff(r.n, 40) <= 1 || r.n >= 40, "swing 40 notes %u", r.n);
    for (unsigned k = 1; k < r.n && k < 40; ++k) {
        unsigned ideal = 125U * k + (k & 1U ? 50U : 0U), got = r.t[k] - r.t[0];
        CHECK(got >= ideal && got < ideal + 10U, "swing 40 note %u at %u, ideal %u", k, got, ideal);
        CHECK(r.note[k] == (k % 3 == 0 ? 60 : k % 3 == 1 ? 64 : 67), "swing 40 note %u is %u", k, r.note[k]);
    }
    CHECK(!r.doubled, "swing 40 doubled notes");

    /* rate, BPM and swing changes while running: the cycle never skips or repeats a note, never two note-ons in one tick */
    memset(&r, 0, sizeof r); now = 1000; reset(3, 25);
    run_ticks(&r, &now, 150, 120);
    arp_swing_set(50); run_ticks(&r, &now, 100, 90);
    arp_edit(0, ARP_RATE, -1); run_ticks(&r, &now, 150, 140);
    arp_swing_set(0); arp_edit(0, ARP_RATE, 2); run_ticks(&r, &now, 150, 100);
    CHECK(r.n > 20, "changing run notes %u", r.n);
    for (unsigned k = 0; k < r.n; ++k)
        CHECK(r.note[k] == (k % 3 == 0 ? 60 : k % 3 == 1 ? 64 : 67), "changing run note %u is %u", k, r.note[k]);
    CHECK(!r.doubled, "changing run doubled notes");

    /* polyrhythm and mutate stay inside the held range (60..67 +- octave) */
    static const uint8_t held[3] = {60, 64, 67};
    struct arp_event ev[6];
    for (unsigned variant = 0; variant < 12; ++variant) {
        reset(3, 0);
        arp_edit(0, ARP_CHANCE, (int)(variant % 5)); arp_edit(0, ARP_MUTATE, variant < 6 ? 0 : 100); arp_edit(0, ARP_VAR, (int)(variant % 4));
        for (unsigned i = 0, t = 5000; i < 600; ++i) {
            t += 10;
            unsigned m = arp_tick(0, 0, t, 120, 1, held, 3, ev, 6);
            for (unsigned k = 0; k < m; ++k)
                if ((ev[k].status & 0xf0) == 0x90) CHECK(ev[k].note >= 48 && ev[k].note <= 79, "variant %u note %u out of range", variant, ev[k].note);
        }
    }
    /* the randomisation page is not stored: a project load clears it */
    reset(3, 0); arp_edit(0, ARP_MUTATE, 50); arp_edit(0, ARP_CHANCE, 2);
    uint8_t st[56]; arp_state_get(st); arp_state_put(st);
    CHECK(!arp_get(0, ARP_MUTATE) && !arp_get(0, ARP_CHANCE) && !arp_get(0, ARP_VAR), "state put keeps random params");
    /* RATE and tempo changes while running (device 10.11: K1 stopped the sound for a long time, or
     * fired a note on every tick). After the change every note is one NEW step after the one before
     * it; the first gap lies between the old and the new step; the note count fits the new step. */
    {
        static const struct { unsigned from, to, tempo_from, tempo_to; } cases[] = {
            {5, 0, 120, 120}, {0, 5, 120, 120}, {5, 4, 120, 120}, {3, 2, 120, 120},
            {3, 3, 90, 140}, {3, 3, 140, 60}, {1, 4, 100, 133}, {0, 5, 60, 240}, {5, 0, 240, 60} };
        static const float beats[6] = {1.0f, 0.5f, 0.3333333f, 0.25f, 0.1666667f, 0.125f};
        for (unsigned c = 0; c < sizeof cases / sizeof *cases; ++c)
            for (unsigned phase = 0; phase < 9; ++phase) { /* the change falls at different points of a step */
                float was = 60000.0f / (float)cases[c].tempo_from * beats[cases[c].from];
                float is = 60000.0f / (float)cases[c].tempo_to * beats[cases[c].to];
                float lo = was < is ? was : is, hi = was < is ? is : was;
                reset(cases[c].from, 0);
                uint32_t t = 5000; struct run a = {0}, b = {0};
                run_ticks(&a, &t, 600 + phase * 7, cases[c].tempo_from);
                CHECK(a.n > 3 && a.n < 256, "case %u: %u notes before the change", c, a.n);
                arp_edit(0, ARP_RATE, (int)cases[c].to - (int)cases[c].from);
                run_ticks(&b, &t, 500, cases[c].tempo_to);
                CHECK(abs_diff(b.n, (unsigned)(5000.0f / is)) <= 2, "case %u phase %u: %u notes in 5 s, step %u ms", c, phase, b.n, (unsigned)is);
                CHECK(!b.doubled, "case %u: doubled notes", c);
                if (!a.n || !b.n) continue;
                float first = (float)(b.t[0] - a.t[a.n - 1]);
                CHECK(first >= lo - 11.0f && first <= hi + 11.0f, "case %u phase %u: first gap %u ms (old %u, new %u)", c, phase, (unsigned)first, (unsigned)was, (unsigned)is);
                for (unsigned k = 1; k < b.n; ++k) {
                    float gap = (float)(b.t[k] - b.t[k - 1]);
                    if (gap < is - 11.0f || gap > is + 11.0f) { CHECK(0, "case %u phase %u: gap %u = %u ms, step %u ms", c, phase, k, (unsigned)gap, (unsigned)is); break; }
                }
            }
    }
    /* The control task is held up for 3 s (a save): no catch-up burst afterwards, the steps go on. */
    {
        reset(5, 0);
        uint32_t t = 5000; struct run a = {0}, b = {0};
        run_ticks(&a, &t, 300, 120);
        t += 3000;
        run_ticks(&b, &t, 100, 120);
        CHECK(b.n >= 15 && b.n <= 17, "after a 3 s hold-up: %u notes in 1 s (16 expected)", b.n);
        for (unsigned k = 1; k < b.n; ++k) CHECK(b.t[k] - b.t[k - 1] >= 50U, "after a hold-up: gap %u ms", (unsigned)(b.t[k] - b.t[k - 1]));
    }
    /* User 10.11 F1: the ARP press (the leave rule was removed 11.11). */
    CHECK(arp_press_latch(0, 0) == 0 && arp_press_latch(0, 1) == 0, "an arp switched on by the press starts with LATCH off");
    CHECK(arp_press_latch(1, 1) == 1 && arp_press_latch(1, 0) == 0, "an arp already on keeps its LATCH");
    /* User 11.11: hold ARP = on / off; REC paused while stopped with the arp; the SEQ grid with the arp. */
    CHECK(arp_hold_toggle(0) == 1 && arp_hold_toggle(1) == 0, "hold ARP switches the arp to the opposite state");
    CHECK(arp_rec_pause(1, 0, 1) == 1, "REC + stopped + arp: the engine's step recording pauses");
    CHECK(arp_rec_pause(1, 1, 1) == 0, "REC + playing + arp: the take records (nearest step)");
    CHECK(arp_rec_pause(1, 0, 0) == 0, "REC + stopped without the arp: step recording as before");
    CHECK(arp_rec_pause(0, 0, 1) == 0 && arp_rec_pause(0, 1, 1) == 0, "no REC: nothing to pause");
    CHECK(arp_grid_writes(0, 0) == 1, "grid without the arp: the key writes its note");
    CHECK(arp_grid_writes(1, 0) == 0, "grid with the arp (melodic): the key only feeds the arp");
    CHECK(arp_grid_writes(1, 4) == 1 && arp_grid_writes(0, 4) == 1, "drum track: the grid as before");
    {   /* PLAY while the arp runs (1/16 at 120 BPM, UP over C E G): with REC the cycle goes on, without it restarts. */
        static const uint8_t held[3] = {60, 64, 67};
        struct arp_event ev[6];
        for (unsigned keep = 0; keep < 2; ++keep) {
            reset(3, 0);
            arp_keep_note(keep);
            uint32_t now = 1000;
            uint8_t seen[8]; unsigned vel[8], n = 0;
            for (unsigned i = 0; i < 20 + 30; ++i) { /* 200 ms stopped (C, E sound), then playing */
                now += 10;
                unsigned m = arp_tick(0, 0, now, 120, i >= 20, held, 3, ev, 6);
                for (unsigned k = 0; k < m; ++k)
                    if ((ev[k].status & 0xf0) == 0x90 && n < 8) { seen[n] = ev[k].note; vel[n++] = ev[k].velocity; }
            }
            CHECK(n >= 4 && seen[0] == 60 && seen[1] == 64, "keep %u: stopped arp played C E", keep);
            if (keep) {
                CHECK(n >= 4 && seen[2] == 67 && seen[3] == 60, "REC: PLAY goes on with G, then C (got %u %u)", seen[2], seen[3]);
                CHECK(n >= 4 && vel[3] == 112 && vel[2] != 112, "REC: the accent stays on the cycle's first note (C)");
            } else
                CHECK(n >= 4 && seen[2] == 60 && seen[3] == 64, "no REC: PLAY restarts the cycle from C (got %u %u)", seen[2], seen[3]);
        }
        reset(3, 0); /* fresh keys with REC on: the cycle starts from its first note */
        arp_keep_note(1);
        uint32_t now = 5000; unsigned first = 0;
        for (unsigned i = 0; i < 5 && !first; ++i) {
            now += 10;
            unsigned m = arp_tick(0, 0, now, 120, 1, held, 3, ev, 6);
            for (unsigned k = 0; k < m; ++k) if ((ev[k].status & 0xf0) == 0x90) first = ev[k].note;
        }
        CHECK(first == 60, "fresh arp with REC on starts from C (got %u)", first);
        arp_keep_note(0);
    }
    /* F2: REC follow over control ticks. */
    {
        CHECK(arp_rec_follow(0, 0x01) == 0, "no take: nothing");
        CHECK(arp_rec_follow(0, 0x01) == 0, "REC never on: an arp on stays on");
        CHECK(arp_rec_follow(1, 0x01) == 0, "take running: nothing yet");
        CHECK(arp_rec_follow(1, 0x01) == 0, "take running: nothing yet (2)");
        CHECK(arp_rec_follow(0, 0x01) == 0x01, "REC 1 -> 0 with the arp on during the take: off");
        CHECK(arp_rec_follow(0, 0x01) == 0, "only on the falling edge");
        /* a take without the arp, the arp switched on after REC ended: it must stay on */
        CHECK(arp_rec_follow(1, 0) == 0 && arp_rec_follow(1, 0) == 0, "take without arp");
        CHECK(arp_rec_follow(0, 0) == 0, "take without arp: nothing to stop");
        CHECK(arp_rec_follow(0, 0x04) == 0, "arp started after the take stays on");
        /* the arp ran for part of the take only (switched off by hand in the middle): still counted */
        CHECK(arp_rec_follow(1, 0x02) == 0 && arp_rec_follow(1, 0x00) == 0, "arp on, then off during the take");
        CHECK(arp_rec_follow(0, 0x00) == 0x02, "the arp was on during this take: reported");
        /* a former take's arp does not leak into the next take */
        CHECK(arp_rec_follow(1, 0x00) == 0 && arp_rec_follow(0, 0x08) == 0, "arp of an older take or after REC: not reported");
        /* two tracks */
        CHECK(arp_rec_follow(1, 0x01) == 0 && arp_rec_follow(1, 0x81) == 0 && arp_rec_follow(0, 0x81) == 0x81, "both tracks of the take");
    }
    if (failures) { printf("arp: %u failures\n", failures); return 1; }
    printf("arp: all checks passed\n");
    return 0;
}
