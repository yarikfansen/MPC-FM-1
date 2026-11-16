/* Host check for app/master_fx.c (freestanding, no C library).
 * Exit 0 = every filter and ring type stays finite and bounded at extreme
 * settings; otherwise the code names the failing case (see below).
 * Built with -DMFX_CYCLES the exit code is instead the CPU cost of the most
 * expensive case in cycles per stereo frame / 10 (x86 TSC, rough guide). */
#include "master_fx.h"
#include <stdint.h>

#ifndef MFX_HOST_LIBC /* a scratch harness may include this file and link the C library */
void *memset(void *d, int c, unsigned long long n)
{
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
#endif

static int32_t buf[FM1_BLOCK_WORDS];
static uint32_t seed = 1;

static void set(unsigned fx, unsigned param, int value)
{
    mfx_edit(fx, param, -255);
    mfx_edit(fx, param, value);
}

/* 110 Hz saw plus a little noise, Q23, then silence after `blocks_on`. */
static int run(unsigned blocks, unsigned blocks_on, int32_t *peak)
{
    static unsigned t;
    for (unsigned b = 0; b < blocks; ++b) {
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i, ++t) {
            int32_t v = 0;
            if (b < blocks_on) {
                seed = seed * 1664525U + 1013904223U;
                v = (int32_t)((t % 401U) * 20000U) - 4000000 + (int32_t)(seed >> 12) - (1 << 19);
            }
            buf[2 * i] = v; buf[2 * i + 1] = v;
        }
        mfx_process(buf);
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
            int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > *peak) *peak = a;
        }
    }
    return 0;
}

/* ---- Output safety (W2: S2 limiter always on, S7 sub-bass filter, S8 reverb headroom) ---- */
#define FSQ 8388608.0 /* full scale, Q23 */
#define RATE 44100.0
#define PI_D 3.14159265358979323846
extern unsigned mfx_wr16_clamps; /* master_fx.c, FM1_HOST_TEST only: reverb line samples clamped */
extern unsigned mfx_test_zeroed; /* master_fx.c, FM1_HOST_TEST only: bytes of effect memory zeroed */
static int32_t in_copy[FM1_BLOCK_WORDS];
static unsigned long gen_n; /* running sample index of the test tone */

static double dsin(double x) /* sin(x) without libm: range reduction, then Taylor */
{
    long n = (long)(x / (2 * PI_D));
    x -= (double)n * 2 * PI_D;
    if (x > PI_D) x -= 2 * PI_D; else if (x < -PI_D) x += 2 * PI_D;
    if (x > PI_D / 2) x = PI_D - x; else if (x < -PI_D / 2) x = -PI_D - x;
    double q = x * x, t = x, s = x;
    for (int k = 1; k < 12; ++k) { t *= -q / (double)((2 * k) * (2 * k + 1)); s += t; }
    return s;
}
/* One block of amp * sin(2 pi hz t) + dc (in full scale) on both channels; a copy goes to in_copy. */
static void fill(double amp, double hz, double dc)
{
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i, ++gen_n) {
        double v = (amp * dsin(2 * PI_D * hz * (double)gen_n / RATE) + dc) * FSQ;
        int32_t s = (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
        buf[2 * i] = buf[2 * i + 1] = s;
    }
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) in_copy[i] = buf[i];
}
/* Clean start: defaults, OUTPUT 0 dB, LOW CUT off, a few silent blocks so every fade state is at rest. */
static void fresh(void)
{
    mfx_reset(); mfx_output_set(0); mfx_lowcut_set(0);
    for (unsigned b = 0; b < 4; ++b) {
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) buf[i] = 0;
        mfx_process(buf);
    }
    gen_n = 0;
}
/* The load guard's last step: every master FX unit out at once (only the boost / limiter, sub filter and chime
 * stay); the units come back on the next plain mfx_process. */
static void process_shed(int32_t *b)
{
    mfx_shed((1U << MFX_UNITS) - 1U, (1U << MFX_UNITS) - 1U);
    mfx_process(b);
    mfx_shed(0, 0);
}
/* S2: 1.6 FS 220 Hz with no effect on comes out limited (<= 0.89 FS + 1 % after 2 blocks), never flat-topped,
 * not squashed (peak >= 0.75 FS); then 0.5 FS passes bit-exact, at once from rest and again after the release. */
