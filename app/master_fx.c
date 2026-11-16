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
static float tone[2], hold[2];
static float modbuf[2][MOD_LEN], ap[2][8], ph_fb[2];
static unsigned mod_pos, crush_count;
static float lfo_chorus, lfo_phaser, ring_phase[2];
/* FILTER: two instances. A TYPE change while the filter runs plays the old type and the new one side by
 * side and crossfades between them (old and new may share a family: ACID / LADDER the ladder, SEM / DJ the
 * SVF). Memory in BSS (small state first, cleared at once; then the comb line); the glides start at -1
 * (= take the current value) and live in a small initialised table so the big part stays out of .data. */
typedef struct {
    float svf1[2], svf2[2], svf3[2], svf4[2]; /* SEM / DJ: state-variable pair */
    float ld[2][4], ld_hp[2];                 /* ACID / LADDER: four saturating one-pole stages per channel */
    float comb[2][COMB_LEN];                  /* COMB */
} flt_state;
typedef struct { float cut_smooth, cut_prev, k_prev; unsigned comb_pos; } flt_glide;
enum { FLT_SMALL = (int)__builtin_offsetof(flt_state, comb) };
static flt_state flt[2];
static flt_glide flt_g[2] = {{-1.0f, -1.0f, -1.0f, 0}, {-1.0f, -1.0f, -1.0f, 0}};
static uint8_t flt_cur; /* the instance that plays fx_type[MFX_FILTER] */
/* TYPE crossfade: xf_stage 0 = none, 1 = the incoming instance's comb line is being cleared (sliced; the old
 * type plays on alone), 2 = the incoming instance runs: XF_PRIME blocks unheard (its start transient dies
 * away), then XF_BLOCKS blocks of a per-sample linear crossfade. xf_pos counts the blocks run in stage 2. */
enum { XF_PRIME = 8, XF_BLOCKS = 8 };
static uint8_t xf_stage, xf_type, xf_pos, xf_ran;
static uint8_t fx_type[MFX_COUNT] = {255, 255, 255, 255, 255, 255, 255}; /* type each effect plays (see fx_smooth) */
static uint16_t xf_off; /* bytes of the incoming comb line already zeroed */
/* Ring: noise carrier, pitch tracking, envelope, Hilbert pair for SHIFT. */
static float rn_a[2], rn_b[2], trk_freq = 220.0f, trk_last, trk_lp, trk_env[2], hb_s[2][2][4][4], hb_x[2];
static uint32_t rn_seed = 0x1234567U;
static unsigned trk_count, trk_cross;
/* Reverb (design A of docs/coord-notes/research-reverb-1011.md, user's choice 10.11): a feedback delay
 * network at half rate (22.05 kHz). Mono feed (3-tap decimator), low cut + band limit, pre-delay, 2 allpass
 * diffusers, 8 delay lines mixed by a Householder matrix (x - sum / 4), a one-pole damping filter per line
 * (DC gain from RT60, top from the type's HF ratio, Jot), line 0 read with a slow triangle modulation,
 * stereo from two orthogonal sign patterns, upsampled linearly. TYPE presets: ROOM, HALL, and PLATE = the
 * research's AMBIENT preset (user: keep the name PLATE). 16-bit lines hold LSB units (no scale multiply per
 * access) and are written by truncation toward zero: tails end at exact 0, no limit cycle. The float states
 * pass (x + RV_DN) - RV_DN, which turns anything under ~1e-25 into an exact 0 (no denormals, exact silence).
 * No division and no library call in the sample loop; coefficients are recomputed (9 x fexp2, PLATE also
 * fsqrt_: our RAM code) only in the block where TYPE or DECAY changed. */
enum { RV_PRE_MAX = 2205, RV_MOD_CAP = 32, RV_LINE_TOTAL = 449 + 509 + 577 + 641 + 701 + 769 + 839 + 907 + RV_MOD_CAP,
       RV_DIF_TOTAL = 281 + 205 };
typedef struct {
    uint16_t len[8], dif[2]; /* line lengths (line 0: + RV_MOD_CAP of room for its modulation), diffusers */
    uint16_t gin_sqrt; /* input gain law: 0 = gin_k (1 - g^2), 1 = gin_k sqrt(1 - g^2) */
    float khp, klp, gd, dph, md, wid; /* low cut, band limit (one-pole), diffuser gain, LFO step, depth (samples), width */
    float inv_tmin, log2_span, damp_k, gin_k, gout; /* RT60 = tmin * 2^(DECAY/127 * log2_span); Jot: b = damp_k * len / fs / RT60 */
} rv_type_t;
typedef struct { uint16_t p[8], dp[2]; float f[8], hp, lp, ph, prevx, last[2]; } rv_state_t;
static int16_t rv_line[RV_LINE_TOTAL], rv_dif[RV_DIF_TOTAL], rv_pre[RV_PRE_MAX];
static rv_state_t rv;
static uint16_t rv_ppos;
static uint16_t rv_key; /* type * 128 + DECAY + 1 of the coefficients below; 0 = recompute */
static float rv_a[8], rv_b[8], rv_gin, rv_gout;
static uint8_t ep[MFX_COUNT][4];
static float dc_x[2], dc_y[2], drv_prev[2];
static void ensure_ready(void);
static volatile uint8_t lowcut_on, out_step = 0; /* 0 dB by default */
/* EQ biquads (3 bands x 2 channels) and the output limiter. */
static float eq_c[3][5], eq_z[3][2][2], lim_gain = 1.0f;
/* Integer limiter gain, Q12, boost included (at rest = boost_q12[out_step]). The float limiter keeps
 * lim_gain (boost excluded) and starts from the integer gain; the integer path starts from rest. */
static const int32_t boost_q12[5] FM1_AUDIO_DATA = {4096, 5786, 8173, 11544, 16306};
static int32_t lim_q12 = 4096;
static uint8_t eq_key[4] = {255, 255, 255, 255};
static float lc1[2], lc2[2];
static float sub_hp[2][2]; /* fixed 20 Hz high-pass at the end of the float chain: 2 one-pole stages per channel */
/* How much of that filter is heard: 1 = all (float path), 0 = at rest (integer path, state zero). Leaving the
 * float path fades the filter's correction out over MFX_SUB_FADE blocks while it keeps running (no step at that
 * block edge); coming back during the fade goes on from the same state and fades it in again. */
