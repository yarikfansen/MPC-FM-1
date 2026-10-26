/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Master insert effects on the final stereo mix: filter, drive, chorus/flanger,
 * phaser, ring modulator, reverb and a 3-band EQ, then an optional output
 * boost with a peak limiter. Controls are bytes written by the control task
 * and read once per block by the audio ISR; coefficients follow per block. */
#include "master_fx.h"
#include <string.h>

#define FS 44100.0f
#define PI_F 3.14159265358979f
enum { COMB_LEN = 1200, MOD_LEN = 2048 };

static volatile int16_t mods[MFX_COUNT][4]; /* LFO offsets, control task */
void mfx_mod_set(unsigned fx, unsigned param, int offset)
{
    if (fx < MFX_COUNT && param < 4) mods[fx][param] = (int16_t)(offset < -127 ? -127 : offset > 127 ? 127 : offset);
}
static const uint8_t type_count[MFX_COUNT] = {5, 4, 2, 3, 8, 3, 5};
static const uint8_t defaults[MFX_COUNT][4] = {
    {0, 127, 20, 0}, {0, 40, 100, 0}, {0, 40, 60, 0}, {0, 30, 50, 0}, {0, 60, 0, 0}, {1, 70, 20, 0}, {2, 64, 64, 64}};
/* Controls start at the defaults, so the effects need no first-use setup. */
static volatile uint8_t prm[MFX_COUNT][4] = {
    {0, 127, 20, 0}, {0, 40, 100, 0}, {0, 40, 60, 0}, {0, 30, 50, 0}, {0, 60, 0, 0}, {1, 70, 20, 0}, {2, 64, 64, 64}};
static const char *const names[MFX_COUNT] = {"FILTER", "DRIVE", "CHORUS", "PHASER", "RING", "REVERB", "EQ"};
static const char *const labels[MFX_COUNT][4] = {
    {"TYPE", "CUTOFF", "RESO", "MIX"}, {"TYPE", "DRIVE", "TONE", "MIX"},
    {"TYPE", "RATE", "DEPTH", "MIX"}, {"STAGES", "RATE", "FEEDBK", "MIX"},
    {"WAVE", "FREQ", "SPREAD", "MIX"}, {"TYPE", "DECAY", "PREDELAY", "MIX"},
    {"MID HZ", "LOW", "MID", "HIGH"}};
static const char *const types[MFX_COUNT][8] = {
    {"ACID", "SEM", "LADDER", "COMB", "DJ"}, {"SOFT", "HARD", "FOLD", "CRUSH"},
    {"CHORUS", "FLANGE"}, {"4 ST", "6 ST", "8 ST"}, {"SINE", "SQUARE", "TRI", "SAW", "NOISE", "TRACK", "SHIFT", "ENV"}, {"ROOM", "HALL", "PLATE"},
    {"250", "500", "1K", "2K", "4K"}};

/* DSP state (ordinary BSS). */
static float svf1[2], svf2[2], comb[2][COMB_LEN], tone[2], hold[2];
static float modbuf[2][MOD_LEN], ap[2][8], ph_fb[2];
static float cut_smooth = -1.0f;
static unsigned comb_pos, mod_pos, crush_count;
static float lfo_chorus, lfo_phaser, ring_phase[2];
/* ACID / LADDER: four saturating one-pole stages per channel, run at 2x. */
static float ld[2][4], ld_in[2], ld_hp[2];
/* Ring: noise carrier, pitch tracking, envelope, Hilbert pair for SHIFT. */
static float rn_a[2], rn_b[2], trk_freq = 220.0f, trk_last, trk_lp, trk_env[2], hb_s[2][2][4][4], hb_x[2];
static uint32_t rn_seed = 0x1234567U;
static unsigned trk_count, trk_cross;
/* Reverb: runs at half rate (22.05 kHz) to stay light on the CPU: mono
 * pre-delay, 4 damped combs and 2 allpasses per side, 16-bit lines, no
 * divisions or modulo in the sample loop. Upsampled linearly. */
enum { RV_COMBS = 4, RV_APS = 2, RV_PRE_MAX = 2205, RV_SPREAD = 12,
       RV_COMB_TOTAL = (558 + 594 + 638 + 678) + RV_COMBS * RV_SPREAD, RV_AP_TOTAL = (278 + 220) + RV_APS * RV_SPREAD };
static const uint16_t rv_comb_len[RV_COMBS] = {558, 594, 638, 678}, rv_ap_len[RV_APS] = {278, 220};
static int16_t rv_comb[2][RV_COMB_TOTAL], rv_ap[2][RV_AP_TOTAL], rv_pre[RV_PRE_MAX];
static uint16_t rv_cpos[2][RV_COMBS], rv_apos[2][RV_APS], rv_ppos;
static float rv_damp[2][RV_COMBS], rv_last[2];
static uint8_t ep[MFX_COUNT][4];
static float svf3[2], svf4[2], cut_prev = -1.0f, dc_x[2], dc_y[2], drv_prev[2];
static void ensure_ready(void);
static volatile uint8_t lowcut_on, out_step = 2; /* +6 dB by default */
/* EQ biquads (3 bands x 2 channels) and the output limiter. */
static float eq_c[3][5], eq_z[3][2][2], lim_gain = 1.0f;
static uint8_t eq_key[4] = {255, 255, 255, 255};
static float lc1[2], lc2[2];
void mfx_lowcut_set(unsigned on) { lowcut_on = on != 0; }
unsigned mfx_lowcut(void) { return lowcut_on; }
void mfx_output_set(unsigned step) { ensure_ready(); out_step = (uint8_t)(step > 4 ? 4 : step); }
unsigned mfx_output(void) { return out_step; }
unsigned mfx_active(unsigned fx)
{
    ensure_ready();
    if (fx == MFX_EQ) return prm[fx][1] != 64 || prm[fx][2] != 64 || prm[fx][3] != 64;
    return fx < MFX_COUNT && prm[fx][3] != 0;
}

