/* SEQ rework C3 host check of app/variation.c with a stub engine: CHANCE patterns mute steps in the playing copy only
 * (the stored pattern comes back on stop), a muted step's ratchet split does not fire and is kept, an old CHANCE byte
 * (percent levels 1..7) loads as OFF, a pattern saves as 0x80 | n, MUTATE stays in a fixed key, and with KEY = AUTO
 * and no chord it does octave jumps only. Not covered: the closed engine, screen drawing, timing on the device. */
#include "variation.h"
#include "instrument.h"
#include "keyscale.h"
#include "seq_steps.h"

void *memset(void *d, int c, unsigned long long n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, unsigned long long n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strcat(char *d, const char *s) { char *r = d; while (*d) ++d; while ((*d++ = *s++)) {} return r; }
unsigned long long strlen(const char *s) { unsigned long long n = 0; while (s[n]) ++n; return n; }

#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)

/* stub engine: 8 tracks of 64 cells */
static struct groove_step eng[8][64];
static unsigned eng_len = 16, eng_playing, eng_step, eng_pads[8];
void groove_sequence_read(unsigned t, unsigned first, struct groove_step *c, unsigned n) { for (unsigned i = 0; i < n; ++i) c[i] = eng[t][first + i]; }
void groove_sequence_replace(unsigned t, unsigned first, const struct groove_step *c, unsigned n) { for (unsigned i = 0; i < n; ++i) eng[t][first + i] = c[i]; }
void groove_sequence_options_copy(struct groove_sequence_options *o) { memset(o, 0, sizeof *o); }
unsigned groove_pad_count(unsigned t) { return eng_pads[t]; }
unsigned groove_pad_note(unsigned t, unsigned p) { (void)t; return 36U + p; }
unsigned groove_midi_input_channel(unsigned t) { return t; }
void groove_get_view(unsigned t, unsigned s, struct groove_view *v)
{
    (void)s; memset(v, 0, sizeof *v);
    v->playing = t == 0 ? (uint8_t)eng_playing : 0; v->play_step = (uint8_t)eng_step; v->sequence_length = (uint8_t)eng_len;
    v->audible = 1; v->tempo = 120;
}
static unsigned posted_on, posted_notes[64];
static int post(uint8_t status, uint8_t a, uint8_t b) { (void)a; if ((status & 0xf0) == 0x90 && b) posted_notes[posted_on++ & 63U] = a; return 1; }

static void fill_track(unsigned base)
{
    memset(eng, 0, sizeof eng);
    for (unsigned s = 0; s < 16; ++s) { eng[0][s].note = (uint8_t)(base + s); eng[0][s].velocity = 100; eng[0][s].drums = 15U << 5; }
}
static void clear_vars(void)
{
    uint8_t z[32]; memset(z, 0, sizeof z); var_state_put(z);
}

static int chance_and_ratchet(void)
{
    clear_vars(); fill_track(60);
    var_edit(0, VAR_CHANCE, 1); CHECK(var_get(0, VAR_CHANCE) == 1); /* HOUSE 1 ..x...x...x...x. */
    char v[12]; var_value(0, VAR_CHANCE, v); CHECK(v[0] == 'H' && v[1] == 'S' && v[2] == '1' && !v[3]);
    var_split_set(0, 0, 1); var_split_set(0, 2, 2); /* step 0 muted (2X), step 2 plays (3X) */
    uint32_t now = 1000; posted_on = 0;
    eng_playing = 1; eng_step = 0; var_tick(now, post);              /* loop start: build */
    for (unsigned s = 0; s < 16; ++s) CHECK((eng[0][s].velocity != 0) == (s == 2 || s == 6 || s == 10 || s == 14));
    for (unsigned k = 0; k < 120; ++k) var_tick(++now, post);         /* the whole step: no ratchet hit */
    CHECK(posted_on == 0 && var_split_get(0, 0) == 1);                /* muted: no hits, split kept */
    eng_step = 1; var_tick(++now, post); for (unsigned k = 0; k < 120; ++k) var_tick(++now, post);
    eng_step = 2; var_tick(++now, post); for (unsigned k = 0; k < 130; ++k) var_tick(++now, post);
    CHECK(posted_on == 2 && posted_notes[0] == 62 && posted_notes[1] == 62); /* 3X = 2 extra hits */
    /* an edit while playing goes into the original; stop brings the stored pattern back untouched */
    eng[0][6].note = 90; /* step 6 plays in HOUSE 1 */
    eng_step = 3; var_tick(++now, post);
    eng_playing = 0; var_tick(++now, post);
    for (unsigned s = 0; s < 16; ++s) CHECK(eng[0][s].velocity == 100);
    CHECK(eng[0][6].note == 90 && eng[0][5].note == 65 && var_split_get(0, 0) == 1);
    /* OFF: every step plays, the split on step 0 fires again */
    var_edit(0, VAR_CHANCE, -1); CHECK(var_get(0, VAR_CHANCE) == 0);
    posted_on = 0; eng_playing = 1; eng_step = 0; var_tick(++now, post);
    for (unsigned k = 0; k < 120; ++k) var_tick(++now, post);
    CHECK(posted_on == 1 && eng[0][1].velocity == 100);
    eng_playing = 0; var_tick(++now, post);
    /* the pattern walks OFF .. DNB 3 and stops at both ends */
    var_edit(0, VAR_CHANCE, 40); CHECK(var_get(0, VAR_CHANCE) == 1);
    for (unsigned i = 0; i < 20; ++i) var_edit(0, VAR_CHANCE, 1);
    CHECK(var_get(0, VAR_CHANCE) == 12); var_value(0, VAR_CHANCE, v); CHECK(v[0] == 'D' && v[1] == 'B' && v[2] == '3');
    return 0;
}
static int saved_byte(void)
{
    uint8_t st[32]; memset(st, 0, sizeof st);
    for (unsigned old = 1; old <= 7; ++old) { st[3] = (uint8_t)old; var_state_put(st); CHECK(var_get(0, VAR_CHANCE) == 0); } /* old % -> OFF */
    st[3] = 0x80 | 5; st[4 + 3] = 0x80 | 12; var_state_put(st);
    CHECK(var_get(0, VAR_CHANCE) == 5 && var_get(1, VAR_CHANCE) == 12);
    uint8_t out[32]; var_state_get(out); CHECK(out[3] == (0x80 | 5) && out[7] == (0x80 | 12) && out[11] == 0);
    return 0;
}
static int mutate_in_key(void)
{
    clear_vars(); var_edit(0, VAR_MUTATE, 100);
    key_edit(0, 3); /* KEY D, MODE MAJ */
    const unsigned dmaj = key_scale_mask();
    CHECK(dmaj == ((1U << 2) | (1U << 4) | (1U << 6) | (1U << 7) | (1U << 9) | (1U << 11) | (1U << 1)));
    uint32_t now = 5000;
    for (unsigned loop = 0; loop < 20; ++loop) {
        fill_track(55);
        eng_playing = 1; eng_step = 0; var_tick(++now, post);
        for (unsigned s = 0; s < 16; ++s) CHECK(dmaj >> (eng[0][s].note % 12U) & 1U);
        eng_playing = 0; var_tick(++now, post);
    }
    key_edit(0, -20); key_auto_reset(); CHECK(key_scale_mask() == 0); /* AUTO, no chord yet: octave jumps only */
    for (unsigned loop = 0; loop < 20; ++loop) {
        fill_track(55);
        eng_playing = 1; eng_step = 0; var_tick(++now, post);
        for (unsigned s = 0; s < 16; ++s) { int n = eng[0][s].note, b = 55 + (int)s; CHECK(n == b + 12 || n == b - 12); }
        eng_playing = 0; var_tick(++now, post);
    }
    var_edit(0, VAR_MUTATE, -100);
    return 0;
}
int main(void)
{
    int r = chance_and_ratchet();
    if (!r) r = saved_byte();
    if (!r) r = mutate_in_key();
    return r;
}
