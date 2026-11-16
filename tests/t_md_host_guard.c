/* Host test for micro_d_host_guard (app/micro_d.c), fix 11.11: a MICRO D
 * track's silent PSG host that became audible again (CC 7 from outside, a
 * path inside the closed instrument) is put back to volume 0 about every
 * 250 ms from the control task.
 *  - channel reaches MICRO D tracks only: first a plain CC 7 = 0 to the
 *    instrument (no undo point, project not marked changed); if the volume is
 *    still not 0 at the next check, groove_control_edit;
 *  - channel shared with another track: groove_control_edit at once (the CC
 *    would silence the other track too);
 *  - never touches a track that is not MICRO D, not PSG, or switched off.
 * Also: micro_d_state_put of 0xff bytes (what extras_defaults does on a
 * factory preset load) clears every MICRO D owner.
 * Build/run (Git Bash): CLANG=... sh tests/run_md_host_guard.sh
 * Exit code 0 = all passed, otherwise the number of the first failed case. */
#include "micro_d.h"
#include "instrument.h"
#include <stdint.h>

void *memset(void *d, int c, unsigned long long n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned long long n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, unsigned long long n)
{
    unsigned char *p = d; const unsigned char *q = s;
    if (p < q) while (n--) *p++ = *q++; else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
unsigned long long strlen(const char *s) { unsigned long long n = 0; while (s[n]) ++n; return n; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
int _fltused;

/* ---- fakes: the instrument ---- */
static uint8_t vol[GROOVE_TRACKS], type[GROOVE_TRACKS], role[GROOVE_TRACKS], in_ch[GROOVE_TRACKS];
static unsigned edits, edit_track, posts, post_fail, cc_ignored, views;
static uint8_t last_post[3];
void groove_get_view(unsigned track, unsigned step, struct groove_view *v) { (void)step; ++views; v->volume = vol[track]; v->type = type[track]; }
void groove_control_edit(unsigned track, int program_steps, int volume_steps, int cutoff_steps)
{
    (void)program_steps; (void)cutoff_steps;
    ++edits; edit_track = track; /* on the device: undo point + project marked changed (NORMAL track) */
    int x = vol[track] + volume_steps; vol[track] = (uint8_t)(x < 0 ? 0 : x > 127 ? 127 : x);
}
int groove_source_set(unsigned track, unsigned t) { type[track] = (uint8_t)t; return 0; }
unsigned groove_track_role(unsigned t) { return role[t]; }
unsigned groove_midi_input_channel(unsigned t) { return in_ch[t]; }
/* The instrument's MIDI input: CC 7 sets the volume of every track on that input channel. */
int fm1_midi_post(const uint8_t *m, size_t n)
{
    ++posts; memset(last_post, 0, sizeof last_post); memcpy(last_post, m, n);
    if (post_fail) return 1;
    if (n == 3 && (m[0] & 0xf0U) == 0xb0U && m[1] == 7 && !cc_ignored)
        for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
            if (role[t] != GROOVE_TRACK_OFF && in_ch[t] == (m[0] & 15U)) vol[t] = m[2];
    return 0;
}

static uint32_t now;
static void setup(void)
{
    uint8_t none[MD_STATE_BYTES];
    memset(none, 0xff, sizeof none);
    micro_d_state_put(none);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) { vol[t] = 100; type[t] = GROOVE_DX7; role[t] = GROOVE_TRACK_NORMAL; in_ch[t] = (uint8_t)t; }
    now += 1000U; micro_d_host_guard(now); /* settle: nothing to do, clears the guard's memory */
    edits = posts = post_fail = cc_ignored = 0;
}
static void run_ms(unsigned ms) { for (unsigned i = 0; i < ms; ++i) micro_d_host_guard(++now); }

int main(void)
{
    /* 1: attach silences the host; a quiet host costs nothing (no post, no edit). */
    setup();
    if (micro_d_attach(1) || vol[1] != 0 || type[1] != GROOVE_PSG) return 1;
    edits = views = 0;
    run_ms(2000);
    if (edits || posts) return 1;
    if (views != 8) return 1;                       /* one look per MICRO D track every 250 ms, not every poll */

    /* 2: CC 7 from outside made the host audible, MICRO D alone on its channel:
     * silent again within 250 ms by a plain CC 7 = 0 on the track's input
     * channel, without groove_control_edit (no undo point, project not changed). */
    vol[1] = 100;
    run_ms(250);
    if (vol[1] != 0 || edits || posts != 1 || last_post[0] != 0xb1 || last_post[1] != 7 || last_post[2] != 0) return 2;
    run_ms(2000);
    if (posts != 1 || edits) return 2;              /* once per event, nothing while it stays silent */

    /* 3: the instrument did not take the CC (closed engine): the next check
     * uses groove_control_edit; a later event starts with the plain CC again. */
    setup(); (void)micro_d_attach(1); edits = 0;
    vol[1] = 100; cc_ignored = 1;
    run_ms(250);
    if (vol[1] != 100 || posts != 1 || edits) return 3;
    run_ms(250);
    if (vol[1] != 0 || posts != 1 || edits != 1 || edit_track != 1) return 3;
    cc_ignored = 0; vol[1] = 70;
    run_ms(250);
    if (vol[1] != 0 || posts != 2 || edits != 1) return 3;
    /* a full MIDI queue (post fails): groove_control_edit in the same check */
    setup(); (void)micro_d_attach(1); edits = 0;
    vol[1] = 100; post_fail = 1;
    run_ms(250);
    if (vol[1] != 0 || edits != 1) return 3;

    /* 4: mixed channel (T2 MICRO D + T3 DX7 on one channel): never the plain CC
     * (it would silence T3); groove_control_edit for the host only. */
    setup(); (void)micro_d_attach(1); edits = 0;
    in_ch[2] = in_ch[1];
    vol[1] = 100;
    run_ms(250);
    if (vol[1] != 0 || vol[2] != 100 || posts || edits != 1 || edit_track != 1) return 4;
    /* the other track switched off: MICRO D only again -> plain CC */
    role[2] = GROOVE_TRACK_OFF; vol[1] = 100;
    run_ms(250);
    if (vol[1] != 0 || vol[2] != 100 || posts != 1 || edits != 1) return 4;

    /* 5: two MICRO D tracks on one channel: still "MICRO D only" (plain CC). */
    setup(); (void)micro_d_attach(1); (void)micro_d_attach(4); edits = 0;
    in_ch[4] = in_ch[1];
    vol[1] = 100; vol[4] = 80;
    run_ms(250);
    if (vol[1] != 0 || vol[4] != 0 || edits || !posts) return 5;

    /* 6: other tracks are never touched, whatever their volume; nor a track
     * whose host is not PSG (stale owner) or that is switched off. */
    setup(); (void)micro_d_attach(1); edits = 0;
    run_ms(1000);
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) if (t != 1 && vol[t] != 100) return 6;
    if (edits || posts) return 6;
    type[1] = GROOVE_DX7; vol[1] = 100;             /* a load put another engine there, owner not cleared yet */
    run_ms(1000);
    if (vol[1] != 100 || edits || posts) return 6;
    type[1] = GROOVE_PSG; role[1] = GROOVE_TRACK_OFF;
    run_ms(1000);
    if (vol[1] != 100 || edits || posts) return 6;
    role[1] = GROOVE_TRACK_NORMAL;
    run_ms(250);
    if (vol[1] != 0) return 6;

    /* 7: detach gives the host its volume back and the guard leaves it alone. */
    setup(); (void)micro_d_attach(1);
    micro_d_detach(1);
    if (micro_d_track(1) || vol[1] != 100) return 7;
    edits = 0;
    run_ms(1000);
    if (vol[1] != 100 || edits || posts) return 7;

    /* 8: 0xff bytes (extras_defaults: factory preset load, pattern without
     * MICRO D) clear every owner; the guard then touches nothing. */
    setup(); (void)micro_d_attach(1); (void)micro_d_attach(4);
    if (micro_d_count() != 2) return 8;
    uint8_t none[MD_STATE_BYTES];
    memset(none, 0xff, sizeof none);
    micro_d_state_put(none);
    if (micro_d_count() || micro_d_track(1) || micro_d_track(4) || micro_d_owns(1)) return 8;
    vol[1] = vol[4] = 100; edits = 0;
    run_ms(1000);
    if (vol[1] != 100 || vol[4] != 100 || edits || posts) return 8;

    /* 9: the millisecond counter wraps: the guard keeps its period. */
    setup(); (void)micro_d_attach(1);
    now = 0xffffff00U; micro_d_host_guard(now);
    vol[1] = 100;
    run_ms(600);
    if (vol[1] != 0) return 9;
    return 0;
}
