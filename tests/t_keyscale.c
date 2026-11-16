/* Host check for app/keyscale.c (freestanding): chord names, inversions,
 * AUTO key from the first chord, chosen KEY / MODE, scale masks, stored byte.
 * Exit 0 = every check passed; otherwise the failing check. */
#include "keyscale.h"
#include <stdint.h>

void *memset(void *d, int c, unsigned long long n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
static int eq(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static int name_is(const uint8_t *n, unsigned c, const char *want, unsigned want_ok, unsigned want_root, unsigned want_minor)
{
    char out[8]; unsigned root = 99, minor = 99;
    unsigned ok = chord_name(n, c, out, &root, &minor);
    if (!eq(out, want) || ok != want_ok) return 0;
    if (ok && (root != want_root || minor != want_minor)) return 0;
    return 1;
}
#define N(...) ((const uint8_t[]){__VA_ARGS__}), sizeof((const uint8_t[]){__VA_ARGS__})
#define M(...) mask_of((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))
static unsigned mask_of(const uint8_t *pc, unsigned n) { unsigned m = 0; for (unsigned i = 0; i < n; ++i) m |= 1U << pc[i]; return m; }

int main(void)
{
    /* 1: names, root position */
    if (!name_is(N(62, 66, 69), "D", 1, 2, 0)) return 1;          /* D F# A */
    if (!name_is(N(55, 58, 62, 65), "Gm7", 1, 7, 1)) return 1;    /* G Bb D F */
    if (!name_is(N(61, 65, 68, 72), "C#maj7", 1, 1, 0)) return 1;
    if (!name_is(N(48, 55), "C5", 1, 0, 2)) return 1;
    if (!name_is(N(60, 65, 67), "Csus4", 1, 0, 2)) return 1;
    if (!name_is(N(57, 60, 63), "Adim", 1, 9, 1)) return 1;
    if (!name_is(N(60, 64, 67, 70), "C7", 1, 0, 0)) return 1;
    if (!name_is(N(60, 64, 70), "C7", 1, 0, 0)) return 1;         /* no fifth */
    /* 2: inversions name the real root; the bass wins a tie (C6 vs Am7) */
    if (!name_is(N(64, 67, 72), "C", 1, 0, 0)) return 2;          /* E G C */
    if (!name_is(N(67, 72, 76), "C", 1, 0, 0)) return 2;          /* G C E */
    if (!name_is(N(60, 64, 67, 69), "C6", 1, 0, 0)) return 2;
    if (!name_is(N(57, 60, 64, 67), "Am7", 1, 9, 1)) return 2;
    /* 3: one pitch class = the note; nothing; unknown; >8 notes clipped */
    if (!name_is(N(60), "C4", 0, 0, 0)) return 3;
    if (!name_is(N(48, 60, 72), "C3", 0, 0, 0)) return 3;
    if (!name_is(N(1), "C#-1", 0, 0, 0)) return 3;
    if (!name_is(0, 0, "", 0, 0, 0)) return 3;
    if (!name_is(N(60, 61, 62), "--", 0, 0, 0)) return 3;
    if (!name_is(N(128, 200), "", 0, 0, 0)) return 3;
    /* 4: AUTO: no key until a chord with a third; single notes, C5, sus, unknown do not fix it */
    key_state_put(0xff);
    if (key_get() != 0 || key_mode_get() != 0 || key_scale_mask() != 0) return 4;
    key_chord_seen(N(60)); key_chord_seen(N(48, 55)); key_chord_seen(N(60, 65, 67)); key_chord_seen(N(60, 61, 62));
    if (key_scale_mask() != 0) return 4;
    key_chord_seen(N(55, 58, 62, 65)); /* Gm7 -> G minor: G A Bb C D Eb F */
    if (key_scale_mask() != M(7, 9, 10, 0, 2, 3, 5)) return 4;
    key_chord_seen(N(62, 66, 69)); /* later chords do not change it */
    if (key_scale_mask() != M(7, 9, 10, 0, 2, 3, 5)) return 4;
    key_auto_reset(); /* project load */
    if (key_scale_mask() != 0) return 4;
    key_chord_seen(N(62, 66, 69)); /* D major: D E F# G A B C# */
    if (key_scale_mask() != M(2, 4, 6, 7, 9, 11, 1)) return 4;
    /* 5: chosen KEY overrides AUTO; MODE only when KEY is chosen; limits */
    key_edit(1, 1); if (key_mode_get() != 0) return 5; /* AUTO: MODE stays */
    key_edit(0, 1); if (key_get() != 1 || key_scale_mask() != M(0, 2, 4, 5, 7, 9, 11)) return 5; /* C major */
    key_edit(1, 1); if (key_mode_get() != 1 || key_scale_mask() != M(0, 2, 3, 5, 7, 8, 10)) return 5; /* C minor */
    key_edit(0, 100); if (key_get() != 12 || key_scale_mask() != M(11, 1, 2, 4, 6, 7, 9)) return 5; /* B minor */
    key_edit(0, -100); if (key_get() != 0 || key_scale_mask() != M(2, 4, 6, 7, 9, 11, 1)) return 5; /* back to AUTO (D major) */
    /* 6: stored byte round trip; bad bytes = defaults */
    key_edit(0, 10); key_edit(1, 1); /* A minor */
    unsigned st = key_state_get();
    key_state_put(0xff); if (key_get() || key_mode_get()) return 6;
    key_state_put(st); if (key_get() != 10 || key_mode_get() != 1 || key_scale_mask() != M(9, 11, 0, 2, 4, 5, 7)) return 6;
    key_state_put(13); if (key_get() || key_mode_get()) return 6;
    key_state_put(0x2a); if (key_get() || key_mode_get()) return 6;
    return 0;
}