static int limiter_case(void (*proc)(int32_t *), int code)
{
    fresh();
    int32_t peak = 0, last = 0; unsigned run_len = 0;
    for (unsigned b = 0; b < 50; ++b) {
        fill(1.6, 220.0, 0.0);
        proc(buf);
        if (b < 2) continue;
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
            int32_t v = buf[2 * i], a = v < 0 ? -v : v;
            if (buf[2 * i + 1] != v) return code;
            if (a > peak) peak = a;
            if ((double)a > 0.89 * 1.01 * FSQ) return code + 1;
            run_len = v == last ? run_len + 1 : 1; last = v;
            if (run_len >= 4 && (double)a > 0.5 * FSQ) return code + 2; /* flat top */
        }
    }
    if ((double)peak < 0.75 * FSQ) return code + 3;
    /* Release: back to rest within 600 blocks (~1.7 s), then bit-exact. */
    for (unsigned b = 0; b < 650; ++b) {
        fill(0.5, 220.0, 0.0);
        proc(buf);
        if (b >= 600) for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) if (buf[i] != in_copy[i]) return code + 4;
    }
    fresh();
    for (unsigned b = 0; b < 50; ++b) {
        fill(0.5, 220.0, 0.0);
        proc(buf);
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) if (buf[i] != in_copy[i]) return code + 5;
    }
    return 0;
}
/* The limiter gain glides inside a block: no step at a block edge (coordinator 7). 0.5 FS 220 Hz, then OUTPUT
 * 0 -> +12 dB: the gain climbs for many blocks. The second difference of a sine of amplitude A is at most
 * A * w^2 = 0.001 * A; a gain step of one block's worth at an edge would show as about 0.008 FS. */
static int limiter_glide_case(void (*proc)(int32_t *), int code)
{
    fresh();
    double y1 = 0, y2 = 0, worst = 0;
    for (unsigned b = 0; b < 160; ++b) {
        if (b == 20) mfx_output_set(4);
        fill(0.5, 220.0, 0.0);
        proc(buf);
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
            double y = buf[2 * i] / FSQ, d2 = y - 2 * y1 + y2;
            if (d2 < 0) d2 = -d2;
            if (b >= 1 && d2 > worst) worst = d2;
            y2 = y1; y1 = y;
        }
    }
    mfx_output_set(0);
    return worst > 0.003 ? code : 0;
}
/* S7, float path forced on by EQ HIGH = 65 (8 kHz shelf +0.19 dB; nothing else on).
 * Returns output power / input power over the last `measure` blocks of `blocks`. */
static double float_path_gain(double hz, unsigned blocks, unsigned measure)
{
    fresh(); set(MFX_EQ, 3, 65);
    double pin = 0, pout = 0;
    for (unsigned b = 0; b < blocks; ++b) {
        fill(0.5, hz, 0.0);
        mfx_process(buf);
        if (b + measure >= blocks)
            for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
                double x = in_copy[2 * i] / FSQ, y = buf[2 * i] / FSQ;
                pin += x * x; pout += y * y;
            }
    }
    set(MFX_EQ, 3, 64);
    return pout / pin;
}
static int float_path_case(void)
{
    /* DC 0.3 FS: under 0.01 FS from 0.5 s on (1 s fed). */
    fresh(); set(MFX_EQ, 3, 65);
    for (unsigned b = 0; b < 345; ++b) {
        fill(0.0, 0.0, 0.3);
        mfx_process(buf);
        if (gen_n > 22050)
            for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) if ((double)(buf[i] < 0 ? -buf[i] : buf[i]) >= 0.01 * FSQ) return 90;
    }
    set(MFX_EQ, 3, 64);
    /* 10 Hz at least 12 dB down (power ratio 10^-1.2), 100 Hz less than 1 dB down (10^-0.1). */
    if (float_path_gain(10.0, 690, 345) > 0.0630957) return 91;
    if (float_path_gain(100.0, 345, 172) < 0.7943282) return 92;
    return 0;
}
/* S8: HALL, DECAY 127, PREDELAY 0, MIX 127, a full-scale tone for 3 s; returns how often a reverb line clamped. */
static unsigned verb_clamps(double hz)
{
    fresh();
    set(MFX_VERB, 0, 1); set(MFX_VERB, 1, 127); set(MFX_VERB, 2, 0); set(MFX_VERB, 3, 127);
    mfx_wr16_clamps = 0;
    for (unsigned b = 0; b < 1034; ++b) { fill(8388607.0 / FSQ, hz, 0.0); mfx_process(buf); }
    set(MFX_VERB, 3, -127);
    return mfx_wr16_clamps;
}
/* S8 wet level: reverb at its defaults (HALL, DECAY 70, PREDELAY 20) with MIX 127, a 1 kHz burst of 0.25 FS
 * for 0.2 s (69 blocks), then silence; returns the mean square (FS^2, both channels) of the following 0.5 s (172 blocks). */
