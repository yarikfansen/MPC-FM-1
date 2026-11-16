/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "binaural.h"
#if FM1_BINAURAL
#include "fm1_block.h"

/* Threads: the control task edits `cfg` and writes the req_* / tgt_* words;
 * the audio interrupt reads them and owns everything below "audio". Every
 * shared field is one aligned 32-bit word or one byte, so no lock is needed.
 * A session start or a timer change bumps req_gen; the audio side then
 * reloads its countdown, so the two never write the same word. */
enum { FULL = 1 << 18,            /* gain 1.0, Q18; every gain is a multiple of 128 */
       STEP = 256,                /* gain change per block: 0 -> FULL in 1024 blocks, about 3 s */
       END_BLOCKS = 3445,         /* the last 10 s of the timer fade out */
       BLOCKS_PER_MIN = 20672,    /* 44100 * 60 / 128 */
       BEAT_MIN = 5, BEAT_MAX = 400, CARRIER_MIN = 100, CARRIER_MAX = 500, NOISE_MAX = 20, TIMERS = 6 };
static const uint16_t preset_beat[BIN_PRESETS] = {20, 60, 100, 180, 400}; /* 0.1 Hz: 2 / 6 / 10 / 18 / 40 */
static const char *const preset_name[BIN_PRESETS + 2] = {"OFF", "DELTA", "THETA", "ALPHA", "BETA", "FOCUS", "CUSTOM"};
static const uint8_t timer_min[TIMERS] = {0, 5, 10, 20, 30, 60};
/* noise level 1..10, 3 dB apart; level 10 = 0.44 (Q18), so the noise is at most
 * 0.44 full scale (raw noise is clamped to full scale) and with the tone
 * (0.25 at most) the sum stays below 0.7. */
static const uint32_t noise_gain[11] = {0, 5120, 7296, 10240, 14592, 20480, 28928, 40960, 57856, 81664, 115328};

static struct {
    uint16_t beat, carrier;          /* 0.1 Hz, Hz */
    uint8_t preset, timer, noise;    /* noise: 0 off, 1..10 pink, 11..20 rain */
    uint8_t mode, width, feed, mix;  /* B1: 0 off / 1 crossfeed; the rest 0..100 */
    uint8_t rain;                    /* the noise type kept while the level is 0 */
} cfg = {100, 250, 0, 0, 0, 0, 50, 60, 80, 0};

static volatile uint32_t tgt_inc_l, tgt_beat_inc, tgt_noise, req_blocks;
static volatile uint8_t req_on, req_gen, tgt_tone, tgt_rain, b1_on;
static volatile float tgt_width = 1.0f, tgt_feed = 0.21f, tgt_mix = 0.8f;
/* audio side, read by the control task for the screen */
static volatile uint32_t blocks_left;
static volatile uint8_t ended, gen_seen;