void mfx_reset(void)
{
    memcpy((void *)prm, defaults, sizeof defaults);
    memset(svf1, 0, sizeof svf1); memset(svf2, 0, sizeof svf2); memset(comb, 0, sizeof comb);
    memset(tone, 0, sizeof tone); memset(hold, 0, sizeof hold); memset(modbuf, 0, sizeof modbuf);
    memset(ap, 0, sizeof ap); memset(ph_fb, 0, sizeof ph_fb);
    cut_smooth = -1.0f; comb_pos = mod_pos = crush_count = 0;
    lfo_chorus = lfo_phaser = ring_phase[0] = ring_phase[1] = 0;
    memset(ld, 0, sizeof ld); memset(ld_in, 0, sizeof ld_in); memset(ld_hp, 0, sizeof ld_hp);
    memset(hb_s, 0, sizeof hb_s); memset(hb_x, 0, sizeof hb_x); memset(trk_env, 0, sizeof trk_env);
    memset(rv_comb, 0, sizeof rv_comb); memset(rv_ap, 0, sizeof rv_ap); memset(rv_pre, 0, sizeof rv_pre);
    memset(rv_cpos, 0, sizeof rv_cpos); memset(rv_apos, 0, sizeof rv_apos); memset(rv_damp, 0, sizeof rv_damp);
    rv_last[0] = rv_last[1] = 0;
    rv_ppos = 0; memset(lc1, 0, sizeof lc1); memset(lc2, 0, sizeof lc2);
    memset(svf3, 0, sizeof svf3); memset(svf4, 0, sizeof svf4); memset(dc_x, 0, sizeof dc_x); memset(dc_y, 0, sizeof dc_y);
    memset(drv_prev, 0, sizeof drv_prev); cut_prev = -1.0f;
    memset(eq_z, 0, sizeof eq_z); memset(eq_key, 255, sizeof eq_key); lim_gain = 1.0f;
}
static void ensure(void) { }
static void ensure_ready(void) { ensure(); }
void mfx_edit(unsigned fx, unsigned param, int delta)
{
    ensure();
    if (fx >= MFX_COUNT || param >= 4 || !delta) return;
    int v = prm[fx][param];
    /* Every detent counts, also for the type: a fast turn must not feel stuck. */
    if (!param) v += delta, v = v < 0 ? 0 : v >= type_count[fx] ? type_count[fx] - 1 : v;
    else v += delta, v = v < 0 ? 0 : v > 127 ? 127 : v;
    prm[fx][param] = (uint8_t)v;
}
unsigned mfx_value(unsigned fx, unsigned param) { ensure(); return fx < MFX_COUNT && param < 4 ? prm[fx][param] : 0; }
const char *mfx_name(unsigned fx) { return fx < MFX_COUNT ? names[fx] : ""; }
const char *mfx_label(unsigned fx, unsigned param) { return fx < MFX_COUNT && param < 4 ? labels[fx][param] : ""; }
const char *mfx_type_name(unsigned fx) { ensure(); return fx < MFX_COUNT ? types[fx][prm[fx][0] % type_count[fx]] : ""; }

/* ---- small math, self-contained ---- */
static inline __attribute__((always_inline)) float fexp2(float x)
{
    if (x < -60) x = -60; if (x > 60) x = 60;
    int n = (int)(x + (x < 0 ? -0.5f : 0.5f));
    float a = (x - (float)n) * 0.6931472f;
    float p = 1 + a * (1 + a * (0.5f + a * (0.1666667f + a * (0.0416667f + a * 0.0083333f))));
    union { uint32_t u; float f; } s = {(uint32_t)(n + 127) << 23};
    return p * s.f;
}
static inline __attribute__((always_inline)) float fsin_cycles(float c) /* sin(2*pi*c) */
{
    c -= (float)(int)c; if (c < 0) c += 1;
    float a = (c > 0.5f ? c - 1.0f : c) * 2 * PI_F;
    if (a > PI_F * 0.5f) a = PI_F - a; else if (a < -PI_F * 0.5f) a = -PI_F - a;
    float q = a * a;
    return a * (1 + q * (-0.1666667f + q * (0.0083333f + q * (-0.0001984f + q * 0.0000028f))));
}
static inline __attribute__((always_inline)) float ftan_pi(float x) /* tan(pi*x), 0 <= x < 0.5 */
{
    return fsin_cycles(x * 0.5f) / fsin_cycles(x * 0.5f + 0.25f);
}
static inline __attribute__((always_inline)) float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline __attribute__((always_inline)) float fabsf_(float x) { return x < 0 ? -x : x; }
static FM1_AUDIO_CODE float fsqrt_(float x) /* x > 0, a few Newton steps from a bit-level guess */
{
    union { float f; uint32_t u; } v = {x};
    v.u = (v.u >> 1) + 0x1fbd1df5U;
    float y = v.f;
    y = 0.5f * (y + x / y); y = 0.5f * (y + x / y); return 0.5f * (y + x / y);
}