static double verb_tail_power(void)
{
    fresh(); set(MFX_VERB, 3, 127);
    double p = 0;
    for (unsigned b = 0; b < 69 + 172; ++b) {
        if (b < 69) fill(0.25, 1000.0, 0.0); else fill(0.0, 0.0, 0.0);
        mfx_process(buf);
        if (b >= 69) for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) { double y = buf[i] / FSQ; p += y * y; }
    }
    set(MFX_VERB, 3, -127);
    return p / (172.0 * FM1_BLOCK_WORDS); /* mean square */
}

/* ---- W6: FILTER TYPE crossfade (no dry burst, no jump) and effect fades without per-block steps ---- */
/* Output meter over both channels: peak, largest sample-to-sample jump, largest second difference
 * |y[n+1] - 2 y[n] + y[n-1]| (all samples, and only where the three samples span a block edge). */
typedef struct { double peak, jump, d2, d2_edge, y1[2], y2[2]; unsigned n; } meter;
static void meter_zero(meter *m) { m->peak = m->jump = m->d2 = m->d2_edge = 0; } /* keeps the sample history */
static void meter_block(meter *m, const int32_t *w)
{
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i, ++m->n)
        for (unsigned c = 0; c < 2; ++c) {
            double y = w[2 * i + c] / FSQ, a = y < 0 ? -y : y;
            if (a > m->peak) m->peak = a;
            if (m->n >= 1) { double j = y - m->y1[c]; j = j < 0 ? -j : j; if (j > m->jump) m->jump = j; }
            if (m->n >= 2) {
                double d = y - 2 * m->y1[c] + m->y2[c]; d = d < 0 ? -d : d;
                if (d > m->d2) m->d2 = d;
                if (i < 2 && d > m->d2_edge) m->d2_edge = d;
            }
            m->y2[c] = m->y1[c]; m->y1[c] = y;
        }
}
static double maxd(double a, double b) { return a > b ? a : b; }
/* Figures of the last runs, for the report (a scratch harness prints them). */
static double w6_burst_peak, w6_burst_ratio, w6_jump_over, w6_fast_diff[2], w6_fade_d2[4], w6_fade_dry;
static unsigned w6_fast_exact[2];

/* No TYPE change: one type with MIX 127 on a fixed signal (110 Hz saw 0.29 FS + noise, R = L / 2), 300 blocks
 * to settle (fade-in done), then 200 blocks hashed (FNV-1a over every output word) while CUTOFF and RESO step
 * every 25 blocks (DJ crosses its open centre at 64). */
