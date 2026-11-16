/* Host check for the MASTER FX REVERB (app/master_fx.c run_verb, FDN-8 design of
 * docs/coord-notes/research-reverb-1011.md), freestanding, no C library.
 * master_fx.c is compiled into this file (MFX_SRC: a mutated copy for the
 * mutation runs of tests/run_reverb.sh) so its state can be inspected.
 * Exit 0 = every check passed; otherwise the number of the failing case:
 *  1x  impulse / burst metrics per type (RT60 500 Hz, HF damping, echo density,
 *      mixing time, tail bandwidth, L/R correlation), x = type * 3 + metric group
 *  19  no modulation heard on a steady 1 kHz sine (all of the tail is the input tone)
 *  20  a full-scale sine (25 Hz .. 4 kHz, DECAY 0 / 64 / 127) clamps a line
 *  21  fuzz: output not finite or out of bounds   22  a float state went subnormal
 *  30  silence in -> the tail does not end in exact 0 (lines, states, output)
 *  40  TYPE change / shed / return: block-edge step (LOW CUT on, see switch_case)   41  more cleared per block than the slice
 *  42  shed: the reverb still heard / not back from clean memory
 *  43  REVERB alone (LOW CUT off): TYPE change, shed / return, MIX off / on (also back during the 20 Hz filter's
 *      fade-out) and off for good: a block-edge step (the integer path used to drop the filter's state)
 *  44  after the fade-out the integer path is bit-identical to the dry input and the filter state is zero
 *  45  a 1.5 FS input during that fade: more flat-topped samples than the limiter alone gives (sub_g forced 0 = the
 *      same run without the fade), or a difference larger than the filter's correction (a clamp before the limiter)
 *  50  MIX 0: output not bit-identical to the reverb before the FDN (hash of 92398aa) */
#include "master_fx.h"
#include <stdint.h>

#ifndef MFX_HOST_LIBC
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
int _fltused = 1; /* MSVC ABI marker for float code without a C library */
#endif
#ifndef MFX_SRC
#define MFX_SRC "../app/master_fx.c"
#endif
#include MFX_SRC

#define SR 44100
#define PI_D 3.14159265358979323846
static double dsin(double x)
{
    long n = (long)(x / (2 * PI_D));
    x -= (double)n * 2 * PI_D;
    if (x > PI_D) x -= 2 * PI_D; else if (x < -PI_D) x += 2 * PI_D;
    if (x > PI_D / 2) x = PI_D - x; else if (x < -PI_D / 2) x = -PI_D - x;
    double x2 = x * x, t = x, s = x;
    for (int k = 1; k < 10; ++k) { t *= -x2 / ((2 * k) * (2 * k + 1)); s += t; }
    return s;
}
static double dcos(double x) { return dsin(x + PI_D / 2); }
static double dsqrt(double x) { if (x <= 0) return 0; double y = x > 1 ? x : 1; for (int i = 0; i < 60; ++i) y = 0.5 * (y + x / y); return y; }
static double dabs(double x) { return x < 0 ? -x : x; }
static uint32_t seed = 12345;
static float frand(void) { seed = seed * 1664525U + 1013904223U; return (float)(int32_t)seed * (1.0f / 2147483648.0f); }