int mfx_cutoff_hz(void)
{
    ensure();
    unsigned type = prm[MFX_FILTER][0];
    float cut = prm[MFX_FILTER][1] * (1.0f / 127);
    if (type == 3) return (int)(40.0f * fexp2(cut * 5.6f) + 0.5f);                 /* COMB */
    if (type == 0) return (int)clampf(40.0f * fexp2(cut * 7.6f), 20.0f, 9000.0f);  /* ACID */
    if (type == 2) return (int)clampf(30.0f * fexp2(cut * 8.7f), 20.0f, 12000.0f); /* LADDER */
    if (type == 4) {                                                                /* DJ */
        float d = (prm[MFX_FILTER][1] - 64) * (1.0f / 63);
        if (d > -0.02f && d < 0.02f) return 0;
        return d < 0 ? (int)(18000.0f * fexp2(d * 9.0f)) : -(int)(20.0f * fexp2(d * 9.5f));
    }
    float fc = 30.0f * fexp2(cut * 9.4f);
    return (int)clampf(fc, 20.0f, 18000.0f);
}
/* ---- effects, each over one block of float L/R (effective controls in ep[][]) ---- */
static inline __attribute__((always_inline)) float ftanh(float x) /* rational tanh, |err| < 2.5% */
{
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
/* Four saturating one-pole stages with resonance feedback, 2x oversampled.
 * LADDER: Moog-style 24 dB, bass held up as resonance rises, self-oscillates
 * at the top. ACID: 303-flavoured - driven input, a high-pass in the loop
 * (the bass thins and the peak squelches), more resonance, earlier scream. */
static FM1_AUDIO_CODE void run_ladder(float **ch, unsigned acid, float cut, float res, float mix)
{
    /* 1x rate (half the CPU of the first version); the top cutoff is kept
     * where the four stages stay stable at full resonance. */
    float fc = acid ? 40.0f * fexp2(cut * 7.6f) : 30.0f * fexp2(cut * 8.7f);
    fc = clampf(fc, 20.0f, acid ? 9000.0f : 12000.0f);
    if (cut_prev < 0) cut_prev = fc;
    float w0 = 2.0f * PI_F * cut_prev / FS, w1 = 2.0f * PI_F * fc / FS;
    float g0 = w0 / (1.0f + w0), g1 = w1 / (1.0f + w1);
    cut_prev = fc;
    /* ACID: softer top than before (scream just before full), LADDER: self-oscillates at the top. */
    float k = acid ? 3.9f * res : 4.1f * res, drive = acid ? 1.9f : 1.15f;
    float comp = acid ? 1.0f + 0.3f * k : 1.0f + 0.55f * k;
    for (unsigned c = 0; c < 2; ++c) {
        float y0 = ld[c][0], y1 = ld[c][1], y2 = ld[c][2], y3 = ld[c][3], hp = ld_hp[c];
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
            float g = g0 + (g1 - g0) * ((float)i * (1.0f / FM1_BLOCK_FRAMES)), x = ch[c][i];
            float fb = y3;
            if (acid) { hp += 0.024f * (fb - hp); fb -= hp; } /* ~170 Hz high-pass in the loop */
            float u = ftanh(x * drive - k * fb) + 1e-20f;
            float t0 = ftanh(y0), t1 = ftanh(y1), t2 = ftanh(y2), t3 = ftanh(y3);
            y0 += g * (u - t0); y1 += g * (t0 - t1); y2 += g * (t1 - t2); y3 += g * (t2 - t3);
            float y = y3 * comp / drive;
            ch[c][i] = x + mix * (y - x);
        }
        ld[c][0] = y0; ld[c][1] = y1; ld[c][2] = y2; ld[c][3] = y3; ld_hp[c] = hp;
    }
}
static FM1_AUDIO_CODE void run_filter(float *l, float *r, float mix)
{
    unsigned type = ep[MFX_FILTER][0];
    float cut = ep[MFX_FILTER][1] * (1.0f / 127), res = ep[MFX_FILTER][2] * (1.0f / 127);
    if (cut_smooth < 0) cut_smooth = cut;
    float *ch[2] = {l, r};
    if (type == 3) { /* comb: resonator tuned 40 Hz..2 kHz, fractional delay */
        cut_smooth += (cut - cut_smooth) * 0.35f;
        float d = FS / (40.0f * fexp2(cut_smooth * 5.6f));
        if (d < 2) d = 2; if (d > COMB_LEN - 2) d = COMB_LEN - 2;
        unsigned di = (unsigned)d; float fr = d - (float)di, fb = res * 0.96f;
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
            unsigned w = (comb_pos + i) % COMB_LEN, r0 = (w + COMB_LEN - di) % COMB_LEN, r1 = (r0 + COMB_LEN - 1) % COMB_LEN;
            for (unsigned c = 0; c < 2; ++c) {
                float x = ch[c][i], delayed = comb[c][r0] + fr * (comb[c][r1] - comb[c][r0]);
                float y = x + fb * delayed;
                y = y / (1.0f + 0.25f * fabsf_(y)); /* tame runaway resonance */
                comb[c][w] = y;
                ch[c][i] = x + mix * (y * 0.6f - x);
            }
        }
        comb_pos = (comb_pos + FM1_BLOCK_FRAMES) % COMB_LEN;
        return;
    }
    if (type == 0 || type == 2) { run_ladder(ch, type == 0, cut, res, mix); return; }
    unsigned mode = 0; /* SEM: 12 dB state-variable low-pass; DJ: LP below centre, HP above */
    float fc_target;
    if (type == 4) {
        float d = ((float)ep[MFX_FILTER][1] - 64.0f) * (1.0f / 63);
        if (d > -0.02f && d < 0.02f) { cut_prev = -1.0f; return; }
        mode = d < 0 ? 0 : 2;
        fc_target = d < 0 ? 18000.0f * fexp2(d * 9.0f) : 20.0f * fexp2(d * 9.5f);
    } else fc_target = 30.0f * fexp2(cut * 9.4f);
    fc_target = clampf(fc_target, 20.0f, 18000.0f);
    if (cut_prev < 0) cut_prev = fc_target;
    float k = 2.0f - 1.94f * res;
    /* Coefficients glide across the block: no zipper noise when turning or modulating. */
    float g0 = ftan_pi(cut_prev / FS), g1 = ftan_pi(fc_target / FS);
    cut_prev = fc_target;
    for (unsigned c = 0; c < 2; ++c) {
        float ic1 = svf1[c], ic2 = svf2[c], jc1 = svf3[c], jc2 = svf4[c];
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
            float g = g0 + (g1 - g0) * ((float)i * (1.0f / FM1_BLOCK_FRAMES));
            float a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
            float x = ch[c][i], v3 = x - ic2;
            float v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
            v1 = v1 / (1.0f + 0.15f * fabsf_(v1)); /* gentle analog-style saturation */
            ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
            float y = mode == 0 ? v2 : mode == 1 ? v1 : x - k * v1 - v2;
            ch[c][i] = x + mix * (y - x);
        }
        svf1[c] = ic1; svf2[c] = ic2; svf3[c] = jc1; svf4[c] = jc2;
    }
}
static inline __attribute__((always_inline)) float shaper(unsigned type, float t)
{
    if (type == 0) return t / (1.0f + fabsf_(t));                      /* soft */
    if (type == 1) return clampf(t, -0.7f, 0.7f);                        /* hard */
    return fsin_cycles(t * 0.25f) * 0.75f;                                     /* fold */
}
static FM1_AUDIO_CODE void run_drive(float *l, float *r, float mix)
{
    unsigned type = ep[MFX_DRIVE][0];
    float amt = ep[MFX_DRIVE][1] * (1.0f / 127), gain = 1.0f + amt * amt * 40.0f;
    float fc = 500.0f * fexp2(ep[MFX_DRIVE][2] * (5.3f / 127)), a = clampf(2 * PI_F * fc / FS, 0.0f, 1.0f);
    float levels = fexp2(15.0f - amt * 12.0f);
    unsigned every = 1U + (unsigned)(amt * 15.0f);
    float makeup = 1.0f / (1.0f + amt * 3.0f);
    float *ch[2] = {l, r};
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        unsigned take = type != 3 || crush_count == 0;
        for (unsigned c = 0; c < 2; ++c) {
            float x = ch[c][i], y;
            if (type == 3) {
                if (take) hold[c] = (float)(int)(x * levels) / levels;
                y = hold[c];
            } else {
                /* 2x oversampling: shape the midpoint and the sample, then average. */
                float mid = 0.5f * (x + drv_prev[c]);
                y = 0.5f * (shaper(type, mid * gain) + shaper(type, x * gain)) * makeup;
                drv_prev[c] = x;
            }
            /* DC blocker, then tone low-pass. */
            float dcy = y - dc_x[c] + 0.995f * dc_y[c]; dc_x[c] = y; dc_y[c] = dcy;
            tone[c] += a * (dcy - tone[c]);
            ch[c][i] = x + mix * (tone[c] - x);
        }
        if (type == 3) crush_count = (crush_count + 1) % every;
    }
}
static inline __attribute__((always_inline)) float hermite(const float *buf, float pos)
{
    unsigned i = (unsigned)pos; float f = pos - (float)i;
    float xm1 = buf[(i + MOD_LEN - 1) % MOD_LEN], x0 = buf[i % MOD_LEN], x1 = buf[(i + 1) % MOD_LEN], x2 = buf[(i + 2) % MOD_LEN];
    float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}
