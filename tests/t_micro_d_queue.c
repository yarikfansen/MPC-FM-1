/* Host check for the MICRO D note queue (app/micro_d.c): the engine calls
 * micro_d_note from both cores at once (odd tracks core 0, even tracks
 * core 1). The lost update is modelled by a hook that runs a second
 * micro_d_note (the other core) inside the first one, between reading and
 * moving the write index. Freestanding, no C library; micro_d.c is
 * compiled into this file so the voices can be inspected.
 * Exit 0 = every check passed; otherwise the number of the failing case. */
#include <stdint.h>
void *memset(void *d, int c, unsigned long long n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned long long n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
unsigned long long strlen(const char *s) { unsigned long long n = 0; while (s[n]) ++n; return n; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
#include "../app/micro_d.c"
int _fltused;
static uint8_t vol[GROOVE_TRACKS] = {100, 100, 100, 100, 100, 100, 100, 100}, type[GROOVE_TRACKS];
void groove_get_view(unsigned track, unsigned step, struct groove_view *v) { (void)step; v->volume = vol[track]; v->type = type[track]; }
void groove_control_edit(unsigned track, int program_steps, int volume_steps, int cutoff_steps)
{ (void)track; (void)program_steps; (void)volume_steps; (void)cutoff_steps; }
int groove_source_set(unsigned track, unsigned t) { type[track] = (uint8_t)t; return 0; }
/* micro_d_host_guard (tests/t_md_host_guard.c) is not exercised here */
unsigned groove_track_role(unsigned t) { (void)t; return GROOVE_TRACK_NORMAL; }
unsigned groove_midi_input_channel(unsigned t) { return t; }
int fm1_midi_post(const uint8_t *m, size_t n) { (void)m; (void)n; return 0; }

enum { A = 0, B = 3 }; /* the user's T1 (core 0) and T4 (core 1) */
static int32_t buf[FM1_BLOCK_WORDS];
static void render(void) { for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) buf[i] = 0; micro_d_render(buf); }
/* Notes gated on a track, as a bit set over the 2 voices' notes (bit = note - 48). */
static uint32_t gated(unsigned track)
{
    struct slot *d = slot_of(track);
    uint32_t m = 0;
    for (unsigned i = 0; i < VOICES; ++i) if (d->v[i].on && d->v[i].gate) m |= 1U << (d->v[i].note - 48U);
    return m;
}
static unsigned in_t, in_n, in_v;
static void other_core(void) { micro_d_note(in_t, in_n, in_v); }
/* micro_d_note(t, n, v) with the other core's note (it, inn, iv) landing in the middle of it */
static void racing(unsigned t, unsigned n, unsigned v, unsigned it, unsigned inn, unsigned iv)
{
    in_t = it; in_n = inn; in_v = iv; md_test_note_mid = other_core;
    micro_d_note(t, n, v);
}
static void reset(void)
{
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) if (micro_d_track(t)) micro_d_detach(t);
    if (micro_d_attach(A) || micro_d_attach(B)) return;
    micro_d_edit(A, MD_MODE, -10); micro_d_edit(A, MD_MODE, 1); /* MODE 2: two voices */
    micro_d_edit(B, MD_MODE, -10); micro_d_edit(B, MD_MODE, 1);
    render();
}

int main(void)
{
    /* 1: note-ons from both cores at the same moment, both ways round, 20
     * times: none lost */
    reset();
    if (!micro_d_track(A) || !micro_d_track(B)) return 1;
    for (unsigned k = 0; k < 20; ++k) {
        racing(A, 60, 100, B, 64, 100);
        racing(B, 67, 100, A, 62, 100);
        render();
        if (gated(A) != ((1U << 12) | (1U << 14)) || gated(B) != ((1U << 16) | (1U << 19))) return 1;
        /* 2: the note-offs race too: none lost (no stuck note) */
        racing(A, 60, 0, B, 64, 0);
        racing(B, 67, 0, A, 62, 0);
        render();
        if (gated(A) || gated(B)) return 2;
    }
    /* 3: order within a lane (= a track) kept, with the other core's notes
     * in between: on 60, off 60, on 62 -> only 62 held; then off 62, on
     * 62, off 62 -> nothing */
    reset();
    racing(A, 60, 100, B, 64, 100); racing(A, 60, 0, B, 64, 0); racing(A, 62, 100, B, 65, 100);
    render();
    if (gated(A) != (1U << 14) || gated(B) != (1U << 17)) return 3;
    micro_d_note(A, 62, 0); micro_d_note(A, 62, 100); racing(A, 62, 0, B, 65, 0);
    render();
    if (gated(A) || gated(B)) return 3;
    /* 4: queue full (32 entries a lane between two renders): the 33rd note on
     * that lane is dropped, as before; the other core's lane is not affected */
    reset();
    for (unsigned k = 0; k < 16; ++k) { micro_d_note(A, 60, 100); micro_d_note(A, 60, 0); }
    micro_d_note(A, 70, 100);  /* dropped */
    micro_d_note(B, 64, 100);
    render();
    if (gated(A) || gated(B) != (1U << 16)) return 4;
    micro_d_note(A, 70, 100); /* room again after the render */
    render();
    if (gated(A) != (1U << 22)) return 4;
    return 0;
}