static uint32_t bitid_run(unsigned type)
{
    static const uint8_t cuts[8] = {40, 90, 20, 110, 64, 30, 100, 50};
    fresh();
    set(MFX_FILTER, 0, (int)type); set(MFX_FILTER, 1, 40); set(MFX_FILTER, 2, 60); set(MFX_FILTER, 3, 127);
    uint32_t h = 2166136261U, s = 99U;
    unsigned t = 0;
    for (unsigned b = 0; b < 500; ++b) {
        if (b >= 300 && (b - 300) % 25 == 0) {
            unsigned k = (b - 300) / 25;
            set(MFX_FILTER, 1, cuts[k]); set(MFX_FILTER, 2, k & 1 ? 90 : 60);
        }
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i, ++t) {
            s = s * 1664525U + 1013904223U;
            int32_t v = (int32_t)((t % 401U) * 12000U) - 2400000 + (int32_t)(s >> 13) - (1 << 18);
            buf[2 * i] = v; buf[2 * i + 1] = v / 2;
        }
        mfx_process(buf);
        if (b >= 300) for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) h = (h ^ (uint32_t)buf[i]) * 16777619U;
    }
    set(MFX_FILTER, 3, -127);
    return h;
}
/* Peak (or largest jump) of one filter type in steady state: blocks 150..199 of amp * sin(hz). */
static double steady(unsigned type, int cut, int res, double hz, unsigned want_jump)
{
    fresh();
    set(MFX_FILTER, 0, (int)type); set(MFX_FILTER, 1, cut); set(MFX_FILTER, 2, res); set(MFX_FILTER, 3, 127);
    meter m = {0};
    for (unsigned b = 0; b < 200; ++b) {
        fill(0.5, hz, 0.0); mfx_process(buf);
        if (b == 150) meter_zero(&m);
        meter_block(&m, buf);
    }
    set(MFX_FILTER, 3, -127);
    return want_jump ? m.jump : m.peak;
}
/* Dry burst: low-pass types at CUTOFF 25 / RESO 20 / MIX 127 on a 0.5 FS 2 kHz sine (all of them attenuate it
 * strongly). TYPE a -> b after 150 blocks: no block of the next 200 may peak over 1.5 x the larger steady peak.
 * Code before W6 (fade out to dry, clear, fade in, 1/8 steps): worst block peak 0.4998 FS, about 100000 x the
 * larger steady peak (first failing pair ACID -> SEM). W6: worst 0.0171 FS, 1.02 x (incoming type primed 8
 * blocks unheard: with no priming 26 x, with 4 blocks 2.8 x, from its start transient). */
static int burst_case(void)
{
    static const uint8_t lp[4] = {0, 1, 2, 4}; /* ACID, SEM, LADDER, DJ (CUTOFF 25 is below its centre) */
    double sp[4];
    for (unsigned k = 0; k < 4; ++k) sp[k] = steady(lp[k], 25, 20, 2000.0, 0);
    w6_burst_peak = w6_burst_ratio = 0;
    int fail = 0;
    for (unsigned a = 0; a < 4; ++a)
        for (unsigned b = 0; b < 4; ++b) {
            if (a == b) continue;
            fresh();
            set(MFX_FILTER, 0, lp[a]); set(MFX_FILTER, 1, 25); set(MFX_FILTER, 2, 20); set(MFX_FILTER, 3, 127);
            for (unsigned k = 0; k < 150; ++k) { fill(0.5, 2000.0, 0.0); mfx_process(buf); }
            set(MFX_FILTER, 0, lp[b]);
            double ref = maxd(sp[a], sp[b]);
            for (unsigned k = 0; k < 200; ++k) {
                fill(0.5, 2000.0, 0.0); mfx_process(buf);
                meter m = {0};
                meter_block(&m, buf);
                if (m.peak > w6_burst_peak) w6_burst_peak = m.peak;
                if (m.peak / ref > w6_burst_ratio) w6_burst_ratio = m.peak / ref;
                if (m.peak > 1.5 * ref && !fail) fail = 100 + (int)(a * 4 + b);
            }
            set(MFX_FILTER, 3, -127);
        }
    return fail;
}
/* No jump: all 5 types at CUTOFF 64 / RESO 40 / MIX 127 on a 0.5 FS 220 Hz sine; TYPE a -> b after 150 blocks:
 * the largest sample-to-sample jump over the next 200 blocks stays within the larger steady jump + 0.02 FS. */