static FM1_AUDIO_CODE void run_chorus(float *l, float *r, float mix)
{
    unsigned flange = ep[MFX_CHORUS][0] == 1;
    float rate = 0.05f * fexp2(ep[MFX_CHORUS][1] * (7.0f / 127)), depth = ep[MFX_CHORUS][2] * (1.0f / 127);
    float base = flange ? 44.0f : 600.0f, swing = (flange ? 176.0f : 320.0f) * depth, fb = flange ? 0.65f : 0.0f;
    float step = rate / FS;
    float *ch[2] = {l, r};
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        unsigned w = (mod_pos + i) % MOD_LEN;
        for (unsigned c = 0; c < 2; ++c) {
            /* Two voices per side for chorus (quadrature between sides); one for flanger. */
            float wet = 0;
            unsigned voices = flange ? 1U : 2U;
            for (unsigned v = 0; v < voices; ++v) {
                float lfo = 0.5f + 0.5f * fsin_cycles(lfo_chorus + c * 0.25f + v * 0.5f);
                float pos = (float)w - (base * (1.0f + 0.35f * v) + swing * lfo);
                if (!(pos > -(float)MOD_LEN)) pos = 0; /* bounded, NaN-safe */
                if (pos < 0) pos += MOD_LEN;
                wet += hermite(modbuf[c], pos);
            }
            wet *= flange ? 1.0f : 0.65f;
            float x = ch[c][i];
            modbuf[c][w] = x + fb * wet;
            ch[c][i] = x + mix * (wet - x);
        }
        lfo_chorus += step; if (lfo_chorus >= 1) lfo_chorus -= 1;
    }
    mod_pos = (mod_pos + FM1_BLOCK_FRAMES) % MOD_LEN;
}
static FM1_AUDIO_CODE void run_phaser(float *l, float *r, float mix)
{
    unsigned stages = 4U + 2U * (ep[MFX_PHASER][0] % 3U);
    float rate = 0.05f * fexp2(ep[MFX_PHASER][1] * (7.0f / 127)), fb = ep[MFX_PHASER][2] * (0.85f / 127);
    float *ch[2] = {l, r};
    enum { SUB = 16 };
    for (unsigned c = 0; c < 2; ++c) {
        for (unsigned s0 = 0; s0 < FM1_BLOCK_FRAMES; s0 += SUB) {
            /* Recompute the sweep every 16 samples; stages are spread for deeper notches. */
            float ph = lfo_phaser + rate * (float)s0 / FS + c * 0.25f;
            float lfo = 0.5f + 0.5f * fsin_cycles(ph);
            float coef[8];
            for (unsigned s = 0; s < stages; ++s) {
                float f = 160.0f * fexp2(lfo * 4.6f) * (1.0f + 0.22f * (float)s);
                float t = ftan_pi(clampf(f, 20.0f, 16000.0f) / FS);
                coef[s] = (1 - t) / (1 + t);
            }
            for (unsigned i = s0; i < s0 + SUB; ++i) {
                float x = ch[c][i], y = x + fb * ph_fb[c];
                for (unsigned s = 0; s < stages; ++s) {
                    float o = -coef[s] * y + ap[c][s];
                    ap[c][s] = y + coef[s] * o;
                    y = o;
                }
                ph_fb[c] = y;
                ch[c][i] = x + mix * (0.5f * (x + y) - x);
            }
        }
    }
    lfo_phaser += rate * FM1_BLOCK_FRAMES / FS; if (lfo_phaser >= 1) lfo_phaser -= 1;
}
static inline __attribute__((always_inline)) float blep(float t, float dt)
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}
/* Ring modulator, 8 carriers. FREQ spans 1 Hz (tremolo-like) .. 8 kHz
 * (metallic); TRACK follows the input pitch (FREQ = ratio x1/4..x4); SHIFT is
 * a frequency shifter (Hilbert pair, FREQ = shift); ENV lets the input level
 * sweep the carrier up (FREQ = base). SPREAD detunes the right channel. */