static uint32_t inc_of(uint32_t f10) /* phase step of a frequency in 0.1 Hz: f * 2^32 / 44100 */
{
    return (uint32_t)(((uint64_t)f10 << 32) / 441000ULL);
}
static void retarget(void)
{
    tgt_inc_l = inc_of(cfg.carrier * 10U);
    tgt_beat_inc = inc_of(cfg.beat); /* right = left + beat: the difference is exact */
    tgt_noise = noise_gain[cfg.noise ? (cfg.noise - 1U) % 10U + 1U : 0U];
    tgt_rain = cfg.noise > 10U;
    tgt_tone = cfg.preset != 0;
    tgt_width = (float)cfg.width * 0.02f; /* 50 = as it is, 0 = mono, 100 = side x2 */
    tgt_feed = (float)cfg.feed * 0.0035f; /* 100 -> 0.35 of the other ear */
    tgt_mix = (float)cfg.mix * 0.01f;
    b1_on = cfg.mode != 0;
}
static void restart_timer(void)
{
    req_blocks = (uint32_t)timer_min[cfg.timer] * BLOCKS_PER_MIN;
    req_gen = (uint8_t)(req_gen + 1U);
}
unsigned binaural_running(void) { return req_on && (!ended || req_gen != gen_seen); } /* a restart not yet seen counts */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
void binaural_edit(unsigned p, int d)
{
    if (!d) return;
    int s = d > 0 ? 1 : -1;
    switch (p) {
    case BIN_TIMER: cfg.timer = (uint8_t)clampi(cfg.timer + s, 0, TIMERS - 1); break;
    case BIN_PRESET: {
        int n = cfg.preset;
        if (n == BIN_CUSTOM) { /* right: the next preset with a bigger beat; left: smaller, else OFF */
            int pick = s > 0 ? BIN_PRESETS : 0;
            if (s > 0) { for (int i = BIN_PRESETS - 1; i >= 0; --i) if (preset_beat[i] > cfg.beat) pick = i + 1; }
            else for (int i = 0; i < BIN_PRESETS; ++i) if (preset_beat[i] < cfg.beat) pick = i + 1;
            n = pick;
        } else n = clampi(n + s, 0, BIN_PRESETS);
        cfg.preset = (uint8_t)n;
        if (n) cfg.beat = preset_beat[n - 1];
        break;
    }
    case BIN_BEAT:
        cfg.beat = (uint16_t)clampi((int)cfg.beat + s * 5, BEAT_MIN, BEAT_MAX);
        if (cfg.preset) cfg.preset = BIN_CUSTOM; /* a tuned preset */
        break;
    case BIN_CARRIER: cfg.carrier = (uint16_t)clampi((int)cfg.carrier + s * 5, CARRIER_MIN, CARRIER_MAX); break;
    case BIN_NOISE: cfg.noise = (uint8_t)clampi(cfg.noise + s, 0, NOISE_MAX); if (cfg.noise) cfg.rain = cfg.noise > 10U; break;
    case BIN_NLEVEL: case BIN_NTYPE: {
        int level = cfg.noise ? (cfg.noise - 1) % 10 + 1 : 0;
        if (p == BIN_NLEVEL) level = clampi(level + s, 0, 10); else cfg.rain = s > 0;
        cfg.noise = (uint8_t)(level ? level + (cfg.rain ? 10 : 0) : 0);
        break;
    }
    case BIN_MODE: cfg.mode = d > 0; break;
    case BIN_WIDTH: cfg.width = (uint8_t)clampi(cfg.width + s * 5, 0, 100); break;
    case BIN_FEED: cfg.feed = (uint8_t)clampi(cfg.feed + s * 5, 0, 100); break;
    case BIN_MIX: cfg.mix = (uint8_t)clampi(cfg.mix + s * 5, 0, 100); break;
    default: return;
    }
    retarget();
    if (p >= BIN_MODE && p <= BIN_MIX) return; /* B1 knobs never start or stop the tones */
    unsigned want = cfg.preset != 0 || cfg.noise != 0;
    /* a new session (also after the timer ended) or a new timer: count again */
    if (want && (p == BIN_TIMER || !binaural_running())) restart_timer();
    req_on = (uint8_t)want;
}
const char *binaural_label(unsigned p)
{
    static const char *const l[BIN_PARAMS] = {"TIMER", "PRESET", "BEAT", "CARRIER", "NOISE", "MODE", "WIDTH", "FEED", "MIX", "NOISE", "NOISE"};
    return p < BIN_PARAMS ? l[p] : "";
}
static char *put_num(char *o, unsigned v, unsigned min_digits)
{
    char t[10]; unsigned n = 0;
    do { t[n++] = (char)('0' + v % 10U); v /= 10U; } while (v || n < min_digits);
    while (n) *o++ = t[--n];
    return o;
}
static char *put_str(char *o, const char *s) { while (*s) *o++ = *s++; return o; }
unsigned binaural_remaining(void)
{
    if (!cfg.timer) return 0;
    if (!binaural_running() || req_gen != gen_seen) return (unsigned)timer_min[cfg.timer] * 60U; /* not counting yet */
    return (unsigned)(((uint64_t)blocks_left * 128U + 44099U) / 44100U);
}
void binaural_text(unsigned p, char out[12])
{
    char *o = out;
    switch (p) {
    case BIN_TIMER:
        if (!cfg.timer) o = put_str(o, "OFF"); /* no timer */
        else { unsigned s = binaural_remaining(); o = put_num(o, s / 60U, 2); *o++ = ':'; o = put_num(o, s % 60U, 2); }
        break;
    case BIN_PRESET: o = put_str(o, req_on && !binaural_running() ? "END" : preset_name[cfg.preset]); break;
    case BIN_BEAT: o = put_num(o, cfg.beat / 10U, 1); *o++ = '.'; o = put_num(o, cfg.beat % 10U, 1); break;
    case BIN_CARRIER: o = put_num(o, cfg.carrier, 1); break;
    case BIN_NOISE:
        if (!cfg.noise) o = put_str(o, "OFF");
        else { o = put_str(o, cfg.noise > 10U ? "RAIN " : "PINK "); o = put_num(o, (cfg.noise - 1U) % 10U + 1U, 1); }
        break;
    case BIN_MODE: o = put_str(o, cfg.mode ? "XFEED" : "OFF"); break;
    case BIN_WIDTH: o = put_num(o, cfg.width, 1); break;
    case BIN_FEED: o = put_num(o, cfg.feed, 1); break;
    case BIN_MIX: o = put_num(o, cfg.mix, 1); break;
    }
    *o = 0;
}
unsigned binaural_beat10(void) { return cfg.beat; }
unsigned binaural_carrier(void) { return cfg.carrier; }
unsigned binaural_xfeed(void) { return cfg.mode; }
unsigned binaural_feed(void) { return cfg.feed; }
unsigned binaural_width(void) { return cfg.width; }
unsigned binaural_mix(void) { return cfg.mix; }
unsigned binaural_noise(void) { return cfg.noise; }
unsigned binaural_preset(void) { return cfg.preset; }
unsigned binaural_timer_min(void) { return timer_min[cfg.timer]; }