#ifndef T_HASH_ONLY
/* ---- unit level: run_verb on float blocks, wet only ---- */
static void verb_start(unsigned type, unsigned decay, unsigned pre)
{
    mfx_reset();
    ep[MFX_VERB][0] = (uint8_t)type; ep[MFX_VERB][1] = (uint8_t)decay; ep[MFX_VERB][2] = (uint8_t)pre; ep[MFX_VERB][3] = 127;
    mfx_wr16_clamps = 0;
}
static int subnormal(float x) { float a = x < 0 ? -x : x; return a != 0.0f && a < 1.17549435e-38f; }
static int states_subnormal(void)
{
    for (unsigned k = 0; k < 8; ++k) if (subnormal(rv.f[k])) return 1;
    return subnormal(rv.hp) || subnormal(rv.lp) || subnormal(rv.last[0]) || subnormal(rv.last[1]) || subnormal(rv.prevx);
}
static int bad_state;
static void verb_block(const float *inl, const float *inr, float *wl, float *wr)
{
    static float l[FM1_BLOCK_FRAMES], r[FM1_BLOCK_FRAMES];
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) { l[i] = inl[i]; r[i] = inr[i]; }
    run_verb(l, r, 1.0f);
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) { wl[i] = l[i] - inl[i]; wr[i] = r[i] - inr[i]; }
    bad_state |= states_subnormal();
}
enum { IRN = 6 * SR / FM1_BLOCK_FRAMES * FM1_BLOCK_FRAMES };
static float in_l[IRN], in_r[IRN], w_l[IRN], w_r[IRN], tmp[IRN];
static double edc[IRN];
static void render(unsigned n)
{
    for (unsigned o = 0; o + FM1_BLOCK_FRAMES <= n; o += FM1_BLOCK_FRAMES) verb_block(in_l + o, in_r + o, w_l + o, w_r + o);
}
static void bandpass(const float *x, float *y, unsigned n, double f0) /* RBJ band-pass, ~1 octave */
{
    double w = 2 * PI_D * f0 / SR, al = dsin(w) / (2 * 1.41), a0 = 1 + al;
    double b0 = al / a0, b2 = -al / a0, a1 = -2 * dcos(w) / a0, a2 = (1 - al) / a0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (unsigned i = 0; i < n; ++i) { double v = b0 * x[i] + b2 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = x[i]; y2 = y1; y1 = v; y[i] = (float)v; }
}
static double rt60(const float *x, unsigned n) /* Schroeder, fit -5 .. -25 dB */
{
    double s = 0;
    for (unsigned i = n; i-- > 0;) { s += (double)x[i] * x[i]; edc[i] = s; }
    if (edc[0] <= 0) return 0;
    long i5 = -1;
    for (unsigned i = 0; i < n; ++i) {
        double q = edc[i] / edc[0];
        if (i5 < 0 && q <= 0.31622777) i5 = (long)i;
        if (q <= 0.0031622777) return i5 < 0 ? 0 : 3.0 * (double)((long)i - i5) / SR;
    }
    return -1;
}
/* FFT for the tail bandwidth */
enum { NFFT = 16384 };
static double fre[NFFT], fim[NFFT], pw[NFFT / 2];
static void fft(void)
{
    for (unsigned i = 1, j = 0; i < NFFT; ++i) {
        unsigned b = NFFT >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j ^= b;
        if (i < j) { double t = fre[i]; fre[i] = fre[j]; fre[j] = t; t = fim[i]; fim[i] = fim[j]; fim[j] = t; }
    }
    for (unsigned len = 2; len <= NFFT; len <<= 1) {
        double a = -2 * PI_D / len, wr = dcos(a), wi = dsin(a);
        for (unsigned i = 0; i < NFFT; i += len) {
            double cr = 1, ci = 0;
            for (unsigned k = 0; k < len / 2; ++k) {
                unsigned p = i + k, q = p + len / 2;
                double vr = fre[q] * cr - fim[q] * ci, vi = fre[q] * ci + fim[q] * cr;
                fre[q] = fre[p] - vr; fim[q] = fim[p] - vi; fre[p] += vr; fim[p] += vi;
                double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}
typedef struct { double rt500, rt6k, ned100, tmix, bw, corr; } metrics;
static metrics measure(unsigned type)
{
    metrics m = {0};
    /* burst: 1 s of low-passed noise at about -12 dBFS, then silence; RT60 of the tail from the stop */
    verb_start(type, 80, 0);
    float lp = 0;
    for (unsigned i = 0; i < IRN; ++i) { if (i < SR) { lp += 0.3f * (frand() - lp); in_l[i] = in_r[i] = lp * 0.9f; } else in_l[i] = in_r[i] = 0; }
    render(IRN);
    bandpass(w_l, tmp, IRN, 500); m.rt500 = rt60(tmp + SR, IRN - SR);
    bandpass(w_l, tmp, IRN, 6000); m.rt6k = rt60(tmp + SR, IRN - SR);
    /* impulse response: echo density, mixing time, bandwidth, correlation */
    verb_start(type, 80, 0);
    for (unsigned i = 0; i < IRN; ++i) in_l[i] = in_r[i] = i < 2 ? 1.0f : 0.0f;
    render(IRN);
    unsigned on = 0; double pk = 0;
    for (unsigned i = 0; i < IRN; ++i) if (dabs(w_l[i]) > pk) pk = dabs(w_l[i]);
    for (unsigned i = 0; i < IRN; ++i) if (dabs(w_l[i]) > pk * 0.01) { on = i; break; }
    const unsigned W = 882; /* 20 ms window */
    m.tmix = -1;
    for (unsigned c = on + W / 2; c + W / 2 < IRN && c < on + SR; c += 64) {
        double s2 = 0; for (unsigned i = c - W / 2; i < c + W / 2; ++i) s2 += (double)w_l[i] * w_l[i];
        double sd = dsqrt(s2 / W); unsigned cnt = 0;
        for (unsigned i = c - W / 2; i < c + W / 2; ++i) if (dabs(w_l[i]) > sd) ++cnt;
        double ned = cnt / (double)W / 0.3173, t = (double)(c - on) / SR;
        if (t <= 0.1) m.ned100 = ned;
        if (m.tmix < 0 && ned >= 0.9) m.tmix = t;
    }
    double sxy = 0, sxx = 0, syy = 0;
    for (unsigned i = on + SR * 8 / 100; i < on + SR; ++i) { sxy += (double)w_l[i] * w_r[i]; sxx += (double)w_l[i] * w_l[i]; syy += (double)w_r[i] * w_r[i]; }
    m.corr = sxy / (dsqrt(sxx * syy) + 1e-30);
    unsigned st = on + SR * 15 / 100;
    for (unsigned i = 0; i < NFFT; ++i) { double w = 0.5 - 0.5 * dcos(2 * PI_D * i / NFFT); fre[i] = w_l[st + i] * w; fim[i] = 0; }
    fft();
    for (unsigned i = 0; i < NFFT / 2; ++i) pw[i] = fre[i] * fre[i] + fim[i] * fim[i];
    double ref = 0; unsigned rc = 0;
    for (unsigned i = 500 * NFFT / SR; i < 2000 * NFFT / SR; ++i) { ref += pw[i]; ++rc; }
    ref /= rc;
    for (double f = 2000; f < 21000; f *= 1.06) {
        unsigned a = (unsigned)(f / 1.12 * NFFT / SR), b = (unsigned)(f * 1.12 * NFFT / SR); double s = 0;
        for (unsigned i = a; i < b && i < NFFT / 2; ++i) s += pw[i];
        if (s / (b - a) > 0.01 * ref) m.bw = f; /* within 20 dB of the 0.5-2 kHz level */
    }
    return m;
}
/* Expected (research A presets at DECAY 80; RT60 = tmin * (tmax / tmin)^(80/127)): ROOM 0.81 s, HALL 2.97 s,
 * PLATE (= AMBIENT) 7.4 s. The 16-bit lines (truncation) shorten the measured tail; the bounds allow that. */
static int metrics_case(void)
{
    static const double rt_lo[3] = {0.45, 1.6, 3.5}, rt_hi[3] = {1.1, 3.6, 9.0};
    static const double bw_lo[3] = {5000, 5000, 4500}, bw_hi[3] = {11500, 11500, 11500};
    static const double tmix_max[3] = {0.06, 0.13, 0.09};
    for (unsigned t = 0; t < 3; ++t) {
        metrics m = measure(t);
        if (m.rt500 < rt_lo[t] || m.rt500 > rt_hi[t]) return 10 + (int)t * 3;
        if (m.rt6k > 0.92 * m.rt500 || m.rt6k < 0.3 * m.rt500) return 10 + (int)t * 3; /* the tail darkens, not dead */
        if (m.ned100 < 0.85 || m.tmix < 0 || m.tmix > tmix_max[t]) return 11 + (int)t * 3;
        if (m.bw < bw_lo[t] || m.bw > bw_hi[t] || m.corr > 0.3 || m.corr < -0.3) return 12 + (int)t * 3;
    }
    return 0;
}
/* 19: the modulated line moves the tail: a steady 1 kHz sine through each type at DECAY 127; over the last
 * second (exactly 1000 periods) the wet part that is not a 1 kHz sine (an unmodulated network is linear and
 * time-invariant: only truncation noise) must hold at least `min` of the wet power. Returns the ratio. */
static double mod_ratio(unsigned type)
{
    static float il[FM1_BLOCK_FRAMES], ol[FM1_BLOCK_FRAMES], orr[FM1_BLOCK_FRAMES];
    verb_start(type, 127, 0);
    double c = dcos(2 * PI_D * 1000.0 / SR), y0 = 0, y1 = -dsin(2 * PI_D * 1000.0 / SR), sa = 0, ca = 0, pw = 0;
    unsigned n = 0, total = 4 * SR / FM1_BLOCK_FRAMES * FM1_BLOCK_FRAMES, from = total - SR;
    static float keep[SR];
    for (unsigned b = 0; b < total / FM1_BLOCK_FRAMES; ++b) {
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) { double y = 2 * c * y0 - y1; y1 = y0; y0 = y; il[i] = (float)(0.3 * y); }
        verb_block(il, il, ol, orr);
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i, ++n) if (n >= from) keep[n - from] = ol[i];
    }
    for (unsigned i = 0; i < SR; ++i) { double ph = 2 * PI_D * 1000.0 * i / SR; sa += keep[i] * dsin(ph); ca += keep[i] * dcos(ph); pw += (double)keep[i] * keep[i]; }
    double tone = 2 * (sa * sa + ca * ca) / SR; /* power of the 1 kHz part */
    return pw > 0 ? (pw - tone) / pw : 0;
}
static int mod_case(void)
{
    static const double min[3] = {0.00008, 0.05, 0.08}; /* measured 0.00016 / 0.26 / 0.17; unmodulated 0.00003 / 0.0004 / 0.025 */
    for (unsigned t = 0; t < 3; ++t) if (mod_ratio(t) < min[t]) return 19;
    return 0;
}
/* 20: no line clamps on a full-scale sine anywhere in 25 Hz .. 4 kHz at DECAY 0 / 64 / 127 (2 s each). */
static int sweep_case(void)
{
    static float zl[FM1_BLOCK_FRAMES], zr[FM1_BLOCK_FRAMES], sl[FM1_BLOCK_FRAMES];
    for (unsigned t = 0; t < 3; ++t)
        for (unsigned d = 0; d <= 127; d += 63 + (d == 63)) /* 0, 63, 127 */
            for (double f = 25; f < 4000; f *= 1.04) {
                verb_start(t, d, 0);
                double dp = 2 * PI_D * f / SR;
                float c = (float)dcos(dp), y0 = 0, y1 = (float)-dsin(dp); /* sine by recursion */
                for (unsigned b = 0; b < 2 * SR / FM1_BLOCK_FRAMES; ++b) {
                    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) { float y = 2 * c * y0 - y1; y1 = y0; y0 = y; sl[i] = y; }
                    verb_block(sl, sl, zl, zr);
                }
                if (mfx_wr16_clamps) return 20;
            }
    return 0;
}
/* 21 / 22: fuzz with random settings and harsh inputs (up to +-4 SR, DC steps, clicks, square waves). */
static int fuzz_case(void)
{
    static float il[FM1_BLOCK_FRAMES], ir[FM1_BLOCK_FRAMES], ol[FM1_BLOCK_FRAMES], orr[FM1_BLOCK_FRAMES];
    bad_state = 0;
    for (unsigned run = 0; run < 120; ++run) {
        verb_start((unsigned)(frand() * 1.5f + 1.5f) % 3U, (unsigned)((frand() + 1) * 63.5f), (unsigned)((frand() + 1) * 63.5f));
        unsigned kind = run % 5;
        float amp = (frand() + 1.0f) * 2.0f;
        for (unsigned b = 0; b < 400; ++b) {
            if (b % 50 == 0) { ep[MFX_VERB][1] = (uint8_t)((frand() + 1) * 63.5f); ep[MFX_VERB][2] = (uint8_t)((frand() + 1) * 63.5f); }
            for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
                unsigned n = b * FM1_BLOCK_FRAMES + i;
                float v = kind == 0 ? amp * frand() : kind == 1 ? ((n / 300) & 1 ? amp : -amp) : kind == 2 ? (n % 9000 < 2 ? amp : 0)
                        : kind == 3 ? (b < 200 ? amp : -amp) : amp * frand() * (b < 100);
                il[i] = v; ir[i] = kind == 4 ? -v : v;
            }
            verb_block(il, ir, ol, orr);
            for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
                if (!(ol[i] == ol[i]) || !(orr[i] == orr[i])) return 21;
                if (dabs(ol[i]) > 64 || dabs(orr[i]) > 64) return 21;
            }
        }
    }
    return bad_state ? 22 : 0;
}
/* 30: after a loud burst at the longest setting the tail reaches exact 0: output, every line, every state. */
static int silence_case(void)
{
    static float il[FM1_BLOCK_FRAMES], ir[FM1_BLOCK_FRAMES], ol[FM1_BLOCK_FRAMES], orr[FM1_BLOCK_FRAMES];
    for (unsigned t = 0; t < 3; ++t) {
        verb_start(t, 127, 127);
        bad_state = 0;
        unsigned zero_run = 0, b;
        for (b = 0; b < 60 * SR / FM1_BLOCK_FRAMES; ++b) {
            for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) il[i] = ir[i] = b < 340 ? 0.8f * frand() : 0.0f;
            verb_block(il, ir, ol, orr);
            unsigned z = 1;
            for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) if (ol[i] != 0.0f || orr[i] != 0.0f) z = 0;
            zero_run = z && b >= 340 ? zero_run + 1 : 0;
            if (zero_run >= 345) break; /* 1 s of exact silence */
        }
        if (zero_run < 345 || bad_state) return 30;
        for (unsigned i = 0; i < RV_LINE_TOTAL; ++i) if (rv_line[i]) return 30;
        for (unsigned i = 0; i < RV_DIF_TOTAL; ++i) if (rv_dif[i]) return 30;
        for (unsigned i = 0; i < RV_PRE_MAX; ++i) if (rv_pre[i]) return 30;
        for (unsigned k = 0; k < 8; ++k) if (rv.f[k] != 0.0f) return 30;
        if (rv.hp != 0.0f || rv.lp != 0.0f || rv.last[0] != 0.0f || rv.last[1] != 0.0f) return 30;
    }
    return 0;
}