static inline __attribute__((always_inline)) float hb_stage(float *st, float a2, float x)
{
    /* y[n] = a^2 * (x[n] + y[n-2]) - x[n-2]; st = {x1, x2, y1, y2} */
    float y = a2 * (x + st[3]) - st[1];
    st[1] = st[0]; st[0] = x; st[3] = st[2]; st[2] = y;
    return y;
}
static FM1_AUDIO_CODE void run_ring(float *l, float *r, float mix)
{
    /* Niemitalo's 90-degree allpass pair, squared coefficients. */
    static const float ha[4] = {0.47940086f, 0.87621849f, 0.97659758f, 0.99749925f};
    static const float hb[4] = {0.16175849f, 0.73302270f, 0.94534499f, 0.99060201f};
    unsigned wave = ep[MFX_RING][0] % 8U;
    float v = ep[MFX_RING][1] * (1.0f / 127), spread = ep[MFX_RING][2] * (0.5f / 127);
    float f = fexp2(v * 13.0f); /* 1 Hz .. 8.2 kHz */
    float *ch[2] = {l, r};
    if (wave == 5) { /* TRACK: zero crossings of the low-passed input */
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
            trk_lp += 0.08f * (0.5f * (l[i] + r[i]) - trk_lp);
            if (trk_lp > 0.002f && trk_last <= 0.002f) ++trk_cross;
            trk_last = trk_lp;
        }
        trk_count += FM1_BLOCK_FRAMES;
        if (trk_count >= 2048U) {
            float est = (float)trk_cross * FS / (float)trk_count;
            if (est > 30.0f && est < 2000.0f) trk_freq += 0.5f * (est - trk_freq);
            trk_count = trk_cross = 0;
        }
        f = trk_freq * fexp2((v - 0.5f) * 4.0f);
    }
    for (unsigned c = 0; c < 2; ++c) {
        float fc = f * (1.0f + (c ? spread : 0.0f)), base = clampf(fc / FS, 0.0f, 0.45f), step = base, ph = ring_phase[c];
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
            float x = ch[c][i], car, out;
            if (wave == 7) { /* ENV: louder input -> higher carrier */
                float a = fabsf_(x);
                trk_env[c] += (a > trk_env[c] ? 0.02f : 0.0005f) * (a - trk_env[c]);
                step = clampf(base * (1.0f + 24.0f * trk_env[c]), 0.0f, 0.45f);
            }
            if (wave == 6) { /* SHIFT: I*cos - Q*sin = one sideband only */
                float iq = x, q = hb_x[c];
                for (unsigned k = 0; k < 4; ++k) { iq = hb_stage(hb_s[c][0][k], ha[k], iq); q = hb_stage(hb_s[c][1][k], hb[k], q); }
                hb_x[c] = x;
                out = iq * fsin_cycles(ph + 0.25f) - q * fsin_cycles(ph);
            } else {
                if (wave == 1) { car = ph < 0.5f ? 1.0f : -1.0f; car += blep(ph, step); float h = ph + 0.5f; if (h >= 1) h -= 1; car -= blep(h, step); }
                else if (wave == 2) car = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
                else if (wave == 3) car = 2.0f * ph - 1.0f - blep(ph, step);
                else if (wave == 4) { /* smooth noise: a new random target every cycle */
                    if (ph + step >= 1.0f) {
                        rn_seed = rn_seed * 1664525U + 1013904223U;
                        rn_a[c] = rn_b[c]; rn_b[c] = (float)(int32_t)rn_seed * (1.0f / 2147483648.0f);
                    }
                    car = rn_a[c] + (rn_b[c] - rn_a[c]) * ph;
                } else car = fsin_cycles(ph);
                out = x * car;
            }
            ch[c][i] = x + mix * (out - x);
            ph += step; if (ph >= 1) ph -= 1;
        }
        ring_phase[c] = ph;
    }
}
static inline __attribute__((always_inline)) float rd16(const int16_t *buf, unsigned pos) { return buf[pos] * (1.0f / 16384.0f); }
static inline __attribute__((always_inline)) int16_t wr16(float v) { return (int16_t)clampf(v * 16384.0f, -32767, 32767); }
static FM1_AUDIO_CODE void run_verb(float *l, float *r, float mix)
{
    static const float scale[3] = {0.55f, 1.0f, 0.42f}, damp_k[3] = {0.45f, 0.3f, 0.08f};
    unsigned type = ep[MFX_VERB][0] % 3U;
    float decay = ep[MFX_VERB][1] * (1.0f / 127.0f), fb = 0.7f + 0.27f * decay, damp = damp_k[type];
    float ap_g = type == 2 ? 0.65f : 0.5f;
    unsigned pre = (unsigned)ep[MFX_VERB][2] * (RV_PRE_MAX - 1U) / 127U;
    unsigned clen[2][RV_COMBS], alen[2][RV_APS];
    int16_t *cline[2][RV_COMBS], *aline[2][RV_APS];
    unsigned off = 0;
    for (unsigned k = 0; k < RV_COMBS; ++k) {
        for (unsigned c = 0; c < 2; ++c) {
            clen[c][k] = (unsigned)((float)rv_comb_len[k] * scale[type]) + (c ? RV_SPREAD : 0U);
            cline[c][k] = rv_comb[c] + off;
            if (rv_cpos[c][k] >= clen[c][k]) rv_cpos[c][k] = 0;
        }
        off += rv_comb_len[k] + RV_SPREAD;
    }
    off = 0;
    for (unsigned k = 0; k < RV_APS; ++k) {
        for (unsigned c = 0; c < 2; ++c) {
            alen[c][k] = rv_ap_len[k] + (c ? RV_SPREAD : 0U);
            aline[c][k] = rv_ap[c] + off;
            if (rv_apos[c][k] >= alen[c][k]) rv_apos[c][k] = 0;
        }
        off += rv_ap_len[k] + RV_SPREAD;
    }
    if (rv_ppos >= RV_PRE_MAX) rv_ppos = 0;
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; i += 2) {
        float in = (l[i] + r[i] + l[i + 1] + r[i + 1]) * 0.25f;
        rv_pre[rv_ppos] = wr16(in);
        unsigned tap = rv_ppos >= pre ? rv_ppos - pre : rv_ppos + RV_PRE_MAX - pre;
        if (++rv_ppos >= RV_PRE_MAX) rv_ppos = 0;
        float x = rd16(rv_pre, tap) * 0.25f;
        for (unsigned c = 0; c < 2; ++c) {
            float out = 0;
            for (unsigned k = 0; k < RV_COMBS; ++k) {
                unsigned p = rv_cpos[c][k];
                int16_t *line = cline[c][k];
                float y = rd16(line, p);
                rv_damp[c][k] = y + (rv_damp[c][k] - y) * damp;
                line[p] = wr16(x + rv_damp[c][k] * fb);
                if (++p >= clen[c][k]) p = 0;
                rv_cpos[c][k] = (uint16_t)p;
                out += y;
            }
            out *= 0.18f;
            for (unsigned k = 0; k < RV_APS; ++k) {
                unsigned p = rv_apos[c][k];
                int16_t *line = aline[c][k];
                float b = rd16(line, p), y = b - out * ap_g;
                line[p] = wr16(out + b * ap_g);
                if (++p >= alen[c][k]) p = 0;
                rv_apos[c][k] = (uint16_t)p;
                out = y;
            }
            float *o = c ? r : l;
            o[i] += mix * 0.5f * (out + rv_last[c]); /* linear upsampling */
            o[i + 1] += mix * out;
            rv_last[c] = out;
        }
    }
}
static FM1_AUDIO_CODE void run_lowcut(float *l, float *r)
{
    /* 2-pole high-pass, 100 Hz, Q 0.7 (state-variable). */
    const float g = 0.0071f, k = 1.414f, a1 = 1.0f / (1.0f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    float *ch[2] = {l, r};
    for (unsigned c = 0; c < 2; ++c)
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
            float x = ch[c][i], v3 = x - lc2[c], v1 = a1 * lc1[c] + a2 * v3, v2 = lc2[c] + a2 * lc1[c] + a3 * v3;
            lc1[c] = 2 * v1 - lc1[c]; lc2[c] = 2 * v2 - lc2[c];
            ch[c][i] = x - k * v1 - v2;
        }
}
/* RBJ biquads: low shelf 120 Hz, peak (Q 0.9) at the chosen mid, high shelf 8 kHz.
 * Coefficients are recomputed only when a control changes. */