static float sub_g; /* fade length: MFX_SUB_FADE (master_fx.h) */
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
    memset(flt, 0, sizeof flt); xf_stage = 0;
    for (unsigned n = 0; n < 2; ++n) flt_g[n].cut_smooth = flt_g[n].cut_prev = -1.0f, flt_g[n].comb_pos = 0; /* k_prev kept, as before */
    memset(tone, 0, sizeof tone); memset(hold, 0, sizeof hold); memset(modbuf, 0, sizeof modbuf);
    memset(ap, 0, sizeof ap); memset(ph_fb, 0, sizeof ph_fb);
    mod_pos = crush_count = 0;
    lfo_chorus = lfo_phaser = ring_phase[0] = ring_phase[1] = 0;
    memset(hb_s, 0, sizeof hb_s); memset(hb_x, 0, sizeof hb_x); memset(trk_env, 0, sizeof trk_env);
    memset(rv_line, 0, sizeof rv_line); memset(rv_dif, 0, sizeof rv_dif); memset(rv_pre, 0, sizeof rv_pre);
    memset(&rv, 0, sizeof rv); rv_key = 0;
    rv_ppos = 0; memset(lc1, 0, sizeof lc1); memset(lc2, 0, sizeof lc2);
    memset(dc_x, 0, sizeof dc_x); memset(dc_y, 0, sizeof dc_y);
    memset(drv_prev, 0, sizeof drv_prev);
    memset(eq_z, 0, sizeof eq_z); memset(eq_key, 255, sizeof eq_key); lim_gain = 1.0f; lim_q12 = boost_q12[out_step % 5];
    memset(sub_hp, 0, sizeof sub_hp); sub_g = 0.0f;
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
static FM1_AUDIO_CODE void run_ladder(flt_state *s, flt_glide *gl, float **ch, unsigned acid, float cut, float res, float mix)
{
    /* 1x rate (half the CPU of the first version); the top cutoff is kept
     * where the four stages stay stable at full resonance. */
    float fc = acid ? 40.0f * fexp2(cut * 7.6f) : 30.0f * fexp2(cut * 8.7f);
    fc = clampf(fc, 20.0f, acid ? 9000.0f : 12000.0f);
    if (gl->cut_prev < 0) gl->cut_prev = fc;
    float w0 = 2.0f * PI_F * gl->cut_prev / FS, w1 = 2.0f * PI_F * fc / FS;
    float g0 = w0 / (1.0f + w0), g1 = w1 / (1.0f + w1);
    gl->cut_prev = fc;
    /* ACID: softer top than before (scream just before full), LADDER: self-oscillates at the top. */
    float k = acid ? 3.9f * res : 4.1f * res, drive = acid ? 1.9f : 1.15f;
    float k0 = gl->k_prev < 0 ? k : gl->k_prev, dk = (k - k0) * (1.0f / FM1_BLOCK_FRAMES); /* RESO glides over the block */
    gl->k_prev = k;
    float comp = acid ? 1.0f + 0.3f * k : 1.0f + 0.4f * k; /* LADDER louder than this clipped at full RESO */
    for (unsigned c = 0; c < 2; ++c) {
        float y0 = s->ld[c][0], y1 = s->ld[c][1], y2 = s->ld[c][2], y3 = s->ld[c][3], hp = s->ld_hp[c];
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
            float g = g0 + (g1 - g0) * ((float)i * (1.0f / FM1_BLOCK_FRAMES)), x = ch[c][i];
            float fb = y3;
            if (acid) { hp += 0.024f * (fb - hp); fb -= hp; } /* ~170 Hz high-pass in the loop */
            float u = ftanh(x * drive - (k0 + dk * (float)i) * fb) + 1e-20f;
            float t0 = ftanh(y0), t1 = ftanh(y1), t2 = ftanh(y2), t3 = ftanh(y3);
            y0 += g * (u - t0); y1 += g * (t0 - t1); y2 += g * (t1 - t2); y3 += g * (t2 - t3);
            float y = y3 * comp / drive;
            ch[c][i] = x + mix * (y - x);
        }
        s->ld[c][0] = y0; s->ld[c][1] = y1; s->ld[c][2] = y2; s->ld[c][3] = y3; s->ld_hp[c] = hp;
    }
}
/* One filter instance n playing `type` over the block (CUTOFF / RESO from ep[]). */
static FM1_AUDIO_CODE void run_filter(unsigned n, unsigned type, float *l, float *r, float mix)
{
    flt_state *s = &flt[n];
    flt_glide *gl = &flt_g[n];
    float cut = ep[MFX_FILTER][1] * (1.0f / 127), res = ep[MFX_FILTER][2] * (1.0f / 127);
    if (gl->cut_smooth < 0) gl->cut_smooth = cut;
    float *ch[2] = {l, r};
    if (type == 3) { /* comb: resonator tuned 40 Hz..2 kHz, fractional delay */
        gl->cut_smooth += (cut - gl->cut_smooth) * 0.35f;
        float d = FS / (40.0f * fexp2(gl->cut_smooth * 5.6f));
        if (d < 2) d = 2; if (d > COMB_LEN - 2) d = COMB_LEN - 2;
        unsigned di = (unsigned)d; float fr = d - (float)di, fb = res * 0.96f;
        unsigned comb_pos = gl->comb_pos;
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
            unsigned w = (comb_pos + i) % COMB_LEN, r0 = (w + COMB_LEN - di) % COMB_LEN, r1 = (r0 + COMB_LEN - 1) % COMB_LEN;
            for (unsigned c = 0; c < 2; ++c) {
                float *comb = s->comb[c];
                float x = ch[c][i], delayed = comb[r0] + fr * (comb[r1] - comb[r0]);
                float y = x + fb * delayed;
                y = y / (1.0f + 0.25f * fabsf_(y)); /* tame runaway resonance */
                comb[w] = y;
                ch[c][i] = x + mix * (y * 0.6f - x);
            }
        }
        gl->comb_pos = (comb_pos + FM1_BLOCK_FRAMES) % COMB_LEN;
        return;
    }
    if (type == 0 || type == 2) { run_ladder(s, gl, ch, type == 0, cut, res, mix); return; }
    unsigned mode = 0; /* SEM: 12 dB state-variable low-pass; DJ: LP below centre, HP above */
    float fc_target;
    if (type == 4) {
        float d = ((float)ep[MFX_FILTER][1] - 64.0f) * (1.0f / 63);
        if (d > -0.02f && d < 0.02f) { gl->cut_prev = -1.0f; return; }
        mode = d < 0 ? 0 : 2;
        fc_target = d < 0 ? 18000.0f * fexp2(d * 9.0f) : 20.0f * fexp2(d * 9.5f);
    } else fc_target = 30.0f * fexp2(cut * 9.4f);
    fc_target = clampf(fc_target, 20.0f, 18000.0f);
    if (gl->cut_prev < 0) gl->cut_prev = fc_target;
    float k = 2.0f - 1.94f * res;
    /* Coefficients glide across the block: no zipper noise when turning or modulating. */
    float g0 = ftan_pi(gl->cut_prev / FS), g1 = ftan_pi(fc_target / FS);
    gl->cut_prev = fc_target;
    for (unsigned c = 0; c < 2; ++c) {
        float ic1 = s->svf1[c], ic2 = s->svf2[c], jc1 = s->svf3[c], jc2 = s->svf4[c];
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
        s->svf1[c] = ic1; s->svf2[c] = ic2; s->svf3[c] = jc1; s->svf4[c] = jc2;
    }
}
/* FILTER over the block: the current instance alone, or during a TYPE change both instances side by side
 * (2 x the cost: mfx_clearing reports the block), the incoming one first unheard, then crossfaded in per
 * sample along one linear ramp over XF_BLOCKS blocks; the last sample of the ramp is the new type alone. */