static double steady_jump[5];
static int jump_case(void)
{
    for (unsigned t = 0; t < 5; ++t) steady_jump[t] = steady(t, 64, 40, 220.0, 1);
    w6_jump_over = -1;
    for (unsigned a = 0; a < 5; ++a)
        for (unsigned b = 0; b < 5; ++b) {
            if (a == b) continue;
            fresh();
            set(MFX_FILTER, 0, (int)a); set(MFX_FILTER, 1, 64); set(MFX_FILTER, 2, 40); set(MFX_FILTER, 3, 127);
            meter m = {0};
            for (unsigned k = 0; k < 150; ++k) { fill(0.5, 220.0, 0.0); mfx_process(buf); meter_block(&m, buf); }
            meter_zero(&m);
            set(MFX_FILTER, 0, (int)b);
            for (unsigned k = 0; k < 200; ++k) { fill(0.5, 220.0, 0.0); mfx_process(buf); meter_block(&m, buf); }
            set(MFX_FILTER, 3, -127);
            double over = m.jump - maxd(steady_jump[a], steady_jump[b]);
            if (over > w6_jump_over) w6_jump_over = over;
            if (over > 0.02) return 120 + (int)(a * 5 + b);
        }
    return 0;
}
/* Fast turning: TYPE stepped every `step` blocks through seq[], then left alone (CUTOFF 64, RESO 40, 0.5 FS
 * 220 Hz). No jump over the limit above; then the output is compared with a run that had the final type from
 * the start (same input): w6_fast_exact = bit-identical from 40 blocks after the last step on; w6_fast_diff =
 * the largest difference from 400 blocks after the last step on (must be under 0.001 FS). */
enum { FAST_WARM = 150, FAST_KEEP = 440 };
static int32_t fast_ref[FAST_KEEP][FM1_BLOCK_WORDS];
static int fast_case(const uint8_t *seq, unsigned n, unsigned step, unsigned slot, int code)
{
    unsigned last = FAST_WARM + step * (n - 1), total = last + FAST_KEEP;
    double lim = 0;
    for (unsigned t = 0; t < 5; ++t) lim = maxd(lim, steady_jump[t]);
    lim += 0.02;
    /* A short run of the final type first, so the remembered RESO glide start is the same in both runs. */
    for (unsigned pass = 0; pass < 3; ++pass) {
        fresh();
        set(MFX_FILTER, 0, pass == 2 ? seq[0] : seq[n - 1]); set(MFX_FILTER, 1, 64); set(MFX_FILTER, 2, 40); set(MFX_FILTER, 3, 127);
        meter m = {0};
        unsigned exact = 1, zeroed = 0;
        double diff = 0;
        for (unsigned b = 0; b < (pass ? total : 20U); ++b) {
            if (pass == 2 && b >= FAST_WARM && b <= last && (b - FAST_WARM) % step == 0) set(MFX_FILTER, 0, seq[(b - FAST_WARM) / step]);
            fill(0.5, 220.0, 0.0);
            unsigned z = mfx_test_zeroed;
            mfx_process(buf);
            if (pass == 2 && mfx_test_zeroed - z > zeroed) zeroed = mfx_test_zeroed - z;
            if (pass == 2) {
                if (b == FAST_WARM) meter_zero(&m);
                meter_block(&m, buf);
            }
            if (b < last) continue;
            if (pass == 1) for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) fast_ref[b - last][i] = buf[i];
            if (pass == 2 && b >= last + 40)
                for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
                    if (buf[i] != fast_ref[b - last][i]) exact = 0;
                    double d = (buf[i] - fast_ref[b - last][i]) / FSQ;
                    d = d < 0 ? -d : d;
                    if (b >= last + 400 && d > diff) diff = d;
                }
        }
        set(MFX_FILTER, 3, -127);
        if (pass < 2) continue;
        w6_fast_exact[slot] = exact; w6_fast_diff[slot] = diff;
        if (m.jump > lim) return code;
        if (diff > 0.001) return code + 1;
        if (zeroed > 4096U) return code + 2; /* the incoming comb line is cleared sliced (FX_CLEAR_BUDGET) */
    }
    return 0;
}
/* Smooth fades: a 0.5 FS 440 Hz sine; at block 40 the change, then 120 blocks: the second difference at every
 * block edge stays within the dry sine's own largest second difference + 0.004 FS (a gain step of a 1/16 fade
 * on a wet signal of ~0.1 FS is ~0.006 FS at the edge). mode 0 = MIX 0 -> 127, mode 1 = TYPE + 1 at MIX 127.
 * Measured (limit = dry 0.00196 + 0.004): code before W6 REVERB 0.00615, DRIVE 0.0223, CHORUS 0.0122 (all fail);
 * W6 0.00548, 0.00209, 0.00244. REVERB at DECAY 127 / PREDELAY 0: at its defaults the wet is too quiet for the
 * old steps to show (0.0045 before and after; its own upsampling corners). */
