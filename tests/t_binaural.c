/* Host check for app/binaural.c (freestanding, no C library).
 * Exit 0 = every check passed; otherwise the code names the failing check.
 * Built with -DBIN_CYCLES=n the exit code is instead the cost of case n in
 * x86 TSC cycles per stereo frame (rough guide): 1 tones + pink noise,
 * 2 tones + rain, 3 crossfeed (B1) alone, 4 everything, 5 all off. */
#include "binaural.h"
#include "fm1_block.h"
#include <stdint.h>

void *memset(void *d, int c, unsigned long long n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

int _fltused = 1; /* MSVC ABI marker for float code without a C library */
enum { FS = 8388608, SEC = 345 /* blocks per second, 44100 / 128 */ };
static int32_t buf[FM1_BLOCK_WORDS];
static unsigned t; /* frames since start, for the B1 input */
static float amp = FS / 2; /* B1 input level */

static float fsin(float c) /* sin(2 pi c) */
{
    c -= (float)(int)c; if (c < 0) c += 1;
    float a = (c > 0.5f ? c - 1.0f : c) * 6.2831853f;
    if (a > 1.5707963f) a = 3.1415927f - a; else if (a < -1.5707963f) a = -3.1415927f - a;
    float q = a * a;
    return a * (1 - q * (0.1666667f - q * (0.0083333f - q * 0.0001984f)));
}
static void set(unsigned p, int steps) { for (int i = 0; i < 300; ++i) binaural_edit(p, -1); for (int i = 0; i < steps; ++i) binaural_edit(p, 1); }
/* one block: input silence (or a 1 kHz / 1.3 kHz pair at half scale), then render */
static void block(int input)
{
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i, ++t) {
        buf[2 * i] = input ? (int32_t)(fsin((float)t * (1000.0f / 44100.0f)) * amp) : 0;
        buf[2 * i + 1] = input ? (int32_t)(fsin((float)t * (1300.0f / 44100.0f)) * amp) : 0;
    }
    binaural_render(buf);
}
struct stat { int32_t peak, jump, prev[2]; unsigned up[2]; int32_t last[2]; };
static void measure(struct stat *s, unsigned first)
{
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i)
        for (unsigned c = 0; c < 2; ++c) {
            int32_t v = buf[2 * i + c], a = v < 0 ? -v : v, j = v - s->prev[c];
            if (a > s->peak) s->peak = a;
            if (j < 0) j = -j;
            if (!(first && !i) && j > s->jump) s->jump = j;
            if (s->last[c] < 0 && v >= 0) ++s->up[c];
            s->last[c] = v; s->prev[c] = v;
        }
}
static void run(unsigned blocks, int input, struct stat *s)
{
    for (unsigned b = 0; b < blocks; ++b) { block(input); if (s) measure(s, 0); }
}
#ifndef BIN_CYCLES
static void all_off(void)
{
    set(BIN_PRESET, 0); set(BIN_NOISE, 0); set(BIN_MODE, 0);
    run(6 * SEC, 0, 0); /* fades out */
}
#endif

#ifdef BIN_CYCLES
static inline uint64_t rdtsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
static int cycles(void)
{
    unsigned c = BIN_CYCLES;
    set(BIN_TIMER, 0);
    if (c == 1 || c == 4) { set(BIN_PRESET, 3); set(BIN_NOISE, c == 4 ? 20 : 10); }
    if (c == 2) { set(BIN_PRESET, 3); set(BIN_NOISE, 20); }
    if (c == 3 || c == 4) { set(BIN_MODE, 1); set(BIN_WIDTH, 15); set(BIN_FEED, 12); set(BIN_MIX, 20); }
    run(5 * SEC, 1, 0);
    uint64_t best = ~0ULL;
    for (unsigned r = 0; r < 20; ++r) {
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) buf[i] = (int32_t)(i * 1000U);
        uint64_t t0 = rdtsc();
        for (unsigned k = 0; k < 64; ++k) binaural_render(buf);
        uint64_t d = rdtsc() - t0;
        if (d < best) best = d;
    }
    return (int)(best / (64U * FM1_BLOCK_FRAMES));
}
#endif