static float xf_buf[2][FM1_BLOCK_FRAMES]; /* BSS: the interrupt stack is small */
static FM1_AUDIO_CODE void run_filter_xf(float *l, float *r, float mix)
{
    if (xf_stage != 2) { run_filter(flt_cur, ep[MFX_FILTER][0], l, r, mix); return; }
    volatile float *bl = xf_buf[0], *br = xf_buf[1]; /* volatile: the copy must not become a memcpy call */
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) { bl[i] = l[i]; br[i] = r[i]; }
    run_filter(flt_cur, ep[MFX_FILTER][0], l, r, mix);
    run_filter(flt_cur ^ 1U, xf_type, xf_buf[0], xf_buf[1], mix);
    xf_ran = 1;
    if (xf_pos >= XF_PRIME) {
        unsigned base = (unsigned)(xf_pos - XF_PRIME) * FM1_BLOCK_FRAMES + 1U;
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
            float c = (float)(base + i) * (1.0f / (XF_BLOCKS * FM1_BLOCK_FRAMES));
            l[i] += c * (xf_buf[0][i] - l[i]);
            r[i] += c * (xf_buf[1][i] - r[i]);
        }
    }
    if (++xf_pos >= XF_PRIME + XF_BLOCKS) { /* done: the incoming instance plays on alone */
        flt_cur ^= 1U; fx_type[MFX_FILTER] = xf_type; xf_stage = 0;
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
    static float amt_s = -1.0f;
    float want = ep[MFX_DRIVE][1] * (1.0f / 127);
    amt_s = amt_s < 0 ? want : amt_s + (want - amt_s) * 0.3f; /* ~10 ms glide */
    float amt = amt_s, gain = 1.0f + amt * amt * 40.0f;
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
    float base = flange ? 44.0f : 600.0f, target = (flange ? 176.0f : 320.0f) * depth, fb = flange ? 0.65f : 0.0f;
    static float swing = -1.0f;
    if (swing < 0) swing = target;
    float step = rate / FS;
    float *ch[2] = {l, r};
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        unsigned w = (mod_pos + i) % MOD_LEN;
        swing += (target - swing) * 0.003f; /* DEPTH glides (~8 ms): no jump of the delay = no click */
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
            ch[c][i] = x + 0.5f * mix * (wet - x); /* MIX 100 % = 50/50: the chorus, not a vibrato */
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
    static const float ha[4] FM1_AUDIO_DATA = {0.47940086f, 0.87621849f, 0.97659758f, 0.99749925f};
    static const float hb[4] FM1_AUDIO_DATA = {0.16175849f, 0.73302270f, 0.94534499f, 0.99060201f};
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
#ifdef FM1_HOST_TEST
unsigned mfx_wr16_clamps; /* host test only: how often a reverb line sample hit the clamp */
#endif
static inline __attribute__((always_inline)) int16_t wr16(float v)
{
    float s = v * 16384.0f;
#ifdef FM1_HOST_TEST
    if (s > 32767.0f || s < -32767.0f) ++mfx_wr16_clamps;
#endif
    return (int16_t)clampf(s, -32767, 32767);
}
/* A reverb line sample already in LSB units: truncate toward zero, clamp. Integer clamp (min / max, no float
 * compares): s is always far inside int32 here (every source is a 16-bit line or the 16-bit pre-delay times
 * gains under 8, so |s| < 2^20). Same result as (int16_t)clampf(s, -32767, 32767). */
static inline __attribute__((always_inline)) int16_t wl16(float s)
{
#ifdef FM1_HOST_TEST
    if (s > 32767.0f || s < -32767.0f) ++mfx_wr16_clamps;
#endif
    int32_t q = (int32_t)s;
    q = q > 32767 ? 32767 : q;
    return (int16_t)(q < -32767 ? -32767 : q);
}
/* Line level (gin_k) and wet level (gout) per type. gin_k: no clamp on any full-scale sine 25 Hz..4 kHz at
 * any DECAY, 15 % margin (host sweep in 0.4 % steps, 11.11; tests/t_reverb.c case 20). gout: the tail of the
 * S8 burst (tests/t_master_fx.c, DECAY 70, PREDELAY 20) as loud as the old reverb's ROOM / HALL; PLATE
 * (= AMBIENT) as loud as the old HALL (the old PLATE was a short room: matching it would make this long tail
 * 26 dB quieter). */
#ifndef RV_GIN_ROOM
#define RV_GIN_ROOM 8000.0f
#define RV_GIN_HALL 7800.0f
#define RV_GIN_PLATE 2850.0f
#endif
#define RV_GOUT_ROOM 0.067581f
#define RV_GOUT_HALL 0.042073f
#define RV_GOUT_PLATE 0.029640f
#define RV_DN 1e-18f /* (x + RV_DN) - RV_DN: values under ~1e-25 become exact 0 (built with -fno-fast-math) */
/* TYPE presets: ROOM (drums, tight), HALL (pads, strings), PLATE = AMBIENT (long, dark, slow movement). */
static const rv_type_t rv_types[3] FM1_AUDIO_DATA = {
    {{171, 193, 219, 243, 267, 293, 319, 345}, {162, 118}, 0, 0.0445686f, 0.9230474f, 0.60f, 4.081632653e-05f, 1.5f, 0.85f,
     4.0f, 2.6780719f, 3.9819498f, RV_GIN_ROOM, RV_GOUT_ROOM},
    {{449, 509, 577, 641, 701, 769, 839, 907}, {281, 205}, 0, 0.0308585f, 0.8639404f, 0.52f, 2.494331066e-05f, 9.0f, 1.0f,
     1.1111111f, 2.7369656f, 9.0664288f, RV_GIN_HALL, RV_GOUT_HALL},
    {{449, 509, 577, 641, 701, 769, 839, 907}, {281, 205}, 1, 0.0253196f, 0.7913787f, 0.60f, 1.587301587e-05f, 14.0f, 1.0f,
     0.4f, 2.4854268f, 17.4612703f, RV_GIN_PLATE, RV_GOUT_PLATE}};
/* Coefficients for TYPE / DECAY: RT60 = tmin * 2^(DECAY / 127 * log2_span); per line g = 10^(-3 len / (fs RT60)),
 * Jot damping pole b = damp_k * len / (fs RT60), a = g (1 - b). The input gain is gin_k (1 - g^2) (PLATE:
 * gin_k sqrt(1 - g^2), its measured limit falls slower) with g of the longest line, so the worst full-scale
 * sine (measured over 25 Hz..4 kHz at every DECAY) stays under the clamp; the output gain undoes it (the wet
 * level does not depend on it). Runs only when TYPE or DECAY changed (fsqrt_ divides: not per sample). */
static FM1_AUDIO_CODE void run_verb_coef(unsigned type, unsigned decay)
{
    const rv_type_t *t = &rv_types[type];
    float inv_t = t->inv_tmin * fexp2(-(float)decay * (1.0f / 127.0f) * t->log2_span) * (1.0f / 22050.0f), g = 0;
    for (unsigned k = 0; k < 8; ++k) {
        float e = (float)t->len[k] * inv_t;
        g = fexp2(-9.9657843f * e);
        float b = t->damp_k * e;
        rv_a[k] = g * (1.0f - b); rv_b[k] = b;
    }
    float nrm2 = 1.0f - g * g; /* g of line 7, the longest */
    float gi = t->gin_k * (t->gin_sqrt ? fsqrt_(nrm2) : nrm2); /* LSB per full scale */
    rv_gin = gi * (1.0f / 16384.0f); /* the pre-delay line holds 16384 per full scale */
    rv_gout = t->gout / gi;
}
static FM1_AUDIO_CODE void run_verb(float *l, float *r, float mix)
{
    unsigned type = ep[MFX_VERB][0] % 3U;
    const rv_type_t *t = &rv_types[type];
    unsigned key = type * 128U + ep[MFX_VERB][1] + 1U;
    if (key != rv_key) { run_verb_coef(type, ep[MFX_VERB][1]); rv_key = (uint16_t)key; }
    unsigned pre = (unsigned)ep[MFX_VERB][2] * (RV_PRE_MAX - 1U) / 127U;
    int16_t *ln[8], *dl[2];
    unsigned cap[8], dcap[2], off = 0;
    for (unsigned k = 0; k < 8; ++k) {
        cap[k] = t->len[k] + (k ? 0U : (unsigned)RV_MOD_CAP);
        ln[k] = rv_line + off; off += cap[k];
        if (rv.p[k] >= cap[k]) rv.p[k] = 0;
    }
    off = 0;
    for (unsigned k = 0; k < 2; ++k) {
        dcap[k] = t->dif[k];
        dl[k] = rv_dif + off; off += dcap[k];
        if (rv.dp[k] >= dcap[k]) rv.dp[k] = 0;
    }
    if (rv_ppos >= RV_PRE_MAX) rv_ppos = 0;
    float hp = rv.hp, lp = rv.lp, ph = rv.ph, prevx = rv.prevx, last0 = rv.last[0], last1 = rv.last[1];
    const float khp = t->khp, klp = t->klp, gd = t->gd, dph = t->dph, md = t->md, gin = rv_gin;
    const float gm = 0.5f * rv_gout, gs = 0.5f * rv_gout * t->wid;
    const unsigned len0 = t->len[0];
    /* Local copies: the line stores (int16) may alias the uint16 positions, which would force reloads. */
    unsigned p[8], dp0 = rv.dp[0], dp1 = rv.dp[1];
    float f[8], ca[8], cb[8];
    for (unsigned k = 0; k < 8; ++k) { p[k] = rv.p[k]; f[k] = rv.f[k]; ca[k] = rv_a[k]; cb[k] = rv_b[k]; }
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; i += 2) {
        float m0 = 0.5f * (l[i] + r[i]), m1 = 0.5f * (l[i + 1] + r[i + 1]);
        float in = 0.25f * prevx + 0.5f * m0 + 0.25f * m1;
        prevx = m1;
        in -= hp; hp = (hp + khp * in + RV_DN) - RV_DN;   /* low cut */
        lp = (lp + klp * (in - lp) + RV_DN) - RV_DN;      /* band limit */
        rv_pre[rv_ppos] = wr16(lp);
        unsigned tap = rv_ppos >= pre ? rv_ppos - pre : rv_ppos + RV_PRE_MAX - pre;
        if (++rv_ppos >= RV_PRE_MAX) rv_ppos = 0;
        float x = (float)rv_pre[tap] * gin;                /* LSB units from here on */
        {   /* allpass diffusers */
            float b = (float)dl[0][dp0], w = x + gd * b;
            dl[0][dp0] = wl16(w);
            if (++dp0 >= dcap[0]) dp0 = 0;
            x = b - gd * w;
            b = (float)dl[1][dp1]; w = x + gd * b;
            dl[1][dp1] = wl16(w);
            if (++dp1 >= dcap[1]) dp1 = 0;
            x = b - gd * w;
        }
        ph += dph; if (ph >= 1.0f) ph -= 1.0f;
        float v[8];
        {   /* line 0: modulated read, linear interpolation */
            float d = (float)len0 - md * (ph < 0.5f ? 2.0f * ph : 2.0f - 2.0f * ph);
            unsigned di = (unsigned)d, w0 = p[0];
            float fr = d - (float)di;
            unsigned q0 = w0 >= di ? w0 - di : w0 + cap[0] - di, q1 = q0 ? q0 - 1U : cap[0] - 1U;
            float y0 = (float)ln[0][q0];
            v[0] = y0 + fr * ((float)ln[0][q1] - y0);
        }
        for (unsigned k = 1; k < 8; ++k) v[k] = (float)ln[k][p[k]];
        float s = 0;
        for (unsigned k = 0; k < 8; ++k) { f[k] = (ca[k] * v[k] + cb[k] * f[k] + RV_DN) - RV_DN; s += f[k]; }
        s *= 0.25f;
        float L = (f[0] - f[1]) + (f[2] - f[3]) + (f[4] - f[5]) + (f[6] - f[7]);
        float R = (f[0] + f[1]) - (f[2] + f[3]) + (f[4] + f[5]) - (f[6] + f[7]);
        float xs = x - s, xn = -x - s;
        for (unsigned k = 0; k < 4; ++k) { ln[k][p[k]] = wl16(f[k] + xs); if (++p[k] >= cap[k]) p[k] = 0; }
        for (unsigned k = 4; k < 8; ++k) { ln[k][p[k]] = wl16(f[k] + xn); if (++p[k] >= cap[k]) p[k] = 0; }
        float a = gm * (L + R) + gs * (L - R), b = gm * (L + R) - gs * (L - R);
        l[i] += mix * 0.5f * (a + last0); l[i + 1] += mix * a; /* linear upsampling */
        r[i] += mix * 0.5f * (b + last1); r[i + 1] += mix * b;
        last0 = a; last1 = b;
    }
    if (fabsf_(hp) < 1e-15f) hp = 0.0f; /* the (x + RV_DN) - RV_DN rounding can hold ~1e-24 here; -300 dBFS */
    if (fabsf_(lp) < 1e-15f) lp = 0.0f;
    for (unsigned k = 0; k < 8; ++k) { rv.p[k] = (uint16_t)p[k]; rv.f[k] = f[k]; }
    rv.dp[0] = (uint16_t)dp0; rv.dp[1] = (uint16_t)dp1;
    rv.hp = hp; rv.lp = lp; rv.ph = ph; rv.prevx = prevx; rv.last[0] = last0; rv.last[1] = last1;
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
/* Fixed 20 Hz high-pass, two one-pole stages per channel, at the end of the
 * float chain (ring difference tones, reverb tails): the limiter would hold
 * DC and sub-bass at full level. The integer path has none (after a fade). */
static inline __attribute__((always_inline)) float subcut_sample(float x, float *s0, float *s1)
{
    const float k = 0.0028459f; /* 1 - exp(-2 pi 20 / 44100): 20 Hz, so 40..60 Hz bass loses under 2 dB */
    float y = x - *s0;
    *s0 += k * y + 1e-20f; /* + 1e-20: no denormals in silence */
    float z = y - *s1;
    *s1 += k * z + 1e-20f;
    return z;
}
/* Float path: heard share from -> to across the block (per-sample ramp, from == to == 1 = plain filter). */
static FM1_AUDIO_CODE void run_subcut(float *l, float *r, float from, float to)
{
    float *ch[2] = {l, r};
    const float step = (to - from) * (1.0f / FM1_BLOCK_FRAMES);
    for (unsigned c = 0; c < 2; ++c) {
        float s0 = sub_hp[c][0], s1 = sub_hp[c][1], g = from;
        if (from == 1.0f && to == 1.0f)
            for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) ch[c][i] = subcut_sample(ch[c][i], &s0, &s1);
        else
            for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
                float x = ch[c][i], z = subcut_sample(x, &s0, &s1);
                g += step; /* frame i ends at from + (to - from) * (i + 1) / FRAMES: the last one at 'to' */
                ch[c][i] = x + g * (z - x);
            }
        sub_hp[c][0] = s0; sub_hp[c][1] = s1;
    }
}
/* At rest the filter is idle: it restarts from zero (passes the first sample as it is, no step) instead of
 * replaying an offset remembered from long ago. */