static FM1_AUDIO_CODE void eq_design(float *c, unsigned kind, float f, float db)
{
    float A = fexp2(db * 0.0830482f); /* 10^(db/40) */
    float w = f / FS, sn = fsin_cycles(w), cs = fsin_cycles(w + 0.25f);
    float b0, b1, b2, a0, a1, a2;
    if (kind == 1) {
        float al = sn / (2.0f * 0.9f);
        b0 = 1 + al * A; b1 = -2 * cs; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cs; a2 = 1 - al / A;
    } else {
        float al = sn * 0.7071068f, sq = 2.0f * al * fsqrt_(A), s = kind ? -1.0f : 1.0f; /* S = 1 */
        b0 = A * ((A + 1) - s * (A - 1) * cs + sq);
        b1 = 2 * s * A * ((A - 1) - s * (A + 1) * cs);
        b2 = A * ((A + 1) - s * (A - 1) * cs - sq);
        a0 = (A + 1) + s * (A - 1) * cs + sq;
        a1 = -2 * s * ((A - 1) + s * (A + 1) * cs);
        a2 = (A + 1) + s * (A - 1) * cs - sq;
    }
    float n = 1.0f / a0;
    c[0] = b0 * n; c[1] = b1 * n; c[2] = b2 * n; c[3] = a1 * n; c[4] = a2 * n;
}
static FM1_AUDIO_CODE void run_eq(float *l, float *r)
{
    static const float mids[5] = {250, 500, 1000, 2000, 4000};
    if (ep[MFX_EQ][0] != eq_key[0] || ep[MFX_EQ][1] != eq_key[1] || ep[MFX_EQ][2] != eq_key[2] || ep[MFX_EQ][3] != eq_key[3]) {
        for (unsigned i = 0; i < 4; ++i) eq_key[i] = ep[MFX_EQ][i];
        eq_design(eq_c[0], 0, 120.0f, ((int)ep[MFX_EQ][1] - 64) * (12.0f / 63));
        eq_design(eq_c[1], 1, mids[ep[MFX_EQ][0] % 5], ((int)ep[MFX_EQ][2] - 64) * (12.0f / 63));
        eq_design(eq_c[2], 2, 8000.0f, ((int)ep[MFX_EQ][3] - 64) * (12.0f / 63));
    }
    float *ch[2] = {l, r};
    for (unsigned b = 0; b < 3; ++b) {
        if (ep[MFX_EQ][1 + b] == 64) { eq_z[b][0][0] = eq_z[b][0][1] = eq_z[b][1][0] = eq_z[b][1][1] = 0; continue; }
        const float *c = eq_c[b];
        for (unsigned k = 0; k < 2; ++k) {
            float z1 = eq_z[b][k][0], z2 = eq_z[b][k][1], *x = ch[k];
            for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) { /* transposed direct form II */
                float in = x[i], y = c[0] * in + z1;
                z1 = c[1] * in - c[3] * y + z2; z2 = c[2] * in - c[4] * y;
                x[i] = y;
            }
            eq_z[b][k][0] = z1; eq_z[b][k][1] = z2;
        }
    }
}
/* Output boost and peak limiter: the block peak sets the gain (instant
 * attack, ~150 ms release), ramped across the block; a soft knee catches
 * anything left above full scale. */