/* Saved: beat, carrier, timer, B1 mode / width / feed / mix; bytes 9..11
 * spare (0xff). Not saved: the preset and the noise, so loading a project
 * never starts a sound. Each field falls back to its default on its own. */
void binaural_state_get(uint8_t out[BIN_STATE_BYTES])
{
    out[0] = (uint8_t)cfg.beat; out[1] = (uint8_t)(cfg.beat >> 8);
    out[2] = (uint8_t)cfg.carrier; out[3] = (uint8_t)(cfg.carrier >> 8);
    out[4] = cfg.timer; out[5] = cfg.mode; out[6] = cfg.width; out[7] = cfg.feed; out[8] = cfg.mix;
    out[9] = out[10] = out[11] = 0xff;
}
void binaural_state_put(const uint8_t in[BIN_STATE_BYTES])
{
    unsigned beat = in[0] | (unsigned)in[1] << 8, carrier = in[2] | (unsigned)in[3] << 8;
    cfg.beat = beat >= BEAT_MIN && beat <= BEAT_MAX ? (uint16_t)beat : 100;
    cfg.carrier = carrier >= CARRIER_MIN && carrier <= CARRIER_MAX ? (uint16_t)carrier : 250;
    cfg.timer = in[4] < TIMERS ? in[4] : 0;
    cfg.mode = in[5] <= 1 ? in[5] : 0;
    cfg.width = in[6] <= 100 ? in[6] : 50;
    cfg.feed = in[7] <= 100 ? in[7] : 60;
    cfg.mix = in[8] <= 100 ? in[8] : 80;
    if (cfg.preset && (cfg.preset == BIN_CUSTOM || preset_beat[cfg.preset - 1] != cfg.beat)) cfg.preset = BIN_CUSTOM;
    retarget();
}

/* ---- audio ---- */
static uint32_t ph_l, ph_r, inc_l, inc_r, g_tone, g_noise, rng_l = 0x12345679U, rng_r = 0x9e3779b9U, rain_count;
static int32_t lp1_l, lp1_r, lp2_l, lp2_r, rain_env = 1024, rain_goal = 1024;
static float w_cur = 1.0f, f_cur, m_cur, lp_l, lp_r, ring_l[16], ring_r[16];
static unsigned ring_at;

static inline int32_t sine15(uint32_t ph) /* parabola with one refinement, error < 0.1 %; peak 32768 */
{
    int32_t a = (int32_t)ph >> 16, ab = a < 0 ? -a : a;
    int32_t y = (a * (32768 - ab)) >> 13, ya = y < 0 ? -y : y;
    return y + (((((y * ya) >> 15) - y) * 7373) >> 15);
}
static inline uint32_t xorshift(uint32_t x) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
static inline uint32_t slew(uint32_t g, uint32_t target) /* both multiples of 128 */
{
    return g < target ? (target - g < STEP ? target : g + STEP) : (g - target < STEP ? target : g - STEP);
}
static inline uint32_t glide(uint32_t cur, uint32_t target) /* 1/16 of the way per block; the last 0.02 Hz at once */
{
    int32_t diff = (int32_t)(target - cur);
    return diff > 2047 || diff < -2047 ? cur + (uint32_t)(diff >> 4) : target;
}