static inline __attribute__((always_inline)) void subcut_rest(void)
{
    sub_hp[0][0] = sub_hp[0][1] = sub_hp[1][0] = sub_hp[1][1] = 0.0f;
}
/* Integer path while sub_g > 0 (the MFX_SUB_FADE blocks after the float path stopped): the filter keeps
 * running on the integer samples and only its correction, faded, is added; at 0 it goes to rest. With
 * sub_g == 0 the integer path does none of this (one compare). */
static FM1_AUDIO_CODE void subcut_fade_out(int32_t *buffer)
{
    float from = sub_g, to = from - 1.0f / MFX_SUB_FADE;
    if (to < 0.0f) to = 0.0f;
    const float step = (to - from) * (1.0f / FM1_BLOCK_FRAMES);
    for (unsigned c = 0; c < 2; ++c) {
        float s0 = sub_hp[c][0], s1 = sub_hp[c][1], g = from;
        for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
            int32_t v = buffer[2 * i + c];
            float x = (float)v * (1.0f / 8388608.0f), z = subcut_sample(x, &s0, &s1);
            g += step;
            /* Only the faded correction is added: the sum may be past full scale here and run_output_int limits
             * it next (no 24-bit clamp ahead of the limiter). The correction is the filter's low part, a few
             * times the input at most; |v| stays within a few full scales (engine output + MICRO D), so v + corr
             * cannot leave int32. The float bound only keeps the conversion defined for any input. */
            float corr = g * (z - x) * 8388608.0f;
            corr = corr > 1.0e9f ? 1.0e9f : corr < -1.0e9f ? -1.0e9f : corr;
            buffer[2 * i + c] = v + (int32_t)corr;
        }
        sub_hp[c][0] = s0; sub_hp[c][1] = s1;
    }
    sub_g = to;
    if (to <= 0.0f) subcut_rest();
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
    static const float mids[5] FM1_AUDIO_DATA = {250, 500, 1000, 2000, 4000};
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
/* Output boost and peak limiter: the block peak sets the gain (attack
 * ramped across one block, ~3 ms, so no step at the block edge; ~150 ms
 * release); a soft knee catches what the ramp lets through. */
static FM1_AUDIO_CODE void run_output(float *l, float *r)
{
    static const float boost[5] FM1_AUDIO_DATA = {1.0f, 1.4125375f, 1.9952623f, 2.8183829f, 3.9810717f};
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
        float m = g * k;
        float x = l[i] * m, y = r[i] * m;
        l[i] = x > 0.9f ? 0.9f + (x - 0.9f) / (1.0f + (x - 0.9f) * 10.0f) : x < -0.9f ? -0.9f + (x + 0.9f) / (1.0f - (x + 0.9f) * 10.0f) : x;
        r[i] = y > 0.9f ? 0.9f + (y - 0.9f) / (1.0f + (y - 0.9f) * 10.0f) : y < -0.9f ? -0.9f + (y + 0.9f) / (1.0f - (y + 0.9f) * 10.0f) : y;
    }
    lim_q12 = boost_q12[out_step % 5]; /* the integer path starts from rest, as before (no stale gain from long ago) */
}
/* Same boost and limiter in fixed point (Q12 gain), used when no float
 * effect is running so the plain synth path stays integer. Runs at 0 dB
 * too (MICRO D joins the mix after the engine limiter): with nothing over
 * the limit and the gain at rest it costs one peak scan and leaves every
 * sample bit-identical. */
