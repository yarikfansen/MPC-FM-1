/* Host check of the boot chime in app/master_fx.c (user 10.11, F10), freestanding.
 * Exit 0 = the five onsets (F4 0 ms, A4 533 ms, C5 1600 ms, F5 2400 ms, the chord of all four at 3200 ms)
 * are within one block, every note's own pitch rises at its onset (Goertzel), all four chord pitches rise at
 * 3200 ms, and the output peak stays under 0.89 of full scale. Otherwise the exit code names the case. */
#include "master_fx.h"
#include <stdint.h>

void *memset(void *d, int c, unsigned long long n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long long n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}
int _fltused = 1;

extern unsigned mfx_test_chime_frames, mfx_test_chime_onset[8];
enum { FRAMES = 44100 * 6, WIN = 4096 };
static int16_t out[FRAMES]; /* left channel, Q15 */
static int32_t buf[FM1_BLOCK_WORDS];

static double cos_small(double x) /* |x| < 0.2 rad */
{
    double q = x * x;
    return 1 - q / 2 * (1 - q / 12 * (1 - q / 30 * (1 - q / 56)));
}
/* Power of frequency hz over WIN frames starting at frame `at`, Hann window (low leakage from the
 * neighbouring chord notes). */
static double power(double hz, unsigned at)
{
    double c = 2 * cos_small(2 * 3.14159265358979 * hz / 44100.0), s1 = 0, s2 = 0;
    double wc = cos_small(2 * 3.14159265358979 / WIN), ws = 2 * 3.14159265358979 / WIN * (1 - (2 * 3.14159265358979 / WIN) * (2 * 3.14159265358979 / WIN) / 6);
    double rc = 1, rs = 0; /* cos / sin of 2 pi i / WIN by rotation */
    for (unsigned i = 0; i < WIN; ++i) {
        double w = 0.5 - 0.5 * rc, t = rc * wc - rs * ws;
        rs = rs * wc + rc * ws; rc = t;
        double s0 = w * out[at + i] / 32768.0 + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}
int main(void)
{
    static const double hz[4] = {349.23, 440.00, 523.25, 698.46};
    static const unsigned at_ms[5] = {0, 533, 1600, 2400, 3200};
    int32_t peak = 0;
    mfx_chime_start();
    for (unsigned f = 0; f + FM1_BLOCK_FRAMES <= FRAMES; f += FM1_BLOCK_FRAMES) {
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) buf[i] = 0;
        mfx_process(buf);
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
            int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > peak) peak = a;
        }
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) out[f + i] = (int16_t)(buf[2 * i] >> 8);
    }
    /* 1-8: onsets of the eight voices within one block of the plan */
    for (unsigned v = 0; v < 8; ++v) {
        unsigned want = at_ms[v < 4 ? v : 4] * 441U / 10U, got = mfx_test_chime_onset[v];
        if (!got) return 1 + (int)v;
        --got;
        if (got + FM1_BLOCK_FRAMES < want || got > want + FM1_BLOCK_FRAMES) return 1 + (int)v;
    }
    /* 10: no clip, peak under 0.89 FS (the limiter level) */
    if (peak >= 7465860) return 10;
    /* 11-14: each melody note's pitch at least 4x stronger just after its onset (+ attack) than just before */
    for (unsigned k = 1; k < 4; ++k) {
        unsigned on = at_ms[k] * 441U / 10U;
        if (!(power(hz[k], on + 600) > 4.0 * power(hz[k], on - WIN) + 1e-6)) return 11 + (int)k;
    }
    if (!(power(hz[0], 600) > 1e-3)) return 11;
    /* 15-18: the chord: all four pitches at least 4x stronger after 3200 ms than before */
    unsigned chord = 3200U * 441U / 10U;
    for (unsigned k = 0; k < 4; ++k)
        if (!(power(hz[k], chord + 600) > 4.0 * power(hz[k], chord - WIN))) return 15 + (int)k;
    /* 19: the chime ends (it must not ring on for ever) */
    for (unsigned i = FRAMES - WIN; i < FRAMES; ++i) if (out[i] > 64 || out[i] < -64) return 19;
    return 0;
}