/* B1: width (mid/side), then crossfeed: the other ear's signal low-passed
 * (one pole, about 700 Hz) and delayed 14 samples (0.32 ms) is mixed in, the
 * level kept by 1 / (1 + feed). MIX blends it with the dry signal. Float, like
 * the master effects; every parameter is interpolated across the block. */
static FM1_AUDIO_CODE void run_b1(int32_t *out)
{
    float m_goal = b1_on ? tgt_mix : 0.0f;
    float w_next = w_cur + (tgt_width - w_cur) * 0.125f, f_next = f_cur + (tgt_feed - f_cur) * 0.125f;
    float m_next = m_cur + (m_goal - m_cur) * 0.125f;
    if (!b1_on && m_next < 0.0005f) m_next = 0.0f; /* faded out: B1 stops here */
    const float k = 1.0f / FM1_BLOCK_FRAMES;
    float d0 = 1.0f / (1.0f + f_cur), d1 = 1.0f / (1.0f + f_next);
    float dw = (w_next - w_cur) * k, df = (f_next - f_cur) * k, dm = (m_next - m_cur) * k, dd = (d1 - d0) * k;
    float w = w_cur, f = f_cur, m = m_cur, dir = d0, ll = lp_l, lr = lp_r;
    unsigned at = ring_at;
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        float l = (float)out[2 * i], r = (float)out[2 * i + 1];
        float mid = (l + r) * 0.5f, side = (l - r) * 0.5f * w;
        float wl = mid + side, wr = mid - side;
        ll += 0.095f * (wl - ll); lr += 0.095f * (wr - lr);
        float xl = ring_r[(at + 2U) & 15U], xr = ring_l[(at + 2U) & 15U]; /* written 14 frames ago */
        ring_l[at] = ll; ring_r[at] = lr; at = (at + 1U) & 15U;
        float ol = (wl + xl * f) * dir, or_ = (wr + xr * f) * dir;
        out[2 * i] = (int32_t)(l + (ol - l) * m);
        out[2 * i + 1] = (int32_t)(r + (or_ - r) * m);
        w += dw; f += df; m += dm; dir += dd;
    }
    w_cur = w_next; f_cur = f_next; m_cur = m_next; lp_l = ll; lp_r = lr; ring_at = at;
}
/* A: two sines (left carrier, right carrier + beat, at most 0.25 full scale)
 * and the noise bed, added to the block. Gains and phase steps move once per
 * block and are ramped linearly across it (gains exactly: multiples of 128). */