#endif
/* ---- integration: through mfx_process (fades, sliced clear, shed) ---- */
static int32_t buf[FM1_BLOCK_WORDS], dry[FM1_BLOCK_WORDS];
static void set(unsigned fx, unsigned param, int value) { mfx_edit(fx, param, -255); mfx_edit(fx, param, value); }
static unsigned long gen_n;
static void fill(double amp, double hz)
{
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i, ++gen_n) {
        int32_t v = (int32_t)(amp * dsin(2 * PI_D * hz * (double)gen_n / SR) * 8388608.0);
        buf[2 * i] = buf[2 * i + 1] = dry[2 * i] = dry[2 * i + 1] = v;
    }
}
typedef struct { double y1, y2, d2, d2_edge; unsigned n; } meter;
static void meter_block(meter *m, const int32_t *w)
{
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i, ++m->n) {
        double y = w[2 * i] / 8388608.0;
        if (m->n >= 2) {
            double d2 = dabs(y - 2 * m->y1 + m->y2);
            if (d2 > m->d2) m->d2 = d2;
            if (i <= 1 && d2 > m->d2_edge) m->d2_edge = d2;
        }
        m->y2 = m->y1; m->y1 = y;
    }
}
#ifndef T_HASH_ONLY
/* 40 / 41 / 42: REVERB at DECAY 127 on a 0.5 SR 440 Hz sine: TYPE ROOM -> HALL -> PLATE -> ROOM, then shed by
 * the load guard and back; every block edge within the dry sine's own largest second difference + 0.004 SR
 * (the limit of tests/t_master_fx.c fade_case), at most the clear slice zeroed per block. */