static FM1_AUDIO_CODE void run_output(float *l, float *r)
{
    static const float boost[5] = {1.0f, 1.4125375f, 1.9952623f, 2.8183829f, 3.9810717f};
    float g = boost[out_step % 5], peak = 0;
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        float a = fabsf_(l[i]), b = fabsf_(r[i]);
        if (a > peak) peak = a; if (b > peak) peak = b;
    }
    float want = peak * g > 0.89f ? 0.89f / (peak * g) : 1.0f;
    float start = lim_gain, end = want < lim_gain ? want : lim_gain + (want - lim_gain) * 0.02f;
    lim_gain = end;
    float step = (end - start) * (1.0f / FM1_BLOCK_FRAMES), k = start;
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i, k += step) {
        float m = g * (want < start ? end : k);
        float x = l[i] * m, y = r[i] * m;
        l[i] = x > 0.9f ? 0.9f + (x - 0.9f) / (1.0f + (x - 0.9f) * 10.0f) : x < -0.9f ? -0.9f + (x + 0.9f) / (1.0f - (x + 0.9f) * 10.0f) : x;
        r[i] = y > 0.9f ? 0.9f + (y - 0.9f) / (1.0f + (y - 0.9f) * 10.0f) : y < -0.9f ? -0.9f + (y + 0.9f) / (1.0f - (y + 0.9f) * 10.0f) : y;
    }
}
/* Same boost and limiter in fixed point (Q12 gain), used when no float
 * effect is running so the plain synth path stays integer. */