static int fade_case(unsigned fx, unsigned mode, unsigned slot, int code)
{
    fresh();
    /* DRIVE 0: SOFT bends the 0.5 FS sine (wet - dry ~0.17 FS, a 1/8 step ~0.02 FS) while its own second
     * difference stays near the dry one; at higher DRIVE the clipped wave's corners alone pass the limit. */
    if (fx == MFX_DRIVE) set(fx, 1, 0);
    if (fx == MFX_VERB) { set(fx, 1, 127); set(fx, 2, 0); }
    if (mode == 1) set(fx, 3, 127);
    meter m = {0}, dry = {0};
    for (unsigned b = 0; b < 160 + (mode ? 200U : 0U); ++b) {
        if (b == (mode ? 240U : 40U)) {
            if (mode) mfx_edit(fx, 0, 1); else set(fx, 3, 127);
            meter_zero(&m);
        }
        fill(0.5, 440.0, 0.0); mfx_process(buf);
        meter_block(&m, buf); meter_block(&dry, in_copy);
    }
    set(fx, 3, -127);
    w6_fade_dry = dry.d2; w6_fade_d2[slot] = m.d2_edge;
    return m.d2_edge > dry.d2 + 0.004 ? code : 0;
}

#ifdef MFX_CYCLES
static inline uint64_t rdtsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }
#endif