static FM1_AUDIO_CODE void run_output_int(int32_t *buffer)
{
    const int32_t limit = 7465860; /* 0.89 of full scale */
    int32_t g = boost_q12[out_step % 5], peak = 1;
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
        int32_t a = buffer[i] < 0 ? -buffer[i] : buffer[i];
        if (a > peak) peak = a;
    }
    /* No 64-bit division in the interrupt: step the gain down until it fits. */
    int32_t want = g;
    for (unsigned n = 0; n < 64 && want > 256 && (int64_t)peak * want > ((int64_t)limit << 12); ++n) want -= want >> 4;
    int32_t start = lim_q12, end = want < start ? want : start + (want - start) / 50 + (want > start);
    lim_q12 = end;
    lim_gain = (float)end / (float)g; /* the float path goes on from here */
    if (start == 4096 && end == 4096) return; /* unity all through the block: samples stay as they are */
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
/* No clicks on changes. A new TYPE fades the effect out (8 blocks, ~23 ms),
 * then (the block after it reached 0) clears its memory, switches and fades
 * back in; an effect that starts again (MIX up from 0) starts from clean
 * memory, not from an old tail. The fade gain fx_gain moves once per block,
 * but mfx_run ramps it per sample across the block (no edge steps).
 * FILTER while it runs is different: a TYPE change crossfades old type ->
 * new type (two instances, see run_filter_xf), never through the dry signal
 * (a low-passed sound would get a burst of its bright input).
 * Clearing is sliced: at most FX_CLEAR_BUDGET bytes per block over all
 * effects together (chorus 16 KB + reverb 16.5 KB + comb 9.6 KB at once took
 * too long inside the interrupt). An effect whose memory is not clean yet
 * stays silent and is not run (fx_gain 0, its MIX forced to 0 for the
 * block), so it never reads stale memory; once clean it is primed (see fx_prime) and fades in over 16 blocks (chorus 32). */
enum { FX_CLEAR_BUDGET = 4096 };
static uint8_t fx_was_on[MFX_COUNT];
static float fx_gain[MFX_COUNT] = {1, 1, 1, 1, 1, 1, 1};
/* Load guard shedding (mfx_shed): shed_g = each unit's guard gain (1 in,
 * 0 out), on top of fx_gain; it falls 1/MFX_SHED_FADE per block while the
 * unit is in shed_fade, so the unit fades out over a few blocks (per-sample
 * ramp in fx_end). shed_out = units fully out this block: not run, no
 * clearing; an effect leaves its memory dirty (fx_was_on = 0), so it comes
 * back like a fresh start: cleared, primed, faded in by fx_gain. */
static float shed_g[MFX_UNITS] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
static volatile uint16_t shed_fade, shed_cut;
static uint16_t shed_out, shed_zero;
/* Unit times of the last mfx_run (see mfx_units_timed). */
static uint32_t (*mfx_lap)(uint32_t *mark);
static uint32_t unit_ticks[MFX_UNITS];
static uint16_t units_timed;
#ifdef FM1_HOST_TEST
void (*mfx_test_spend)(unsigned unit);
#endif
void mfx_clock_set(uint32_t (*lap)(uint32_t *mark)) { mfx_lap = lap; }
FM1_AUDIO_CODE void mfx_shed(unsigned fade, unsigned cut) { shed_fade = (uint16_t)fade; shed_cut = (uint16_t)(cut & fade); }
FM1_AUDIO_CODE unsigned mfx_units_silent(void) { return shed_zero; }
FM1_AUDIO_CODE unsigned mfx_units_timed(void) { return units_timed; }
FM1_AUDIO_CODE uint32_t mfx_unit_ticks(unsigned unit) { return unit < MFX_UNITS ? unit_ticks[unit] : 0U; }
/* memset is library code in Flash; the audio ISR must not call it (it also
 * runs while Flash is written). Volatile stores keep the compiler from
 * turning this loop back into a memset call. */
#ifdef FM1_HOST_TEST
unsigned mfx_test_zeroed; /* host test only: bytes fx_zero cleared */
void (*mfx_test_zero)(unsigned bytes); /* host test only: called per fx_zero (to spend test time) */
#endif
static FM1_AUDIO_CODE void fx_zero(void *p, unsigned bytes)
{
#ifdef FM1_HOST_TEST
    mfx_test_zeroed += bytes;
    if (mfx_test_zero) mfx_test_zero(bytes);
#endif
    volatile uint8_t *b = p;
    while (bytes && ((uintptr_t)b & 3U)) { *b++ = 0; --bytes; }
    volatile uint32_t *w = (volatile uint32_t *)b;
    for (; bytes >= 4U; bytes -= 4U) *w++ = 0;
    b = (volatile uint8_t *)w;
    while (bytes--) *b++ = 0;
}
/* The memory each effect clears, in order; fx_first[fx] .. fx_first[fx + 1] - 1
 * are its regions. RAM tables (the interrupt also runs while Flash is busy). */
/* FILTER regions are those of instance 0; fx_clear_step moves them to the current instance flt_cur. */
static const struct { void *p; uint16_t bytes; } fx_regions[] FM1_AUDIO_DATA = {
    {&flt[0], FLT_SMALL}, {flt[0].comb, sizeof flt[0].comb},                           /* FILTER */
    {tone, sizeof tone}, {hold, sizeof hold}, {drv_prev, sizeof drv_prev}, {dc_x, sizeof dc_x}, {dc_y, sizeof dc_y}, /* DRIVE */
    {modbuf, sizeof modbuf},                                                            /* CHORUS */
    {ap, sizeof ap}, {ph_fb, sizeof ph_fb},                                             /* PHASER */
    {hb_s, sizeof hb_s}, {hb_x, sizeof hb_x}, {trk_env, sizeof trk_env},                /* RING */
    {&rv, sizeof rv}, {rv_dif, sizeof rv_dif}, {rv_pre, sizeof rv_pre}, {rv_line, sizeof rv_line}}; /* REVERB */
static const uint8_t fx_first[MFX_COUNT + 1] FM1_AUDIO_DATA = {0, 2, 7, 8, 10, 13, 17, 17};
_Static_assert(sizeof modbuf <= 65535 && sizeof rv_line <= 65535 && sizeof flt[0].comb <= 65535, "region sizes fit uint16_t");
_Static_assert(sizeof fx_regions / sizeof fx_regions[0] == 17, "fx_first matches fx_regions");
/* Clearing in progress: fx_dirty set, next region fx_clr_reg, bytes done in it fx_clr_off. */
static uint8_t fx_dirty[MFX_COUNT], fx_clr_reg[MFX_COUNT];
static uint16_t fx_clr_off[MFX_COUNT];
/* Priming: once clean, an effect runs this many blocks at gain 0 so its
 * delay line holds signal before it is heard (a line started from zero on a
 * running signal gives a step when the first written sample reaches the read
 * tap). CHORUS: longest tap 600 x 1.35 + 320 = 1130 samples -> 9 blocks;
 * FLANGE: 44 x 1.35 + 176 = 236 -> 2 blocks. The others need none (reverb:
 * it only outputs what passed its lines, >= 171 samples, so it rises from 0). */
static uint8_t fx_prime[MFX_COUNT];
static unsigned fx_cleared; /* bytes zeroed in the last mfx_process (0 = an honest timing block) */
/* Not an honest timing block: the last mfx_process zeroed effect memory, or ran two FILTER instances
 * side by side (a TYPE crossfade, about twice the filter's cost). The load guard skips such blocks. */
FM1_AUDIO_CODE unsigned mfx_clearing(void) { return fx_cleared != 0 || xf_ran; }
/* Start clearing fx from its first region (scalars at once, they cost nothing). */
static FM1_AUDIO_CODE void fx_clear(unsigned fx)
{
    if (fx == MFX_FILTER) { flt_g[flt_cur].cut_smooth = flt_g[flt_cur].cut_prev = -1.0f; xf_stage = 0; } /* k_prev kept, as before */
    else if (fx == MFX_VERB) rv_key = 0; /* positions and float states are in the cleared region rv */
    fx_dirty[fx] = fx_first[fx] < fx_first[fx + 1];
    fx_clr_reg[fx] = fx_first[fx];
    fx_clr_off[fx] = 0;
    fx_prime[fx] = fx != MFX_CHORUS ? 0 : fx_type[fx] == 1 ? 2 : 9;
}
/* Zero up to budget bytes of fx's memory; returns the bytes used. */
static FM1_AUDIO_CODE unsigned fx_clear_step(unsigned fx, unsigned budget)
{
    unsigned used = 0;
    while (fx_dirty[fx] && used < budget) {
        unsigned r = fx_clr_reg[fx], off = fx_clr_off[fx];
        unsigned left = fx_regions[r].bytes - off, n = budget - used;
        if (n > left) n = left;
        fx_zero((uint8_t *)fx_regions[r].p + off + (fx == MFX_FILTER ? flt_cur * sizeof(flt_state) : 0U), n);
        used += n; off += n;
        if (off >= fx_regions[r].bytes) {
            off = 0;
            if (++r >= fx_first[fx + 1]) fx_dirty[fx] = 0;
        }
        fx_clr_reg[fx] = (uint8_t)r; fx_clr_off[fx] = (uint16_t)off;
    }
    return used;
}
/* FILTER TYPE change while it runs: prepare the incoming instance (the one not playing) for `type`: small
 * state zeroed at once, glides from the current CUTOFF / RESO (no glide from an old value). Only COMB reads
 * the comb line, so only a COMB start needs it clean (stage 1, sliced). Returns the bytes zeroed. */