static FM1_AUDIO_CODE void run_tone(int32_t *out, unsigned active)
{
    unsigned fade = 256U; /* Q8: the last 10 s of a timer fade out */
    if (active && req_blocks && blocks_left < END_BLOCKS) fade = blocks_left * 256U / END_BLOCKS;
    uint32_t tone_to = active && tgt_tone ? (FULL * fade >> 8) & ~127U : 0U;
    /* the rain swells and calms: a new loudness (0.55..1) every 64 blocks, followed smoothly */
    if (!(rain_count++ & 63U)) { rng_r = xorshift(rng_r); rain_goal = 563 + (int32_t)(rng_r >> 24) * 461 / 255; }
    rain_env += (rain_goal - rain_env) >> 3;
    uint32_t noise_to = active ? (uint32_t)(((uint64_t)tgt_noise * (tgt_rain ? (uint32_t)rain_env : 1024U) * fade) >> 18) & ~127U : 0U;
    uint32_t t_next = slew(g_tone, tone_to), n_next = slew(g_noise, noise_to);
    uint32_t il_next = glide(inc_l, tgt_inc_l), ir_next = glide(inc_r, tgt_inc_l + tgt_beat_inc);
    int32_t dt = ((int32_t)t_next - (int32_t)g_tone) / FM1_BLOCK_FRAMES, dn = ((int32_t)n_next - (int32_t)g_noise) / FM1_BLOCK_FRAMES;
    int32_t dil = (int32_t)(il_next - inc_l) >> 7, dir_ = (int32_t)(ir_next - inc_r) >> 7;
    uint32_t pl = ph_l, pr = ph_r, il = inc_l, ir = inc_r, gt = g_tone, gn = g_noise, rl = rng_l, rr = rng_r;
    int32_t a1 = lp1_l, a2 = lp2_l, b1 = lp1_r, b2 = lp2_r;
    unsigned rain = tgt_rain;
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        int32_t g4 = (int32_t)(gt >> 4); /* <= 2^14; sine * g4 < 2^29 */
        int32_t tl = (sine15(pl) * g4) >> 8, tr = (sine15(pr) * g4) >> 8; /* FULL -> 2^21 = 0.25 full scale */
        rl = xorshift(rl); rr = xorshift(rr);
        int32_t wl = (int32_t)rl >> 9, wr = (int32_t)rr >> 9, nl, nr; /* white, +-2^22 */
        if (rain) { /* rain: band-passed hiss (about 440 Hz .. 7 kHz) */
            a1 += (wl - a1) >> 1; a2 += (wl - a2) >> 4; b1 += (wr - b1) >> 1; b2 += (wr - b2) >> 4;
            nl = a1 - a2; nr = b1 - b2;
        } else { /* pink-ish: two poles (880 Hz, 55 Hz) and a little white */
            a1 += (wl - a1) >> 3; a2 += (wl - a2) >> 7; b1 += (wr - b1) >> 3; b2 += (wr - b2) >> 7;
            nl = a1 + 3 * a2 + (wl >> 3); nr = b1 + 3 * b2 + (wr >> 3);
        }
        nl = nl > 8388607 ? 8388607 : nl < -8388608 ? -8388608 : nl;
        nr = nr > 8388607 ? 8388607 : nr < -8388608 ? -8388608 : nr;
        int32_t g6 = (int32_t)(gn >> 6); /* < 2^11; (noise >> 4) * g6 < 2^30 */
        out[2 * i] += tl + (((nl >> 4) * g6) >> 8);
        out[2 * i + 1] += tr + (((nr >> 4) * g6) >> 8);
        pl += il; pr += ir; il += (uint32_t)dil; ir += (uint32_t)dir_; gt += (uint32_t)dt; gn += (uint32_t)dn;
    }
    /* the gain ramps land exactly; a phase step may be off by < 128 units (0.001 Hz) */
    ph_l = pl; ph_r = pr; inc_l = il_next; inc_r = ir_next; g_tone = t_next; g_noise = n_next;
    lp1_l = a1; lp2_l = a2; lp1_r = b1; lp2_r = b2; rng_l = rl; rng_r = rr;
}
/* Soft limiter (effects audit 10): BINAURAL works after the master limiter,
 * so tones + noise + WIDTH on a loud mix could reach the hard clip in
 * fm1_output_mix. Below KNEE (0.8 full scale) every sample passes unchanged;
 * above it y = KNEE + d / (1 + d / ROOM) bends smoothly towards full scale and
 * never reaches it. One compare per sample; the division only above the knee.
 * Runs only while BINAURAL adds or changes sound. */
enum { KNEE = 6710886 /* 0.8 FS */, ROOM = 8388607 - KNEE };
static FM1_AUDIO_CODE void soft_limit(int32_t *b)
{
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
        int32_t v = b[i];
        if (v <= KNEE && v >= -KNEE) continue;
        /* |v| - KNEE fits int32 even for INT32_MIN; an int64 here would
         * convert through __floatdisf, library code in Flash. */
        uint32_t a = v < 0 ? 0U - (uint32_t)v : (uint32_t)v;
        float d = (float)(int32_t)(a - (uint32_t)KNEE);
        int32_t y = KNEE + (int32_t)(d / (1.0f + d * (1.0f / (float)ROOM)));
        if (y > 8388607 - 1) y = 8388607 - 1;
        b[i] = v < 0 ? -y : y;
    }
}
FM1_AUDIO_CODE void binaural_render(int32_t *buffer)
{
    unsigned changed = b1_on || m_cur != 0.0f;
    if (changed) run_b1(buffer); /* after OFF it fades out, then costs nothing */
    if (req_gen != gen_seen) { gen_seen = req_gen; blocks_left = req_blocks; ended = 0; }
    unsigned active = req_on && !ended;
    if (!active && !g_tone && !g_noise) { /* idle: start the next session at the right pitch */
        inc_l = tgt_inc_l; inc_r = tgt_inc_l + tgt_beat_inc;
        if (changed) soft_limit(buffer);
        return;
    }
    if (active && req_blocks) {
        if (blocks_left) --blocks_left;
        if (!blocks_left) { ended = 1; active = 0; }
    }
    run_tone(buffer, active);
    soft_limit(buffer);
}
#endif