static int switch_case(unsigned lowcut)
{
    mfx_reset(); gen_n = 0;
    set(MFX_VERB, 0, 0); set(MFX_VERB, 1, 127); set(MFX_VERB, 2, 0); set(MFX_VERB, 3, 127);
    /* LOW CUT on keeps the float path all through. Off (case 43), REVERB is the only unit: while it is cleared,
     * shed or at MIX 0 mfx_run takes the integer path, and the 20 Hz filter must fade out there instead of
     * dropping its state (0.026-0.045 FS step before the fix; the old reverb 0.093 FS). */
    mfx_lowcut_set(lowcut);
    if (!lowcut) mfx_comp_edit(3, -255); /* COMP off too (case 50 leaves it on): REVERB is the only unit */
    meter m = {0}, d = {0};
    unsigned blocks = lowcut ? 1500 : 1700;
    for (unsigned b = 0; b < blocks; ++b) {
        /* MIX turned down to 0 (the last detents one by one: the reverb's own wet stops at once at MIX 0, so a
         * big last detent cuts its wet; not the 20 Hz filter's), back up 3 blocks after it reached 0 (inside the
         * filter's fade-out), down again for good */
        if ((b >= 1500 && b < 1507) || (b >= 1580 && b < 1587)) mfx_edit(MFX_VERB, 3, -16); /* off: knob down fast to 15, */
        if ((b >= 1507 && b < 1522) || (b >= 1587 && b < 1602)) mfx_edit(MFX_VERB, 3, -1);  /* then detent by detent to 0 */
        if (b >= 1524 && b < 1532) mfx_edit(MFX_VERB, 3, 16); /* back on inside the 20 Hz filter's fade-out */
        if (b == 300 || b == 600 || b == 900) mfx_edit(MFX_VERB, 0, b == 900 ? -2 : 1);
        if (b == 1100) mfx_shed(1U << MFX_VERB, 0);
        if (b == 1250) mfx_shed(0, 0);
        unsigned z0 = mfx_test_zeroed, pp = rv_ppos;
        fill(0.5, 440.0); mfx_process(buf);
        if (b >= 1100 + MFX_SHED_FADE + 2 && b < 1250 && (rv_ppos != pp || fx_heard[MFX_VERB] != 0.0f)) return 42; /* out: not run */
        if (mfx_test_zeroed - z0 > FX_CLEAR_BUDGET) return 41;
        if (b >= 100) { meter_block(&m, buf); meter_block(&d, dry); }
        if (b == 1100 + MFX_SHED_FADE + 2 && !((mfx_units_silent() >> MFX_VERB) & 1U)) return 42;
        if (b == 1253 && (fx_dirty[MFX_VERB] ? fx_gain[MFX_VERB] != 0.0f : 0)) return 42;
        if (b == 1499 && (fx_dirty[MFX_VERB] || fx_gain[MFX_VERB] != 1.0f)) return 42; /* back, cleared, faded in */
        if (b == 1690) { /* long after MIX 0: the integer path at rest, as before the fix */
            for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) if (buf[i] != dry[i]) return 44;
            if (sub_g != 0.0f || sub_hp[0][0] != 0.0f || sub_hp[0][1] != 0.0f || sub_hp[1][0] != 0.0f || sub_hp[1][1] != 0.0f) return 44;
        }
    }
    mfx_lowcut_set(0);
    return m.d2_edge > d.d2 + 0.004 ? (lowcut ? 40 : 43) : 0;
}
#endif
#ifndef T_HASH_ONLY
/* 45: REVERB alone on a 1.5 FS 440 Hz sine (past full scale: run_output_int limits it), MIX turned to 0; the 20 blocks
 * of the integer path from there are run twice, once as is (the 20 Hz filter fades out) and once with the fade
 * skipped (sub_g = 0, state 0 before each of those blocks = the limiter alone). The fade may only add its correction:
 * no more samples at the 24-bit rail than the limiter alone, and every sample within the correction of it: 0.2 FS in
 * the first block (the limiter has not stepped down yet; measured 0.134), 0.1 FS after it (measured 0.071). A 24-bit
 * clamp ahead of the limiter cuts the 1.5 FS peaks to 1 FS: 0.37 FS off after the first block. */