static FM1_AUDIO_CODE unsigned xf_begin(unsigned type)
{
    unsigned n = flt_cur ^ 1U;
    fx_zero(&flt[n], FLT_SMALL);
    flt_g[n].cut_smooth = flt_g[n].cut_prev = flt_g[n].k_prev = -1.0f;
    flt_g[n].comb_pos = 0;
    xf_type = (uint8_t)type; xf_pos = 0;
    xf_off = type == 3 ? 0 : (uint16_t)sizeof flt[0].comb;
    xf_stage = type == 3 ? 1 : 2;
    return FLT_SMALL;
}
/* One block of the FILTER TYPE change logic (fx_smooth, filter on and clean); returns the bytes zeroed.
 * While the incoming type is not heard yet (clearing or priming) it follows the knob, and turning back to
 * the playing type cancels the change. Once the crossfade is audible it runs to its end; a newer TYPE
 * starts the next crossfade after it, so the last chosen type wins and nothing jumps. */
static FM1_AUDIO_CODE unsigned xf_step(unsigned want, unsigned budget)
{
    unsigned used = 0;
    if (xf_stage && xf_pos < XF_PRIME && want != xf_type) {
        xf_stage = 0;
        if (want == fx_type[MFX_FILTER]) return 0;
    }
    if (!xf_stage) used = xf_begin(want);
    if (xf_stage == 1) {
        unsigned left = sizeof flt[0].comb - xf_off, n = budget > used ? budget - used : 0U;
        if (n > left) n = left;
        fx_zero((uint8_t *)flt[flt_cur ^ 1U].comb + xf_off, n);
        xf_off = (uint16_t)(xf_off + n); used += n;
        if (xf_off >= sizeof flt[0].comb) xf_stage = 2;
    }
    return used;
}
static FM1_AUDIO_CODE void fx_smooth(void)
{
    unsigned budget = FX_CLEAR_BUDGET;
    fx_cleared = 0; xf_ran = 0;
    for (unsigned fx = 0; fx < MFX_COUNT; ++fx) {
        if (fx == MFX_EQ) continue;
        if ((shed_out >> fx) & 1U) { /* shed by the load guard: off, memory left dirty, cleared on return */
            fx_was_on[fx] = 0; ep[fx][3] = 0;
            if (fx == MFX_FILTER) xf_stage = 0;
            continue;
        }
        unsigned want = ep[fx][0], on = prm[fx][3] != 0; /* the user's MIX: an LFO dip to 0 keeps the tail */
        unsigned switched = 0; /* a TYPE switch this block: the fade-in starts next block, as before */
        unsigned xf = 0;       /* FILTER TYPE crossfade path this block */
        if (fx_type[fx] == 255) fx_type[fx] = (uint8_t)want;
        if (on && !fx_was_on[fx]) { fx_type[fx] = (uint8_t)want; fx_clear(fx); fx_gain[fx] = 0.0f; }
        fx_was_on[fx] = (uint8_t)on;
        if (fx == MFX_FILTER && on && !fx_dirty[fx] && (xf_stage || want != fx_type[fx])) {
            unsigned n = xf_step(want, budget);
            budget -= n; fx_cleared += n; xf = 1;
        } else if (want != fx_type[fx]) {
            /* Fade out 1/8 per block; the block that reaches 0 still plays the old type (its ramp ends at
             * 0), the next one switches and clears: no step at either edge. */
            if (fx_gain[fx] <= 0.0f) { fx_gain[fx] = 0.0f; fx_type[fx] = (uint8_t)want; fx_clear(fx); switched = 1; }
            else { fx_gain[fx] -= 0.125f; if (fx_gain[fx] < 0.0f) fx_gain[fx] = 0.0f; }
        }
        if (fx_dirty[fx]) {
            unsigned n = fx_clear_step(fx, budget);
            budget -= n; fx_cleared += n;
        }
        if (fx_dirty[fx]) { fx_gain[fx] = 0.0f; ep[fx][3] = 0; } /* silent and not run until clean */
        else if (fx_prime[fx]) { --fx_prime[fx]; fx_gain[fx] = 0.0f; } /* runs, not heard yet */
        else if (!switched && (xf || want == fx_type[fx]) && fx_gain[fx] < 1.0f) {
            /* Fade-in 1/16 per block (~46 ms; was 1/8), CHORUS 1/32 (~93 ms); mfx_run ramps the gain
             * per sample across each block, so these are slopes, not steps. Fade-out of a TYPE
             * change stays 1/8. */
            fx_gain[fx] += fx == MFX_CHORUS ? 0.03125f : 0.0625f;
            if (fx_gain[fx] > 1.0f) fx_gain[fx] = 1.0f;
        }
        ep[fx][0] = fx_type[fx];
    }
}
static FM1_AUDIO_CODE void mfx_run(int32_t *buffer);
/* COMP (v12): a glue compressor on the whole mix, after EQ / LOW CUT and
 * before the output limiter. Block-rate peak detector (stereo linked),
 * attack / release per block, the gain ramped across the block, automatic
 * makeup. Controls: THRESH 0..15 (0..-30 dB), RATIO 0..3 (2, 4, 8, 20 : 1),
 * SPEED 0..2 (fast / med / slow), MIX 0..127 (0 = off). */
static volatile uint8_t comp_prm[4] = {6, 1, 1, 0};
static float comp_env, comp_gain = 1.0f, comp_gr_db;
static FM1_AUDIO_CODE float flog2(float x) /* ~0.01 accurate, x > 0 */
{
    union { float f; uint32_t u; } v = {x};
    float e = (float)(int)((v.u >> 23) & 255U) - 127.0f;
    v.u = (v.u & 0x007fffffU) | 0x3f800000U; /* mantissa 1..2 */
    float m = v.f;
    return e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
}
static FM1_AUDIO_CODE void run_comp(float *l, float *r)
{
    static const float ratio[4] FM1_AUDIO_DATA = {2.0f, 4.0f, 8.0f, 20.0f};
    static const float att[3] FM1_AUDIO_DATA = {0.7f, 0.35f, 0.15f}, rel[3] FM1_AUDIO_DATA = {0.06f, 0.02f, 0.008f}; /* per block (2.9 ms) */
    unsigned sp = comp_prm[2] % 3U;
    float thr_db = -2.0f * (float)comp_prm[0], k = 1.0f - 1.0f / ratio[comp_prm[1] & 3U];
    float mix = comp_prm[3] * (1.0f / 127);
    float peak = 1e-9f;
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        float a = fabsf_(l[i]), b = fabsf_(r[i]);
        if (a > peak) peak = a; if (b > peak) peak = b;
    }
    comp_env += (peak - comp_env) * (peak > comp_env ? att[sp] : rel[sp]);
    float lvl_db = 6.0206f * flog2(comp_env > 1e-9f ? comp_env : 1e-9f), over = lvl_db - thr_db;
    float gr_db = over > 0 ? -over * k : 0.0f, makeup_db = -thr_db * k * 0.5f;
    comp_gr_db = gr_db;
    float want = fexp2((gr_db + makeup_db) * (1.0f / 6.0206f));
    float g0 = comp_gain, step = (want - g0) * (1.0f / FM1_BLOCK_FRAMES);
    comp_gain = want;
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        float g = 1.0f + mix * (g0 + step * (float)i - 1.0f);
        l[i] *= g; r[i] *= g;
    }
}
void mfx_comp_edit(unsigned param, int d)
{
    static const uint8_t top[4] = {15, 3, 2, 127};
    if (param > 3 || !d) return;
    int v = (int)comp_prm[param] + (param < 3 ? (d > 0 ? 1 : -1) : d);
    comp_prm[param] = (uint8_t)(v < 0 ? 0 : v > top[param] ? top[param] : v);
}
unsigned mfx_comp_value(unsigned param) { return param < 4 ? comp_prm[param] : 0; }
int mfx_comp_reduction_db(void) { return (int)(comp_gr_db - 0.5f); }
/* MIX glides block to block (~10 ms), so a fast knob or an LFO on MIX
 * does not click; the fade of a type change (fx_gain) stays on top. */