int main(void)
{
    /* W6, no TYPE change: each filter type bit-identical to the code before W6. Hashes measured with that code
     * (app/master_fx.c at f2a30c7) running this same bitid_run sequence first in the program, as here (the
     * ladder's RESO glide start carries over from one run to the next, so the order matters). */
    {
        static const uint32_t ref[5] = {0x0255DDF5U, 0xDC5DBC27U, 0x4BB1150FU, 0xF1EFE4A1U, 0x417E3641U};
        for (unsigned t = 0; t < 5; ++t) if (bitid_run(t) != ref[t]) return 150 + (int)t;
    }
    /* Filters: type 0..4 at full resonance, cutoff low / mid / high. */
    for (unsigned type = 0; type < 5; ++type)
        for (unsigned cut = 0; cut < 3; ++cut) {
            set(MFX_FILTER, 0, (int)type); set(MFX_FILTER, 1, (int)(cut * 63)); set(MFX_FILTER, 2, 127); set(MFX_FILTER, 3, 127);
            int32_t peak = 0;
#ifdef MFX_CYCLES
            uint64_t t0 = rdtsc();
            run(200, 200, &peak);
            uint64_t per = (rdtsc() - t0) / (200U * FM1_BLOCK_FRAMES);
            if (type == MFX_CYCLES && cut == 1) return (int)(per / 10U);
#else
            run(300, 200, &peak);
            if (peak >= (1 << 23) * 8 || peak < 0) return 10 + (int)(type * 3 + cut);  /* blew up */
#endif
        }
    set(MFX_FILTER, 3, -127);
    /* Ring modulator: 8 carriers at the lowest and highest FREQ. */
    for (unsigned wave = 0; wave < 8; ++wave)
        for (unsigned f = 0; f < 2; ++f) {
            set(MFX_RING, 0, (int)wave); set(MFX_RING, 1, f ? 127 : 0); set(MFX_RING, 2, 127); set(MFX_RING, 3, 127);
            int32_t peak = 0;
            run(200, 200, &peak);
            if (peak >= (1 << 23) * 4 || peak < 0) return 40 + (int)(wave * 2 + f);
        }
    set(MFX_RING, 3, -127);
    /* Reverb: 3 types at full DECAY, PREDELAY and MIX, a long silent tail
     * (int16 lines, damping state), then off and on again (fx_clear). */
    for (unsigned type = 0; type < 3; ++type) {
        set(MFX_VERB, 0, (int)type); set(MFX_VERB, 1, 127); set(MFX_VERB, 2, 127); set(MFX_VERB, 3, 127);
        int32_t peak = 0;
        run(2000, 200, &peak);
        if (peak >= (1 << 23) || peak <= 0) return 60 + (int)type;
        /* off for 20 blocks: longer than the 20 Hz filter's fade-out on the integer path (MFX_SUB_FADE 16), so
         * only the reverb's own memory can show here (back within the fade the filter goes on from its state)
         * - was 4 blocks while the integer path dropped the filter's state at once */
        set(MFX_VERB, 3, -127); run(20, 20, &peak);
        set(MFX_VERB, 3, 127); peak = 0; run(4, 0, &peak);
        if (peak > (1 << 12)) return 63 + (int)type; /* restart from clean memory, no old tail */
    }
    /* Load guard (CPU FULL) with every unit cut (process_shed): the mix is
     * left untouched at 0 dB boost, and the effect comes back from clean
     * memory, no old tail. */
    set(MFX_VERB, 0, 1); set(MFX_VERB, 3, 127);
    { int32_t peak = 0; run(400, 400, &peak); }
    /* The 20 Hz filter fades out over MFX_SUB_FADE blocks on the integer path (no step at the cut; before, the
     * cut block itself was untouched because the filter's state was dropped at once): untouched after that. */
    for (unsigned k = 0; k <= MFX_SUB_FADE; ++k) {
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) buf[i] = (int32_t)(i * 1000U) - 128000;
        process_shed(buf);
    }
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) if (buf[i] != (int32_t)(i * 1000U) - 128000) return 70;
    { int32_t peak = 0; run(4, 0, &peak); if (peak > (1 << 12)) return 71; }
    set(MFX_VERB, 3, -127);
    /* S2: limiter always on at OUTPUT 0 dB, through both entry points. */
    { int e = limiter_case(mfx_process, 80); if (e) return e; }
    { int e = limiter_case(process_shed, 86); if (e) return e; }
    { int e = limiter_glide_case(mfx_process, 96); if (e) return e; }
    { int e = limiter_glide_case(process_shed, 97); if (e) return e; }
    /* S7: DC and 10 Hz removed, 100 Hz kept, in the float path. */
    { int e = float_path_case(); if (e) return e; }
    /* S8: no clamp inside the reverb lines on a full-scale 100 Hz tone (old gains: 0 here too, see below) and
     * on a tone at the 3rd harmonic of the longest HALL comb, 22050 / 678 * 3 = 97.57 Hz (old gains: 42809). */
    if (verb_clamps(100.0) != 0) return 93;
    if (verb_clamps(22050.0 / 678.0 * 3.0) != 0) return 94;
    /* S8: wet level within 1 dB of the code before the change (0.25 / 0.18 gains, no 120 Hz high-pass):
     * tail mean square measured then = 0.00135640086 (RMS 0.0368 FS). 1 dB = power x 1.2589. */
    {
        const double old_tail = 0.00135640086;
        double p = verb_tail_power();
        if (p < old_tail / 1.2589254 || p > old_tail * 1.2589254) return 95;
    }
    /* W6: FILTER TYPE change without the dry burst (100..115), without a jump (120..144); fast turning (160..163);
     * effect fades without block-edge steps (170..172). */
    { int e = burst_case(); if (e) return e; }
    { int e = jump_case(); if (e) return e; }
    {
        static const uint8_t s1[9] = {0, 1, 2, 3, 4, 3, 2, 1, 0}, s2[7] = {0, 1, 2, 3, 4, 3, 2}, s3[6] = {0, 1, 2, 3, 4, 3};
        /* Every 2 blocks, back to the start type: bit-identical (holds: the change is still unheard when the
         * knob comes back, so it is cancelled and the playing instance was never touched). */
        int e = fast_case(s1, 9, 2, 0, 160);
        if (e) return e;
        if (!w6_fast_exact[0]) return 163;
        /* Every 11 blocks (newer TYPE during an audible crossfade, a COMB start while priming) and every 2 blocks
         * ending on COMB: not bit-identical (the new instance has another history), under 1e-6 FS after 400 blocks. */
        e = fast_case(s2, 7, 11, 1, 160);
        if (e) return e;
        e = fast_case(s3, 6, 2, 1, 160);
        if (e) return e;
    }
    { int e = fade_case(MFX_VERB, 0, 0, 170); if (e) return e; }
    { int e = fade_case(MFX_DRIVE, 1, 1, 171); if (e) return e; }
    { int e = fade_case(MFX_CHORUS, 0, 2, 172); if (e) return e; }
    return 0;
}
#ifndef MFX_HOST_LIBC
void *memcpy(void *d, const void *s, unsigned long long n)
{
    unsigned char *p = d; const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}
int _fltused = 1;
#endif