static int32_t lim_q12 = 4096;
static FM1_AUDIO_CODE void run_output_int(int32_t *buffer)
{
    static const int32_t boost[5] = {4096, 5786, 8173, 11544, 16306};
    const int32_t limit = 7465860; /* 0.89 of full scale */
    int32_t g = boost[out_step % 5], peak = 1;
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
        int32_t a = buffer[i] < 0 ? -buffer[i] : buffer[i];
        if (a > peak) peak = a;
    }
    /* No 64-bit division in the interrupt: step the gain down until it fits. */
    int32_t want = g;
    for (unsigned n = 0; n < 64 && want > 256 && (int64_t)peak * want > ((int64_t)limit << 12); ++n) want -= want >> 4;
    int32_t start = lim_q12, end = want < start ? want : start + (want - start) / 50 + (want > start);
    if (want < start) start = end; /* instant attack */
    lim_q12 = end;
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        int32_t k = start + (int32_t)(((int64_t)(end - start) * (int32_t)i) >> 7);
        for (unsigned c = 0; c < 2; ++c) {
            int64_t v = ((int64_t)buffer[2 * i + c] * k) >> 12;
            buffer[2 * i + c] = v > 8388607 ? 8388607 : v < -8388607 ? -8388607 : (int32_t)v;
        }
    }
}
/* SCOPE page: mono copy of the output, every second frame (22 kHz), written
 * only while the page is open. The band analysis runs in the UI task. */
enum { SCOPE_LEN = 512 };
static int16_t scope_buf[SCOPE_LEN];
static volatile unsigned scope_pos, scope_on;
void mfx_scope_enable(unsigned on) { scope_on = on; }
void mfx_scope_read(int16_t *out, unsigned n)
{
    if (n > SCOPE_LEN) n = SCOPE_LEN;
    unsigned p = scope_pos + SCOPE_LEN - n;
    for (unsigned i = 0; i < n; ++i) out[i] = scope_buf[(p + i) % SCOPE_LEN];
}
static FM1_AUDIO_CODE void scope_capture(const int32_t *b)
{
    unsigned p = scope_pos;
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; i += 2, p = (p + 1U) % SCOPE_LEN)
        scope_buf[p] = (int16_t)((b[2 * i] + b[2 * i + 1]) >> 9);
    scope_pos = p;
}
static FM1_AUDIO_CODE void mfx_run(int32_t *buffer);
FM1_AUDIO_CODE void mfx_process(int32_t *buffer)
{
    if (!buffer) return;
    mfx_run(buffer);
    if (scope_on) scope_capture(buffer);
}
static FM1_AUDIO_CODE void mfx_run(int32_t *buffer)
{
    unsigned any = 0;
    for (unsigned fx = 0; fx < MFX_COUNT; ++fx)
        for (unsigned p = 0; p < 4; ++p) {
            int v = (int)prm[fx][p] + (p ? (int)mods[fx][p] : 0);
            ep[fx][p] = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
        }
    for (unsigned fx = 0; fx < MFX_EQ; ++fx) any |= ep[fx][3];
    unsigned eq = ep[MFX_EQ][1] != 64 || ep[MFX_EQ][2] != 64 || ep[MFX_EQ][3] != 64;
    any |= lowcut_on | eq;
    if (!any) { /* no effect running: integer boost and limiter only */
        cut_smooth = -1.0f; lim_gain = 1.0f;
        if (out_step) run_output_int(buffer); else lim_q12 = 4096;
        return;
    }
    /* Block buffers live in BSS: the audio interrupt stack is small. */
    static float l[FM1_BLOCK_FRAMES], r[FM1_BLOCK_FRAMES];
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        l[i] = (float)buffer[2 * i] * (1.0f / 8388608.0f);
        r[i] = (float)buffer[2 * i + 1] * (1.0f / 8388608.0f);
    }
    if (ep[MFX_DRIVE][3]) run_drive(l, r, ep[MFX_DRIVE][3] * (1.0f / 127));
    if (ep[MFX_FILTER][3]) run_filter(l, r, ep[MFX_FILTER][3] * (1.0f / 127)); else cut_smooth = cut_prev = -1.0f;
    if (ep[MFX_CHORUS][3]) run_chorus(l, r, ep[MFX_CHORUS][3] * (1.0f / 127));
    if (ep[MFX_PHASER][3]) run_phaser(l, r, ep[MFX_PHASER][3] * (1.0f / 127));
    if (ep[MFX_RING][3]) run_ring(l, r, ep[MFX_RING][3] * (1.0f / 127));
    if (ep[MFX_VERB][3]) run_verb(l, r, ep[MFX_VERB][3] * (1.0f / 127));
    if (eq) run_eq(l, r);
    if (lowcut_on) run_lowcut(l, r);
    if (out_step) run_output(l, r); else lim_gain = 1.0f;
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        buffer[2 * i] = (int32_t)(clampf(l[i], -1.0f, 0.9999999f) * 8388607.0f);
        buffer[2 * i + 1] = (int32_t)(clampf(r[i], -1.0f, 0.9999999f) * 8388607.0f);
    }
}

/* Pattern extras: 28 control bytes, output step, low cut. 0xff = default. */
void mfx_state_get(uint8_t out[32])
{
    memset(out, 0xff, 32);
    memcpy(out, (const void *)prm, sizeof prm);
    out[28] = out_step; out[29] = lowcut_on;
}
void mfx_state_put(const uint8_t in[32])
{
    for (unsigned f = 0; f < MFX_COUNT; ++f) for (unsigned p = 0; p < 4; ++p) {
        unsigned v = in[f * 4 + p];
        if (v == 0xff) v = defaults[f][p];
        if (!p && v >= type_count[f]) v = defaults[f][0];
        if (v > 127) v = 127;
        prm[f][p] = (uint8_t)v;
    }
    out_step = in[28] <= 4 ? in[28] : 2;
    lowcut_on = in[29] == 1;
}