static float mix_s[MFX_COUNT];
static FM1_AUDIO_CODE float mix_glide(unsigned fx)
{
    float want = ep[fx][3] * (1.0f / 127);
    if (!fx_was_on[fx] || mix_s[fx] <= 0.0f) mix_s[fx] = want; /* a fresh start fades in by fx_gain */
    else mix_s[fx] += (want - mix_s[fx]) * 0.3f;
    return mix_s[fx];
}
/* Fade gain per sample: fx_gain moves once per block. Every effect mixes linearly in its mix argument
 * (x + mix * (y - x); reverb adds mix * out; chorus 0.5 * mix), so in a block where the gain differs from
 * the one heard in the block before, the effect runs at full fade gain on the block, and fx_end blends
 * before -> after with a gain ramping linearly from the old to the new value (last sample = new value).
 * fx_heard is 0 for an effect that did not run. Blocks with a steady gain: no copy, the same arithmetic
 * as before (mix * fx_gain). */
static float fx_before[2][FM1_BLOCK_FRAMES], fx_heard[MFX_UNITS]; /* BSS: the interrupt stack is small */
/* The gain a unit is heard at: an effect's fade gain times the load guard's gain; EQ / COMP / LOW CUT have only
 * the guard's (they always started at once, with no fade). */
static inline __attribute__((always_inline)) float unit_gain(unsigned u) { return u < MFX_EQ ? fx_gain[u] * shed_g[u] : shed_g[u]; }
/* A unit that does not run: effects are heard at 0 (they fade in from 0 when they start); EQ / COMP / LOW CUT
 * follow the guard's gain, so switching one on is heard at once as before. */
static inline __attribute__((always_inline)) void unit_idle(unsigned u) { fx_heard[u] = u < MFX_EQ ? 0.0f : shed_g[u]; }
/* Copy the block before a unit whose heard gain changes this block; returns 1 when it did. */
static inline __attribute__((always_inline)) unsigned blend_begin(unsigned u, const float *l, const float *r)
{
    if (unit_gain(u) == fx_heard[u]) return 0;
    volatile float *bl = fx_before[0], *br = fx_before[1]; /* volatile: the copy must not become a memcpy call */
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) { bl[i] = l[i]; br[i] = r[i]; }
    return 1;
}
static inline __attribute__((always_inline)) float fx_begin(unsigned fx, const float *l, const float *r)
{
    float m = mix_glide(fx);
    return blend_begin(fx, l, r) ? m : m * unit_gain(fx);
}
static inline __attribute__((always_inline)) void fx_end(unsigned fx, float *l, float *r)
{
    float g0 = fx_heard[fx], g1 = unit_gain(fx);
    fx_heard[fx] = g1;
    if (g0 == g1) return;
    float dg = (g1 - g0) * (1.0f / FM1_BLOCK_FRAMES);
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        float g = g0 + dg * (float)(i + 1U);
        l[i] = fx_before[0][i] + g * (l[i] - fx_before[0][i]);
        r[i] = fx_before[1][i] + g * (r[i] - fx_before[1][i]);
    }
}
static inline __attribute__((always_inline)) void fx_unheard(void)
{
    for (unsigned u = 0; u < MFX_UNITS; ++u) unit_idle(u);
}
/* Boot chime (user 07.11, octave up 09.11): F - A - C - F rising (F4 A4 C5 F5,
 * 349.2 / 440 / 523.3 / 698.5 Hz), one note with each splash wave
 * (screen.c boot_note_ms 300 / 833 / 1900 / 2700: user 10.11, F10, the second
 * note 1/3 of the 800 ms step earlier, at 533 ms from the first), ringing on;
 * then at 3 200 ms a chord of all four notes together (voices 4..7, the same
 * notes at 0.7 of their level); the top F a little accented.
 * Soft FM tones (modulator 2 x carrier, low index), 12 ms attack, ~2.6 s tail.
 * Added to the output once after a normal start. No libm: the sine table
 * comes from the rotation recurrence. */
enum { CHIME_VOICES = 8, CHIME_ATTACK = 529 /* 12 ms */ };
static float chime_sin[257];
static volatile uint8_t chime_go;
static struct { uint32_t pc, pm, dc, dm, wait, age; float amp, peak, idx; } chime_v[CHIME_VOICES];
void mfx_chime_start(void)
{
    if (chime_go) return;
    float c = 0.99969881869620425f, prev = 0.0f, cur = 0.02454122852291229f; /* cos, sin of 2 pi / 256 */
    chime_sin[0] = 0.0f;
    for (unsigned i = 1; i <= 256; ++i) { chime_sin[i] = cur; float next = 2.0f * c * cur - prev; prev = cur; cur = next; }
    static const float hz[CHIME_VOICES] = {349.23f, 440.00f, 523.25f, 698.46f,  /* F4 A4 C5 F5 (user 09.11) */
                                           349.23f, 440.00f, 523.25f, 698.46f}; /* the final chord */
    static const uint32_t at_ms[CHIME_VOICES] = {0, 533, 1600, 2400, 3200, 3200, 3200, 3200};
    static const float level[CHIME_VOICES] = {0.060f, 0.055f, 0.055f, 0.065f,
                                              0.060f * 0.7f, 0.055f * 0.7f, 0.055f * 0.7f, 0.065f * 0.7f};
    for (unsigned v = 0; v < CHIME_VOICES; ++v) {
        chime_v[v].pc = chime_v[v].pm = chime_v[v].age = 0;
        chime_v[v].dc = (uint32_t)(hz[v] * (4294967296.0f / FS));
        chime_v[v].dm = (uint32_t)(hz[v] * 2.0f * (4294967296.0f / FS));
        chime_v[v].wait = at_ms[v] * 441U / 10U;
        chime_v[v].amp = 0.0f;
        chime_v[v].peak = level[v];
        chime_v[v].idx = 0.9f;
    }
    chime_go = 1;
}
#ifdef FM1_HOST_TEST
unsigned mfx_test_chime_frames, mfx_test_chime_onset[CHIME_VOICES]; /* host test only: frames mixed, first sounding frame + 1 */
#endif
static FM1_AUDIO_CODE void chime_mix(int32_t *buffer)
{
    unsigned alive = 0;
    for (unsigned n = 0; n < FM1_BLOCK_FRAMES; ++n) {
        float sum = 0.0f;
        for (unsigned v = 0; v < CHIME_VOICES; ++v) {
            if (chime_v[v].wait) { --chime_v[v].wait; continue; }
#ifdef FM1_HOST_TEST
            if (!mfx_test_chime_onset[v]) mfx_test_chime_onset[v] = mfx_test_chime_frames + 1U;
#endif
            if (chime_v[v].age < CHIME_ATTACK) chime_v[v].amp = chime_v[v].peak * (float)++chime_v[v].age / CHIME_ATTACK;
            else { chime_v[v].amp *= 0.99994f; chime_v[v].idx *= 0.99990f; } /* ~2.6 s to -60 dB, tone softens */
            float m = chime_sin[chime_v[v].pm >> 24];
            int32_t bend = (int32_t)(chime_v[v].idx * m * 683565275.6f); /* radians to 2^32 per turn */
            sum += chime_v[v].amp * chime_sin[(chime_v[v].pc + (uint32_t)bend) >> 24];
            chime_v[v].pc += chime_v[v].dc;
            chime_v[v].pm += chime_v[v].dm;
        }
#ifdef FM1_HOST_TEST
        ++mfx_test_chime_frames;
#endif
        int32_t add = (int32_t)(sum * 8388607.0f);
        for (unsigned ch = 0; ch < 2; ++ch) {
            int32_t x = buffer[2 * n + ch] + add;
            buffer[2 * n + ch] = x > 8388607 ? 8388607 : x < -8388607 ? -8388607 : x;
        }
    }
    for (unsigned v = 0; v < CHIME_VOICES; ++v) alive |= chime_v[v].wait || chime_v[v].age < CHIME_ATTACK || chime_v[v].amp > 0.0001f;
    if (!alive) chime_go = 0;
}
FM1_AUDIO_CODE void mfx_process(int32_t *buffer)
{
    if (!buffer) return;
    if (chime_go) chime_mix(buffer);
    mfx_run(buffer);
    if (scope_on) scope_capture(buffer);
}
FM1_AUDIO_CODE unsigned mfx_units_on(void)
{
    unsigned on = lowcut_on ? 1U << MFX_U_LOWCUT : 0U;
    for (unsigned fx = 0; fx < MFX_EQ; ++fx) if (prm[fx][3]) on |= 1U << fx; /* the user's MIX, as fx_smooth */
    for (unsigned p = 1; p < 4; ++p) {
        int v = (int)prm[MFX_EQ][p] + (int)mods[MFX_EQ][p];
        if ((v < 0 ? 0 : v > 127 ? 127 : v) != 64) on |= 1U << MFX_EQ; /* as mfx_run: an LFO on a gain counts */
    }
    if (comp_prm[3]) on |= 1U << MFX_U_COMP;
    return on;
}
/* One block of the load guard's gains (see shed_g): fade the shed units out, cut the `cut` ones now, bring
 * the others back. A unit whose gain was already 0 at the start of the block is out. */