int main(void)
{
#ifdef BIN_CYCLES
    return cycles();
#else
    struct stat s;
    /* 1: OFF from power on: no output change, nothing running */
    for (unsigned b = 0; b < 10; ++b) {
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) buf[i] = (int32_t)(i * 777U) - 50000;
        binaural_render(buf);
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) if (buf[i] != (int32_t)(i * 777U) - 50000) return 1;
    }
    if (binaural_running()) return 1;

    /* 2: state: 0xff = defaults, round trip, loading never starts a sound */
    { uint8_t st[BIN_STATE_BYTES], back[BIN_STATE_BYTES];
      for (unsigned i = 0; i < BIN_STATE_BYTES; ++i) st[i] = 0xff;
      binaural_state_put(st);
      if (binaural_beat10() != 100 || binaural_carrier() != 250 || binaural_xfeed() || binaural_timer_min() || binaural_running()) return 2; /* timer OFF by default */
      set(BIN_BEAT, 10); set(BIN_CARRIER, 30); set(BIN_WIDTH, 7);
      binaural_state_get(st); set(BIN_BEAT, 0); set(BIN_CARRIER, 0); binaural_state_put(st); binaural_state_get(back);
      for (unsigned i = 0; i < BIN_STATE_BYTES; ++i) if (st[i] != back[i]) return 2;
      set(BIN_MODE, 1); set(BIN_TIMER, 2); binaural_state_get(st); set(BIN_MODE, 0); set(BIN_TIMER, 0); binaural_state_put(st);
      if (!binaural_xfeed() || binaural_timer_min() != 10) return 2; /* 3D SOUND ON and the timer come back */
      if (binaural_beat10() != 55 || binaural_carrier() != 250 || binaural_width() != 35 || binaural_running()) return 2; }

    /* 3: presets: from CUSTOM right = the next bigger beat, left = smaller, below DELTA = OFF */
    set(BIN_PRESET, 3); if (binaural_beat10() != 100) return 3; /* ALPHA */
    binaural_edit(BIN_BEAT, 1); binaural_edit(BIN_BEAT, 1); binaural_edit(BIN_BEAT, 1); binaural_edit(BIN_BEAT, 1); /* 12 Hz */
    if (binaural_preset() != BIN_CUSTOM) return 3;
    binaural_edit(BIN_PRESET, 1); if (binaural_preset() != 4 || binaural_beat10() != 180) return 3;
    set(BIN_PRESET, 3); for (int i = 0; i < 4; ++i) binaural_edit(BIN_BEAT, 1);
    binaural_edit(BIN_PRESET, -1); if (binaural_preset() != 3 || binaural_beat10() != 100) return 3;
    set(BIN_BEAT, 0); binaural_edit(BIN_PRESET, -1); if (binaural_preset() != 0) return 3;
    /* noise level and type on their own knobs: the type is kept at level 0 */
    set(BIN_NOISE, 0); binaural_edit(BIN_NTYPE, 1); for (int i = 0; i < 3; ++i) binaural_edit(BIN_NLEVEL, 1);
    if (binaural_noise() != 13 || !binaural_running()) return 3;
    binaural_edit(BIN_NTYPE, -1); if (binaural_noise() != 3) return 3;
    for (int i = 0; i < 20; ++i) binaural_edit(BIN_NLEVEL, -1);
    if (binaural_noise() != 0 || binaural_running()) return 3;
    binaural_edit(BIN_NTYPE, 1); binaural_edit(BIN_NLEVEL, 1); if (binaural_noise() != 11) return 3;
    all_off();

    /* 4: the beat: zero crossings over 20 s, L = carrier and R - L = beat within 0.1 Hz */
    static const uint8_t beats[3] = {0, 3, 5}; /* presets: (custom 0.5 Hz), ALPHA 10, FOCUS 40 */
    for (unsigned k = 0; k < 3; ++k) {
        set(BIN_TIMER, 0); set(BIN_CARRIER, k * 30); /* 100 / 250 / 400 Hz */
        if (beats[k]) set(BIN_PRESET, beats[k]); else { set(BIN_PRESET, 1); set(BIN_BEAT, 0); }
        unsigned beat10 = binaural_beat10(), car = binaural_carrier();
        run(5 * SEC, 0, 0);
        s = (struct stat){0};
        run(20 * SEC, 0, &s); /* 20 * 345 blocks = 19.98 s */
        float sec = 20.0f * SEC * FM1_BLOCK_FRAMES / 44100.0f;
        float fl = (float)s.up[0] / sec, fb = ((float)s.up[1] - (float)s.up[0]) / sec;
        if (fl < (float)car - 0.1f || fl > (float)car + 0.1f) return 4;
        if (fb < (float)beat10 / 10.0f - 0.1f || fb > (float)beat10 / 10.0f + 0.1f) return 4;
        if (s.peak > FS / 4 + 64) return 4; /* tone at 0.25 full scale */
        all_off();
    }

    /* 5: peak at max settings with noise (pink and rain): below 0.7 full scale */
    for (unsigned n = 10; n <= 20; n += 10) {
        set(BIN_TIMER, 0); set(BIN_PRESET, 5); set(BIN_CARRIER, 80); set(BIN_NOISE, (int)n);
        s = (struct stat){0};
        run(30 * SEC, 0, &s);
        if (s.peak >= FS * 7 / 10 || s.peak < FS / 4) return 5;
        all_off();
    }

    /* 6: no clicks: carrier and beat jump every block, the output slope stays
     * within that of a 540 Hz sine (500 + 40 beat) at 0.25 full scale
     * (161 kLSB / sample; a click would be ~2 M) */
    set(BIN_TIMER, 0); set(BIN_PRESET, 5); set(BIN_CARRIER, 80);
    run(5 * SEC, 0, 0);
    s = (struct stat){0}; measure(&s, 1);
    for (unsigned b = 0; b < 10 * SEC; ++b) {
        if (b % 3 == 0) set(BIN_CARRIER, (b / 3) & 1 ? 0 : 80);
        if (b % 5 == 0) set(BIN_BEAT, (b / 5) & 1 ? 0 : 79);
        block(0); measure(&s, 0);
    }
    if (s.jump > 175000) return 61;
    /* noise: switched between off and level 10 every 0.1 s, the jumps stay
     * those of the steady noise (ramped gain, no step) */
    set(BIN_PRESET, 0); set(BIN_NOISE, 10); run(5 * SEC, 0, 0);
    struct stat q = {0}; measure(&q, 1); run(10 * SEC, 0, &q);
    s = (struct stat){0}; measure(&s, 1);
    for (unsigned b = 0; b < 10 * SEC; ++b) { if (b % 34 == 0) set(BIN_NOISE, (b / 34) & 1 ? 0 : 10); block(0); measure(&s, 0); }
    if (s.jump > q.jump + q.jump / 10) return 62;
    all_off();
    /* B1: width / feed / mix jump between extremes; slope within 3x that of the input */
    set(BIN_MODE, 1);
    run(2 * SEC, 1, 0);
    s = (struct stat){0}; measure(&s, 1);
    for (unsigned b = 0; b < 10 * SEC; ++b) {
        if (b % 2 == 0) set(BIN_WIDTH, (b / 2) & 1 ? 0 : 20);
        if (b % 3 == 0) set(BIN_FEED, (b / 3) & 1 ? 0 : 20);
        if (b % 7 == 0) set(BIN_MIX, (b / 7) & 1 ? 0 : 20);
        if (b % 50 == 0) binaural_edit(BIN_MODE, (b / 50) & 1 ? 1 : -1);
        block(1); measure(&s, 0);
    }
    if (s.jump > 3 * 785000 || s.peak > 2 * FS) return 63; /* input slope: 2 pi 1300 / 44100 * FS / 2 = 777 k */
    all_off();
    /* B1 faded out after OFF: the input passes unchanged */
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) buf[i] = (int32_t)(i * 777U);
    binaural_render(buf);
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) if (buf[i] != (int32_t)(i * 777U)) return 64;

    /* 7: timer 5 min: ends in silence, the last 10 s fade, then exactly zero */
    set(BIN_TIMER, 1); set(BIN_PRESET, 3); set(BIN_NOISE, 15);
    if (!binaural_running() || binaural_remaining() != 300) return 71;
    run(290 * SEC, 0, 0);
    if (!binaural_running() || binaural_remaining() > 12 || binaural_remaining() < 8) return 72;
    s = (struct stat){0}; run(SEC, 0, &s); int32_t at_9s = s.peak; /* 10 .. 9 s left */
    run(8 * SEC, 0, 0);
    s = (struct stat){0}; run(SEC / 2, 0, &s); /* the last half second */
    if (s.peak > at_9s / 10 + 1000) return 73;
    run(2 * SEC, 0, 0);
    if (binaural_running()) return 74;
    s = (struct stat){0}; run(SEC, 0, &s);
    if (s.peak) return 75;
    /* a new turn restarts the session and the timer */
    binaural_edit(BIN_BEAT, 1);
    if (!binaural_running()) return 76;
    run(1, 0, 0); if (binaural_remaining() < 299) return 77;
    all_off();

    /* 9: no hard clip: a mix at 0.95 FS plus FOCUS at 500 Hz, rain 10, crossfeed
     * at width 100 / mix 100: every sample stays below full scale (soft limiter) */
    amp = FS * 0.95f;
    set(BIN_TIMER, 0); set(BIN_PRESET, 5); set(BIN_CARRIER, 80); set(BIN_NOISE, 20);
    set(BIN_MODE, 1); set(BIN_WIDTH, 20); set(BIN_FEED, 0); set(BIN_MIX, 20);
    run(4 * SEC, 1, 0);
    s = (struct stat){0}; run(10 * SEC, 1, &s);
    if (s.peak >= FS - 1 || s.peak < FS * 8 / 10) return 9;
    all_off();
    /* 10: below the knee (0.8 FS) the limiter is transparent: crossfeed on at
     * MIX 0 gives the input back sample for sample, up to 0.79 FS */
    amp = FS * 0.79f;
    set(BIN_MODE, 1); set(BIN_MIX, 0); set(BIN_WIDTH, 10);
    run(6 * SEC, 1, 0);
    for (unsigned b = 0; b < SEC; ++b) {
        unsigned t0 = t;
        block(1);
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
            unsigned f = t0 + i;
            if (buf[2 * i] != (int32_t)(fsin((float)f * (1000.0f / 44100.0f)) * amp)) return 10;
            if (buf[2 * i + 1] != (int32_t)(fsin((float)f * (1300.0f / 44100.0f)) * amp)) return 10;
        }
    }
    set(BIN_MIX, 16); /* back to the default 80 */
    amp = FS / 2;
    all_off();

    /* 8: OFF again: no work, the block passes unchanged */
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) buf[i] = (int32_t)(i * 333U);
    binaural_render(buf);
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) if (buf[i] != (int32_t)(i * 333U)) return 8;
    return 0;
#endif
}