static int32_t hot_a[20 * FM1_BLOCK_WORDS], hot_b[20 * FM1_BLOCK_WORDS];
static void hot_run(int32_t *out, unsigned skip_fade)
{
    mfx_reset(); gen_n = 0;
    set(MFX_VERB, 0, 0); set(MFX_VERB, 1, 127); set(MFX_VERB, 2, 0); set(MFX_VERB, 3, 127);
    mfx_lowcut_set(0); mfx_comp_edit(3, -255);
    unsigned at = 0;
    for (unsigned b = 0; b < 360; ++b) {
        if (b >= 300 && b < 307) mfx_edit(MFX_VERB, 3, -16);
        if (b >= 307 && b < 322) mfx_edit(MFX_VERB, 3, -1); /* MIX 0 from block 321 on */
        if (skip_fade && b >= 321) { sub_g = 0.0f; subcut_rest(); }
        fill(1.5, 440.0); mfx_process(buf);
        if (b >= 321 && at < 20) { for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) out[at * FM1_BLOCK_WORDS + i] = buf[i]; ++at; }
    }
}
static int hot_case(void)
{
    hot_run(hot_a, 0);
    float g_left = sub_g; /* 0 after the 16-block fade */
    hot_run(hot_b, 1);
    if (g_left != 0.0f) return 45;
    unsigned rail_a = 0, rail_b = 0;
    double worst = 0, moved = 0;
    for (unsigned i = 0; i < 20 * FM1_BLOCK_WORDS; ++i) {
        rail_a += hot_a[i] >= 8388607 || hot_a[i] <= -8388607;
        rail_b += hot_b[i] >= 8388607 || hot_b[i] <= -8388607;
        double d = dabs((double)hot_a[i] - (double)hot_b[i]) / 8388608.0;
        if (d > (i < FM1_BLOCK_WORDS ? 0.2 : 0.1)) worst = 1.0;
        if (i < 16 * FM1_BLOCK_WORDS) moved += d;
    }
    if (rail_a > rail_b || worst > 0.0 || moved == 0.0) return 45; /* moved: the fade did run */
    return 0;
}
#endif
/* 50: MIX 0 = the dry path of the code before this reverb: FILTER ACID, CHORUS and COMP on, REVERB HALL set up
 * with MIX 0 (TYPE and DECAY turned on the way), 500 blocks of a saw + noise, FNV-1a hash of the output. */