static FM1_AUDIO_CODE void shed_step(void)
{
    unsigned fade = shed_fade, cut = shed_cut, out = 0, zero = 0;
    for (unsigned u = 0; u < MFX_UNITS; ++u) {
        unsigned bit = 1U << u;
        float g = shed_g[u];
        if (cut & bit) { g = 0.0f; out |= bit; }
        else if (fade & bit) {
            if (g <= 0.0f) out |= bit;
            g -= 1.0f / MFX_SHED_FADE;
            if (g < 0.0f) g = 0.0f;
        } else if (g < 1.0f) {
            if ((shed_out & bit) && u < MFX_EQ) g = 1.0f; /* an effect restarts from clean memory: fx_gain fades it in */
            else { g += 0.0625f; if (g > 1.0f) g = 1.0f; } /* 16 blocks, as an effect's fade-in */
        }
        shed_g[u] = g;
        if (g <= 0.0f) zero |= bit;
    }
    unsigned left = out & ~shed_out; /* out from this block on: no state kept */
    if (left & (1U << MFX_EQ)) { volatile float *z = &eq_z[0][0][0]; for (unsigned i = 0; i < 12; ++i) z[i] = 0.0f; }
    if (left & (1U << MFX_U_LOWCUT)) lc1[0] = lc1[1] = lc2[0] = lc2[1] = 0.0f;
    shed_out = (uint16_t)out; shed_zero = (uint16_t)zero;
}
_Static_assert(sizeof eq_z == 12 * sizeof(float), "eq_z is 12 floats");
static FM1_AUDIO_CODE void mfx_run(int32_t *buffer)
{
    unsigned any = 0;
    for (unsigned fx = 0; fx < MFX_COUNT; ++fx)
        for (unsigned p = 0; p < 4; ++p) {
            int v = (int)prm[fx][p] + (p ? (int)mods[fx][p] : 0);
            ep[fx][p] = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
        }
    shed_step();
    fx_smooth();
    units_timed = 0;
    for (unsigned fx = 0; fx < MFX_EQ; ++fx) any |= ep[fx][3];
    unsigned eq = (ep[MFX_EQ][1] != 64 || ep[MFX_EQ][2] != 64 || ep[MFX_EQ][3] != 64) && !((shed_out >> MFX_EQ) & 1U);
    unsigned lowcut = lowcut_on && !((shed_out >> MFX_U_LOWCUT) & 1U);
    unsigned comp = comp_prm[3] && !((shed_out >> MFX_U_COMP) & 1U);
    any |= lowcut | eq | comp;
    if (!any) { /* no effect running: integer boost and limiter only */
        flt_g[0].cut_smooth = flt_g[1].cut_smooth = -1.0f; fx_unheard();
        comp_env = 0; comp_gain = 1.0f; comp_gr_db = 0;
        if (sub_g > 0.0f) subcut_fade_out(buffer); /* the float path just stopped: the 20 Hz filter fades out */
        run_output_int(buffer); /* also at 0 dB: MICRO D can push the sum past full scale */
        return;
    }
    /* Block buffers live in BSS: the audio interrupt stack is small. */
    static float l[FM1_BLOCK_FRAMES], r[FM1_BLOCK_FRAMES];
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        l[i] = (float)buffer[2 * i] * (1.0f / 8388608.0f);
        r[i] = (float)buffer[2 * i + 1] * (1.0f / 8388608.0f);
    }
    /* Each unit is timed on its own (lap before and after) for the load guard. */
    uint32_t mark = 0;
    unsigned timed = 0;
#ifdef FM1_HOST_TEST
#define UNIT_SPEND(u) do { if (mfx_test_spend) mfx_test_spend(u); } while (0)
#else
#define UNIT_SPEND(u) do { } while (0)
#endif
#define LAP_START() do { if (mfx_lap) (void)mfx_lap(&mark); } while (0)
#define LAP_END(u) do { UNIT_SPEND(u); if (mfx_lap) unit_ticks[u] = mfx_lap(&mark); timed |= 1U << (u); } while (0)
#define FX_RUN(fx, call) do { if (ep[fx][3]) { LAP_START(); float m = fx_begin(fx, l, r); call; fx_end(fx, l, r); LAP_END(fx); }                               else unit_idle(fx); } while (0)
#define UNIT_RUN(u, on, call) do { if (on) { LAP_START(); (void)blend_begin(u, l, r); call; fx_end(u, l, r); LAP_END(u); }                                    else unit_idle(u); } while (0)
    FX_RUN(MFX_DRIVE, run_drive(l, r, m));
    FX_RUN(MFX_FILTER, run_filter_xf(l, r, m));
    if (!ep[MFX_FILTER][3]) for (unsigned n = 0; n < 2; ++n) flt_g[n].cut_smooth = flt_g[n].cut_prev = -1.0f; /* a TYPE crossfade waits */
    FX_RUN(MFX_CHORUS, run_chorus(l, r, m));
    FX_RUN(MFX_PHASER, run_phaser(l, r, m));
    FX_RUN(MFX_RING, run_ring(l, r, m));
    FX_RUN(MFX_VERB, run_verb(l, r, m));
    UNIT_RUN(MFX_EQ, eq, run_eq(l, r));
    UNIT_RUN(MFX_U_LOWCUT, lowcut, run_lowcut(l, r));
    UNIT_RUN(MFX_U_COMP, comp, run_comp(l, r));
    if (!comp) { comp_env = 0; comp_gain = 1.0f; comp_gr_db = 0; }
#undef FX_RUN
#undef UNIT_RUN
#undef LAP_START
#undef LAP_END
#undef UNIT_SPEND
    if (xf_ran) timed &= ~(1U << MFX_FILTER); /* two FILTER instances: not its running cost */
    units_timed = (uint16_t)timed;
    { /* no DC or sub-bass held at full level by the limiter */
        float from = sub_g, to = from + 1.0f / MFX_SUB_FADE;
        if (from <= 0.0f) from = to = 1.0f; /* from rest: zero state passes the first sample as it is */
        if (to > 1.0f) to = 1.0f;
        sub_g = to;
        run_subcut(l, r, from, to);
    }
    run_output(l, r); /* also at boost 0: a loud LADDER or DRIVE is limited, not clipped */
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
    out[30] = (uint8_t)(comp_prm[0] | comp_prm[1] << 4 | comp_prm[2] << 6); out[31] = comp_prm[3]; /* COMP */
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
    out_step = in[28] <= 4 ? in[28] : 0;
    lowcut_on = in[29] == 1;
    if (in[30] != 0xff && in[31] <= 127 && ((in[30] >> 6) & 3U) <= 2U) {
        comp_prm[0] = in[30] & 15U; comp_prm[1] = (in[30] >> 4) & 3U; comp_prm[2] = (in[30] >> 6) & 3U; comp_prm[3] = in[31];
    } else { comp_prm[0] = 6; comp_prm[1] = 1; comp_prm[2] = 1; comp_prm[3] = 0; }
}