uint32_t mix0_hash(void)
{
    mfx_reset(); gen_n = 0; seed = 777;
    set(MFX_FILTER, 0, 0); set(MFX_FILTER, 1, 60); set(MFX_FILTER, 2, 40); set(MFX_FILTER, 3, 100);
    set(MFX_CHORUS, 3, 64); mfx_comp_edit(3, 100);
    set(MFX_VERB, 0, 1); set(MFX_VERB, 1, 100); set(MFX_VERB, 2, 30); set(MFX_VERB, 3, 0);
    uint32_t h = 2166136261U;
    for (unsigned b = 0; b < 500; ++b) {
        if (b == 200) { set(MFX_VERB, 0, 2); set(MFX_VERB, 1, 127); } /* turning TYPE / DECAY at MIX 0 changes nothing */
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i, ++gen_n) {
            int32_t v = (int32_t)((gen_n % 401U) * 20000U) - 4000000 + (int32_t)(frand() * 300000.0f);
            buf[2 * i] = v; buf[2 * i + 1] = -v / 2;
        }
        mfx_process(buf);
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) { h ^= (uint32_t)buf[i]; h *= 16777619U; }
    }
    return h;
}
#ifndef MIX0_REF
#define MIX0_REF 0xC962F0ECU /* measured with app/master_fx.c of 92398aa (tests/run_reverb.sh --ref prints it) */
#endif
#ifndef T_NO_MAIN
int main(void)
{
    int e;
    if (mix0_hash() != MIX0_REF) return 50;
    if ((e = metrics_case())) return e;
    if ((e = mod_case())) return e;
    if ((e = sweep_case())) return e;
    if ((e = fuzz_case())) return e;
    if ((e = silence_case())) return e;
    if ((e = switch_case(1))) return e;
    if ((e = switch_case(0))) return e;
    if ((e = hot_case())) return e;
    return 0;
}
#endif
