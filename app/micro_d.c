/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* MICRO D paraphonic virtual analog. Per track: up to two oscillators (one
 * per note, user 10.11: at most 2 voices, always; SAW, SHARK, SQUARE->PULSE and TRI mixed by level, polyBLEP, an
 * optional detuned second layer), noise, one shared 24 dB ladder with drive,
 * one ADS envelope for filter and VCA, one LFO. Notes come from the closed
 * sequencer/arpeggiator in the audio ISR (micro_d_note) or from the keys
 * through a small queue (micro_d_live). Controls are bytes written by the
 * control task; the DSP smooths them per 16-frame sub-block. */
#include "micro_d.h"
#include "build_config.h"
#include "instrument.h"
#include <string.h>
#ifndef FM1_HOST_TEST
#include "fm1_device.h"
#endif

#ifdef FM1_HOST_TEST
/* Test-only counts read by tests/t_micro_d.py: ftanh calls, float divisions on
 * the per-sample path (ftanh, blep), inner ladder saturations, and blocks that
 * ran the full render. */
unsigned md_test_tanh, md_test_div, md_test_sat, md_test_busy;
#define MD_COUNT(c) (++(c))
#else
#define MD_COUNT(c) ((void)0)
#endif

#define FS 44100.0f
#define PI_F 3.14159265358979f
/* VOICES: at most 2 per track, always (user 10.11, stable sound: "cut the voices to 2"); MODE 3 / 4 play 2. */
enum { SUB = 16, VOICES = 2, QUEUE = 32, STACK = 8 };

struct voice {
    float ph[2], pitch, target, amp, level;
    uint8_t note, gate, on;
    uint32_t age;
};
struct slot {
    volatile uint8_t track, volume, preset, audible;
    volatile uint8_t p[MD_PARAMS];
    struct voice v[VOICES];
    uint8_t stack[STACK], depth, mode, gated, attack;
    float env, gate_amp, mute_gain, y[4], g_prev, lfo_ph, lfo_val, sh, pink[3], dc_x, dc_y, last_pitch, meter;
    uint32_t clock, noise;
};
static struct slot slots[MD_SLOTS];
static volatile uint8_t owner[GROOVE_TRACKS]; /* slot + 1, 0 = not MICRO D */
static volatile uint32_t q_write, q_read;
static volatile uint32_t queue[QUEUE];
/* Notes from the engine (sequencer / arpeggiator): queued by micro_d_note,
 * played by micro_d_render (both RAM code, like every audio callee). The
 * engine renders on two cores, odd tracks (0-based even) on core 0 and the
 * others on core 1, and calls micro_d_note from both at the same time: one
 * lane per core (track & 1, as midi_router.c), so each lane has one writer
 * and no write index is shared. Core 0's audio ISR reads both lanes. */
enum { LANES = 2 };
static volatile uint32_t iq_write[LANES], iq_read[LANES];
static volatile uint32_t iqueue[LANES][QUEUE];
#ifdef FM1_HOST_TEST
void (*md_test_note_mid)(void); /* host test: runs once inside micro_d_note, between reading and moving the write index */
#endif
static inline __attribute__((always_inline)) void lane_barrier(void)
{
#ifndef FM1_HOST_TEST
    __asm__ volatile("csync" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}
static uint8_t inited;

/* ---- presets: name, then the 20 controls in enum order ---- */
struct preset { char name[10]; uint8_t p[MD_PARAMS]; };
static const struct preset presets[] = {
    /*            SAW SHK SQR TRI DET NOI GLD VCA  CUT RES CON DRV  ATK DEC SUS RTG  RAT WAV AMT MOD */
    {"MICRO D",   {118,  0, 72, 40, 14, 64,  0,  0,  44, 42, 94, 62,   0, 58, 46,  1,  60,  0, 63,  3}},
    {"FAT LEAD",  {100,  0, 50,  0, 30, 64, 20,  0,  70, 35, 84, 50,   4, 70, 90,  0,  80,  0, 56,  0}},
    {"SQUELCH",   { 90,  0,  0,  0,  0, 64,  0,  0,  30, 92,104, 70,   0, 40, 10,  1,  60,  0, 63,  0}},
    {"SOFT PAD",  { 70,  0, 40, 60, 24, 58,  0,  0,  62, 20, 76, 10,  80, 90,100,  0,  30,  0, 72,  3}},
    {"BRASS D",   {120,  0,  0,  0, 16, 64,  0,  0,  48, 20,100, 30,  30, 60, 80,  1,  60,  0, 63,  2}},
    {"PLUCK D",   { 80, 40,  0,  0,  0, 64,  0,  0,  36, 50,110, 20,   0, 38,  0,  1,  60,  0, 63,  2}},
    {"SUB BASS",  {  0,  0,  0,127,  0, 64,  0,  0,  50, 10, 64, 60,   0, 60,100,  0,  60,  0, 63,  0}},
    {"WHISTLE",   {  0,  0,  0, 90,  0, 64, 30,  0,  70,118, 70,  0,  20, 60,110,  0,  70,  0, 52,  0}},
    {"NOISE SWP", {  0,  0,  0,  0,  0,127,  0,  0,  20,100,120,  0,  60,100, 40,  1,  20,  1, 90,  0}},
    {"UNISON LD", {110,  0, 30,  0, 40, 64, 10,  0,  72, 30, 80, 60,   2, 70,100,  0,  80,  0, 60,  4}},
    {"PARA KEYS", { 60, 50,  0, 40,  0, 64,  0,  0,  56, 25, 90, 15,   2, 64, 60,  1,  60,  0, 63,  3}},
    {"ACID D",    {100,  0,  0,  0,  0, 64,  0,  0,  26, 96,112, 90,   0, 34,  0,  1,  60,  0, 63,  0}},
    {"SHARK LD",  {  0,120,  0,  0, 20, 64, 16,  0,  80, 40, 76, 40,   0, 70,100,  0,  84,  0, 54,  0}},
    {"PWM PAD",   {  0,  0,100, 30, 30, 64,  0,  0,  66, 15, 70,  0,  90, 90,110,  0,  24,  1, 70,  3}},
    {"PERC BLIP", {  0,  0, 90,  0,  0, 70,  0,  1,  60, 70,127, 20,   0, 24,  0,  1,  60,  0, 63,  1}},
    {"DRONE",     { 80, 60,  0, 60, 60, 60, 40,  0,  40, 60, 64, 50, 100,127,127,  0,  10,  0, 90,  4}},
};
enum { PRESETS = sizeof presets / sizeof presets[0] };
static const uint8_t maxes[MD_PARAMS] = {127,127,127,127, 127,127,127,1, 127,127,127,127, 127,127,127,1, 127,3,126,4};
/* Plain words: what the knob does, not the circuit's name. */
static const char *const labels[MD_PARAMS] = {
    "SAW", "SHARK", "SQUARE", "TRI", "THICK", "AIR", "SLIDE", "KEY",
    "BRIGHT", "PEAK", "MOVE", "DIRT", "START", "FALL", "HOLD", "RESTART",
    "SPEED", "SHAPE", "DEPTH", "NOTES"};
static const char *const waves[4] = {"SINE", "TRI", "SQR", "S&H"};
static const char *const modes[5] = {"1", "2", "2", "2", "ALL 1"}; /* MODE 3 / 4 (old projects, presets) play 2 voices */

/* ---- small math (same approximations as master_fx.c) ---- */
static inline __attribute__((always_inline)) float fexp2(float x)
{
    if (x < -60) x = -60; if (x > 60) x = 60;
    int n = (int)(x + (x < 0 ? -0.5f : 0.5f));
    float a = (x - (float)n) * 0.6931472f;
    float p = 1 + a * (1 + a * (0.5f + a * (0.1666667f + a * (0.0416667f + a * 0.0083333f))));
    union { uint32_t u; float f; } s = {(uint32_t)(n + 127) << 23};
    return p * s.f;
}
/* sqrt for 0..1 without libm: two Newton steps from a linear guess. */
static inline __attribute__((always_inline)) float fsqrt_approx(float x)
{
    if (x <= 0.0f) return 0.0f;
    float r = 0.5f + 0.5f * x;
    r = 0.5f * (r + x / r); r = 0.5f * (r + x / r); r = 0.5f * (r + x / r);
    return r;
}
static inline __attribute__((always_inline)) float ftanh(float x)
{
    MD_COUNT(md_test_tanh);
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    MD_COUNT(md_test_div);
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
/* The inner ladder stages: the same curve as ftanh, x (27 + x^2) / (27 + 9 x^2)
 * held at +-1 beyond |x| = 3, with the division replaced by a reciprocal of the
 * denominator: a quadratic first guess in x^2, then two Newton steps
 * r = r (2 - d r). Odd, bounded to +-1, monotonic up to float rounding (the
 * same 2e-7 wiggles near |x| = 3 as ftanh itself). Largest difference from
 * ftanh over -3..3 in float: 3.0e-5 (near |x| = 2.6), 2.8e-5 for |x| <= 1.5. */
static inline __attribute__((always_inline)) float fsat(float x)
{
    MD_COUNT(md_test_sat);
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float s = x * x, d = 27.0f + 9.0f * s;
    float r = 0.034298395f + s * (-0.00595329861f + s * 0.000360804863f);
    r *= 2.0f - d * r;
    r *= 2.0f - d * r;
    return x * (27.0f + s) * r;
}
static inline __attribute__((always_inline)) float fsin_cycles(float c)
{
    c -= (float)(int)c; if (c < 0) c += 1;
    float a = (c > 0.5f ? c - 1.0f : c) * 2 * PI_F;
    if (a > PI_F * 0.5f) a = PI_F - a; else if (a < -PI_F * 0.5f) a = -PI_F - a;
    float q = a * a;
    return a * (1 + q * (-0.1666667f + q * (0.0083333f + q * (-0.0001984f + q * 0.0000028f))));
}
static inline __attribute__((always_inline)) float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static inline __attribute__((always_inline)) float blep(float t, float dt)
{
    if (t < dt) { MD_COUNT(md_test_div); t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { MD_COUNT(md_test_div); t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}
/* Time for a 0..127 control: lo * 2^(v/127 * oct) seconds. */
static inline __attribute__((always_inline)) float ctl_time(unsigned v, float lo, float oct) { return lo * fexp2((float)v * (1.0f / 127) * oct); }
/* Per-sample coefficient of a one-pole that covers ~99% in t seconds. */
static inline __attribute__((always_inline)) float coef(float t) { float c = 4.6f / (t * FS); return c > 1.0f ? 1.0f : c; }

static void init_once(void)
{
    if (inited) return;
    inited = 1;
    for (unsigned s = 0; s < MD_SLOTS; ++s) {
        slots[s].track = 0xff; slots[s].volume = 100; slots[s].preset = 0; slots[s].audible = 1;
        memcpy((void *)slots[s].p, presets[0].p, MD_PARAMS);
        slots[s].noise = 0x2545f491U + s;
    }
}
static struct slot *slot_of(unsigned track)
{
    if (track >= GROOVE_TRACKS) return 0;
    unsigned o = owner[track]; /* read once */
    return o ? &slots[o - 1U] : 0;
}

/* ---- control task ---- */
int micro_d_track(unsigned track) { return track < GROOVE_TRACKS && owner[track] != 0; }
unsigned micro_d_count(void)
{
    unsigned n = 0;
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) n += owner[t] != 0;
    return n;
}
static void host_volume(unsigned track, unsigned target)
{
    struct groove_view v; memset(&v, 0, sizeof v);
    groove_get_view(track, 0, &v);
    int d = (int)target - (int)v.volume;
    if (d) groove_control_edit(track, 0, d, 0);
}
int micro_d_attach(unsigned track)
{
    if (!FM1_MICRO_D) return -1;
    init_once();
    if (track >= GROOVE_TRACKS) return -1;
    if (owner[track]) return 0;
    unsigned s = 0;
    while (s < MD_SLOTS && slots[s].track != 0xff) ++s;
    if (s == MD_SLOTS) return -1;
    struct groove_view v; memset(&v, 0, sizeof v);
    groove_get_view(track, 0, &v);
    if (v.type != GROOVE_PSG && groove_source_set(track, GROOVE_PSG)) return -1;
    groove_get_view(track, 0, &v);
    struct slot *d = &slots[s];
    memset(d->v, 0, sizeof d->v); d->depth = 0; d->gated = 0; d->env = 0; d->gate_amp = 0;
    memset(d->y, 0, sizeof d->y); d->g_prev = -1.0f;
    d->volume = v.volume ? v.volume : 100;
    d->track = (uint8_t)track;
    host_volume(track, 0); /* the PSG host stays silent */
    owner[track] = (uint8_t)(s + 1U);
    return 0;
}
void micro_d_detach(unsigned track)
{
    struct slot *d = slot_of(track);
    if (!d) return;
    owner[track] = 0;
    d->track = 0xff;
    host_volume(track, d->volume);
}
/* The host must stay silent, but a controller that still reaches it (CC 7 on a channel shared with another
 * track, a path inside the closed instrument) makes its PSG voice audible: attach silences it only once.
 * Control task (groove_project_poll): every MD_GUARD_MS the host of each MICRO D track whose volume is not 0
 * is put back to 0. Only a PSG host is touched, never another engine a stale owner may point at.
 * When the track's input channel reaches MICRO D tracks only, the first try is a plain CC 7 = 0 posted to the
 * instrument: a transient controller, so no undo point, the project is not marked changed and the engine is
 * not paused. If the volume is still not 0 at the next check, or the channel is shared with another track
 * (the CC would silence that one too), host_volume does it through groove_control_edit, which for a NORMAL
 * track takes an undo point and marks the project changed, like a volume turn on the mixer. */
enum { MD_GUARD_MS = 250 };
void micro_d_host_guard(uint32_t now_ms)
{
    static uint32_t checked;
    static uint8_t soft; /* tracks whose last try was the plain CC */
    if (now_ms - checked < MD_GUARD_MS) return;
    checked = now_ms;
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) {
        uint8_t bit = (uint8_t)(1U << t);
        struct groove_view v; memset(&v, 0, sizeof v);
        if (owner[t]) groove_get_view(t, 0, &v);
        if (!owner[t] || !v.volume || v.type != GROOVE_PSG || groove_track_role(t) == GROOVE_TRACK_OFF) {
            soft &= (uint8_t)~bit;
            continue;
        }
        unsigned ch = groove_midi_input_channel(t), shared = 0;
        for (unsigned u = 0; u < GROOVE_TRACKS; ++u)
            shared |= !owner[u] && groove_track_role(u) != GROOVE_TRACK_OFF && groove_midi_input_channel(u) == ch;
        if (!shared && !(soft & bit)) {
            const uint8_t m[3] = {(uint8_t)(0xb0U | ch), 7, 0};
            if (!fm1_midi_post(m, 3)) { soft |= bit; continue; }
        }
        soft &= (uint8_t)~bit;
        host_volume(t, 0);
    }
}
unsigned micro_d_value(unsigned track, unsigned param)
{
    struct slot *d = slot_of(track);
    return d && param < MD_PARAMS ? d->p[param] : 0;
}
unsigned micro_d_max(unsigned param) { return param < MD_PARAMS ? maxes[param] : 0; }
void micro_d_edit(unsigned track, unsigned param, int delta)
{
    struct slot *d = slot_of(track);
    if (!d || param >= MD_PARAMS || !delta) return;
    int v = (int)d->p[param] + delta, m = maxes[param];
    d->p[param] = (uint8_t)(v < 0 ? 0 : v > m ? m : v);
}
const char *micro_d_label(unsigned param) { return param < MD_PARAMS ? labels[param] : ""; }
static void put_num(char *o, int v)
{
    char t[8]; unsigned n = 0, neg = v < 0; if (neg) v = -v;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 7);
    if (neg) *o++ = '-';
    while (n) *o++ = t[--n];
    *o = 0;
}
static void put_time(char *o, float s) { put_num(o, (int)(s * 1000.0f + 0.5f)); strcat(o, "MS"); } /* always milliseconds */
void micro_d_text(unsigned track, unsigned param, char out[12])
{
    unsigned v = micro_d_value(track, param);
    out[0] = 0;
    switch (param) {
    case MD_NOISE:
        if (v == 64) strcpy(out, "OFF");
        else { strcpy(out, v < 64 ? "PINK " : "WHITE "); put_num(out + strlen(out), v < 64 ? 64 - (int)v : (int)v - 64); }
        return;
    case MD_VCA: strcpy(out, v ? "ON" : "SHAPE"); return; /* sound follows the shape, or just the key */
    case MD_RETRIG: strcpy(out, v ? "ON" : "OFF"); return;
    case MD_WAVE: strcpy(out, waves[v & 3U]); return;
    case MD_MODE: strcpy(out, modes[v > 4 ? 4 : v]); return;
    case MD_CONTOUR: { int c = (int)v - 64; if (c > 0) { out[0] = '+'; put_num(out + 1, c); } else put_num(out, c); return; }
    case MD_AMOUNT: { int c = (int)v - 63; if (!c) strcpy(out, "OFF"); else { strcpy(out, c > 0 ? "BRIGHT " : "PITCH "); put_num(out + strlen(out), c > 0 ? c : -c); } return; }
    case MD_CUTOFF: {
        int hz = (int)clampf(30.0f * fexp2((float)v * (1.0f / 127) * 8.7f), 20.0f, 12000.0f);
        if (hz < 1000) put_num(out, hz);
        else { put_num(out, hz / 1000); strcat(out, "."); put_num(out + strlen(out), hz % 1000 / 100); strcat(out, "K"); }
        return;
    }
    case MD_ATTACK: put_time(out, v ? ctl_time(v, 0.001f, 12.3f) : 0.001f); return;
    case MD_DECAY: put_time(out, ctl_time(v, 0.005f, 10.0f)); return;
    case MD_GLIDE: if (!v) strcpy(out, "OFF"); else put_time(out, ctl_time(v, 0.005f, 9.0f)); return;
    case MD_RATE: {
        int c = (int)(0.05f * fexp2((float)v * (1.0f / 127) * 8.64f) * 10.0f + 0.5f);
        put_num(out, c / 10); strcat(out, "."); put_num(out + strlen(out), c % 10); strcat(out, "HZ");
        return;
    }
    default: put_num(out, (int)v); return;
    }
}
/* Mixer pan, smooth (user 10.11), by the instrument's own law for CC 10: 0..127, at 64 both channels
 * are full; towards a side the other channel fades out in a straight line. Per track, so it holds
 * whichever slot the track gets. */
_Static_assert(GROOVE_TRACKS == 8, "track_pan_gain initialiser");
static volatile float track_pan_gain[GROOVE_TRACKS][2] = {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}};
void micro_d_pan(unsigned track, unsigned value)
{
    if (track >= GROOVE_TRACKS) return;
    if (value > 127U) value = 64U;
    track_pan_gain[track][0] = value <= 64U ? 1.0f : (float)(127U - value) * (1.0f / 63.0f);
    track_pan_gain[track][1] = value >= 64U ? 1.0f : (float)value * (1.0f / 64.0f);
}
void micro_d_audible(unsigned track, unsigned audible)
{
    struct slot *d = slot_of(track);
    if (d) d->audible = audible != 0;
}
unsigned micro_d_volume(unsigned track) { struct slot *d = slot_of(track); return d ? d->volume : 0; }
void micro_d_volume_edit(unsigned track, int delta)
{
    struct slot *d = slot_of(track);
    if (!d) return;
    int v = (int)d->volume + delta;
    d->volume = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
}
unsigned micro_d_preset(unsigned track) { struct slot *d = slot_of(track); return d ? d->preset : 0; }
unsigned micro_d_preset_count(void) { return PRESETS; }
const char *micro_d_preset_name(unsigned preset) { return preset < PRESETS ? presets[preset].name : ""; }
void micro_d_preset_load(unsigned track, unsigned preset)
{
    struct slot *d = slot_of(track);
    if (!d || preset >= PRESETS) return;
    d->preset = (uint8_t)preset;
    memcpy((void *)d->p, presets[preset].p, MD_PARAMS);
}
unsigned micro_d_voices(unsigned track, uint8_t notes[4])
{
    struct slot *d = slot_of(track);
    unsigned mask = 0;
    if (!d) return 0;
    for (unsigned i = 0; i < VOICES; ++i) {
        notes[i] = d->v[i].note;
        if (d->v[i].on && (d->v[i].gate || d->v[i].level > 0.05f)) mask |= 1U << i;
    }
    return mask;
}
unsigned micro_d_level(unsigned track)
{
    struct slot *d = slot_of(track);
    if (!d) return 0;
    float m = d->meter * 127.0f;
    return m > 127.0f ? 127U : (unsigned)m;
}
void micro_d_live(unsigned track, unsigned note, unsigned velocity)
{
    if (!micro_d_track(track) || note > 127) return;
    /* Two writers: the control task (keys, our ARP via groove_midi_local) and
     * the MIDI routing task (fm1_app_midi_received). A few instructions with
     * interrupts held keep one from overwriting the other's slot. */
#ifndef FM1_HOST_TEST
    uint32_t irq = fm1_device_irq_save();
#endif
    uint32_t w = q_write;
    if (w - q_read < QUEUE) {
        queue[w & (QUEUE - 1U)] = track | (note << 8) | ((velocity & 127U) << 16);
#ifndef FM1_HOST_TEST
        __asm__ volatile("csync" ::: "memory");
#endif
        q_write = w + 1U;
    }
#ifndef FM1_HOST_TEST
    fm1_device_irq_restore(irq);
#endif
}
void micro_d_state_get(uint8_t out[MD_STATE_BYTES])
{
    init_once();
    for (unsigned s = 0; s < MD_SLOTS; ++s) {
        uint8_t *o = out + s * MD_SLOT_BYTES;
        o[0] = slots[s].track; o[1] = slots[s].volume; o[2] = slots[s].preset;
        memcpy(o + 3, (const void *)slots[s].p, MD_PARAMS);
    }
}
void micro_d_state_put(const uint8_t in[MD_STATE_BYTES])
{
    init_once();
    for (unsigned t = 0; t < GROOVE_TRACKS; ++t) owner[t] = 0;
    for (unsigned s = 0; s < MD_SLOTS; ++s) {
        const uint8_t *i = in + s * MD_SLOT_BYTES;
        struct slot *d = &slots[s];
        d->track = 0xff;
        memset(d->v, 0, sizeof d->v); d->depth = 0; d->gated = 0; d->env = 0; d->gate_amp = 0;
        if (!FM1_MICRO_D || i[0] >= GROOVE_TRACKS || owner[i[0]]) continue;
        unsigned ok = 1;
        for (unsigned p = 0; p < MD_PARAMS; ++p) if (i[3 + p] > maxes[p]) ok = 0;
        if (!ok) continue;
        d->volume = i[1] > 127 ? 100 : i[1];
        d->preset = i[2] < PRESETS ? i[2] : 0;
        memcpy((void *)d->p, i + 3, MD_PARAMS);
        d->track = i[0];
        owner[i[0]] = (uint8_t)(s + 1U);
    }
}

/* ---- audio ---- */
FM1_AUDIO_CODE int micro_d_owns(unsigned track) { return track < GROOVE_TRACKS && owner[track] != 0; }
static inline __attribute__((always_inline)) void env_trigger(struct slot *d) { d->attack = 1; } /* attack restarts from the current level */
static inline __attribute__((always_inline)) unsigned voices_for(unsigned mode) { return mode == MD_MONO ? 1U : mode == MD_UNISON || mode + 1U > VOICES ? VOICES : mode + 1U; }
static inline __attribute__((always_inline)) void voice_start(struct slot *d, struct voice *v, unsigned note, unsigned vel, float detune)
{
    /* A sounding oscillator changes pitch with its phase running on: no
     * step in the wave, and its level ramps to the new velocity. */
    float level = 0.55f + 0.45f * (float)vel * (1.0f / 127);
    if (!v->on) { v->ph[0] = 0; v->ph[1] = 0.37f; v->pitch = (float)note + detune; v->level = 0; }
    if (!d->p[MD_GLIDE]) v->pitch = (float)note + detune;
    v->target = (float)note + detune;
    v->note = (uint8_t)note; v->on = 1; v->gate = 1; v->amp = level;
    v->age = ++d->clock;
}
static FM1_AUDIO_CODE void mono_play(struct slot *d, unsigned vel, unsigned retrigger)
{
    unsigned note = d->stack[d->depth - 1U];
    if (d->mode == MD_UNISON) {
        /* two voices (was four at -0.12 -0.04 +0.04 +0.12): the same mean detune */
        static const float spread[VOICES] FM1_AUDIO_DATA = {-0.08f, 0.08f};
        for (unsigned i = 0; i < VOICES; ++i) voice_start(d, &d->v[i], note, vel, spread[i] * (1.0f + (float)d->p[MD_DETUNE] * (1.0f / 32)));
    } else voice_start(d, &d->v[0], note, vel, 0);
    d->last_pitch = (float)note;
    if (retrigger) env_trigger(d);
}
static FM1_AUDIO_CODE void note_event(struct slot *d, unsigned note, unsigned vel)
{
    unsigned mode = d->p[MD_MODE] > 4 ? 4 : d->p[MD_MODE];
    if (mode != d->mode) { /* mode change: let everything fade */
        for (unsigned i = 0; i < VOICES; ++i) { d->v[i].gate = 0; d->v[i].amp = 0; }
        d->depth = 0; d->gated = 0; d->mode = (uint8_t)mode;
    }
    unsigned retrig = d->p[MD_RETRIG] != 0;
    if (mode == MD_MONO || mode == MD_UNISON) {
        unsigned k = 0; /* remove the note from the stack */
        for (unsigned i = 0; i < d->depth; ++i) if (d->stack[i] != note) d->stack[k++] = d->stack[i];
        d->depth = (uint8_t)k;
        if (vel) {
            unsigned first = d->depth == 0;
            if (d->depth == STACK) { /* drop the oldest; no memmove (Flash library code) */
                for (unsigned i = 1; i < STACK; ++i) d->stack[i - 1] = d->stack[i];
                --d->depth;
            }
            d->stack[d->depth++] = (uint8_t)note;
            mono_play(d, vel, first || retrig);
        } else if (d->depth) mono_play(d, 64, retrig);
        else for (unsigned i = 0; i < VOICES; ++i) d->v[i].gate = 0;
        d->gated = d->depth != 0;
        return;
    }
    unsigned n = voices_for(mode);
    if (vel) {
        struct voice *pick = 0;
        for (unsigned i = 0; i < n && !pick; ++i) if (d->v[i].on && d->v[i].note == note) pick = &d->v[i];
        if (!pick) { /* a free voice, else the quietest released one, else the oldest */
            for (unsigned i = 0; i < n; ++i) {
                struct voice *v = &d->v[i];
                if (v->gate) continue;
                if (!pick || v->level < pick->level) pick = v;
            }
            if (!pick) for (unsigned i = 0; i < n; ++i) if (!pick || d->v[i].age < pick->age) pick = &d->v[i];
        }
        unsigned first = d->gated == 0;
        voice_start(d, pick, note, vel, 0);
        d->last_pitch = (float)note;
        if (first || retrig) env_trigger(d);
    } else {
        for (unsigned i = 0; i < VOICES; ++i) {
            struct voice *v = &d->v[i];
            if (!v->on || v->note != note || !v->gate) continue;
            v->gate = 0;
            /* With other notes still held, the released oscillator stops
             * (paraphonic); the last one rings through the envelope. */
        }
    }
    unsigned g = 0;
    for (unsigned i = 0; i < VOICES; ++i) g += d->v[i].gate;
    if (g) for (unsigned i = 0; i < VOICES; ++i) if (!d->v[i].gate) d->v[i].amp = 0;
    d->gated = (uint8_t)g;
}
FM1_AUDIO_CODE void micro_d_note(unsigned track, unsigned note, unsigned velocity)
{
    if (track >= GROOVE_TRACKS || !owner[track] || note > 127) return;
    unsigned lane = track & 1U;
    uint32_t w = iq_write[lane];
    if (w - iq_read[lane] >= QUEUE) return; /* full: the note is dropped, as before */
#ifdef FM1_HOST_TEST
    if (md_test_note_mid) { void (*f)(void) = md_test_note_mid; md_test_note_mid = 0; f(); }
#endif
    iqueue[lane][w & (QUEUE - 1U)] = track | (note << 8) | ((velocity > 127 ? 127U : velocity) << 16);
    lane_barrier(); /* the entry is written before the other core sees the index move */
    iq_write[lane] = w + 1U;
}
static FM1_AUDIO_CODE void play_event(uint32_t e)
{
    unsigned t = e & 255U;
    if (t >= GROOVE_TRACKS) return;
    unsigned o = owner[t]; /* read once: the control task may change it */
    if (o) note_event(&slots[o - 1U], (e >> 8) & 255U, (e >> 16) & 127U);
}
/* One oscillator sample. The shape set is fixed for the block (flags computed
 * before the loops) and the levels carry norm and the layer gain; SHARK =
 * (saw + tri) / 2 is folded into the saw and tri levels. Saw and square share
 * one blep(ph, dt); a shape at level 0 costs nothing. */
static inline __attribute__((always_inline)) float osc(float ph, float dt, unsigned use_saw, unsigned use_tri, unsigned use_sqr,
                                                       float c_saw, float c_tri, float c_sqr, float dc_sqr, float width)
{
    float b = use_saw || use_sqr ? blep(ph, dt) : 0.0f, out = 0.0f;
    if (use_saw) out += (2.0f * ph - 1.0f - b) * c_saw;
    if (use_tri) out += (1.0f - 4.0f * (ph < 0.5f ? 0.5f - ph : ph - 0.5f)) * c_tri;
    if (use_sqr) {
        float q = ph < width ? 1.0f : -1.0f, p2 = ph - width; if (p2 < 0) p2 += 1.0f;
        q += b - blep(p2, dt);
        out += q * c_sqr - dc_sqr; /* centred: narrow pulses keep no DC */
    }
    return out;
}
static inline __attribute__((always_inline)) float rnd(uint32_t *state)
{
    uint32_t x = *state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *state = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}
/* LFO, once per sub-block */
static inline __attribute__((always_inline)) void lfo_step(struct slot *d, float step, unsigned wave, uint32_t *noise)
{
    d->lfo_ph += step;
    if (d->lfo_ph >= 1.0f) { d->lfo_ph -= 1.0f; d->sh = rnd(noise); }
    float ph = d->lfo_ph, l;
    switch (wave) {
    case 0: l = fsin_cycles(ph); break;
    case 1: l = 1.0f - 4.0f * (ph < 0.5f ? 0.5f - ph : ph - 0.5f); break;
    case 2: l = ph < 0.5f ? 1.0f : -1.0f; break;
    default: l = d->sh; break;
    }
    d->lfo_val += (l - d->lfo_val) * 0.5f; /* soften the square and S&H steps */
}
/* Ladder coefficient for the sub-block from the envelope, LFO and key. */
static inline __attribute__((always_inline)) float filter_g(const struct slot *d, float cut, float contour, float lfo_flt, float env)
{
    float oct = contour * env + lfo_flt * d->lfo_val + (d->last_pitch - 60.0f) * (0.5f / 12);
    float fc = clampf(30.0f * fexp2(cut + oct), 20.0f, 12000.0f);
    float w = fc * (2.0f * PI_F / FS);
    return w / (1.0f + w);
}
_Static_assert(FM1_BLOCK_FRAMES == 128, "key idle: seven squarings = 128 samples");
static FM1_AUDIO_CODE void render_slot(struct slot *d, int32_t *out)
{
    const uint8_t *p = (const uint8_t *)d->p;
    unsigned track = d->track; /* read once: the control task may change it */
    /* mixer pan, with the output scale folded in */
    float out_l = (track < GROOVE_TRACKS ? track_pan_gain[track][0] : 1.0f) * (0.8f * 8388608.0f);
    float out_r = (track < GROOVE_TRACKS ? track_pan_gain[track][1] : 1.0f) * (0.8f * 8388608.0f);
    unsigned mode = p[MD_MODE] > 4 ? 4 : p[MD_MODE];
    if (mode != d->mode) note_event(d, 0, 0); /* applies the mode change */
    unsigned gate_vca = p[MD_VCA] != 0, wave = p[MD_WAVE] & 3U, key_idle = 0;
    float noise_l = p[MD_NOISE] == 64 ? 0.0f : (p[MD_NOISE] < 64 ? (64.0f - p[MD_NOISE]) * (1.0f / 64) : (p[MD_NOISE] - 64.0f) * (1.0f / 63));
    noise_l *= noise_l * 0.8f;
    float drive = 1.0f + (float)p[MD_DRIVE] * (3.0f / 127), drive_in = 1.2f * drive;
    if (gate_vca && !d->gated && d->gate_amp < 1e-5f) {
        /* KEY: the gate has closed, the ladder output reaches the DC blocker
         * at below -100 dB. Free the oscillators (as the first sub-block below
         * would), and once the ladder has rung out (down to 1e-4, or to the
         * noise floor when AIR is on) stop the full render. */
        for (unsigned i = 0; i < VOICES; ++i) { d->v[i].on = 0; d->v[i].level = 0; }
        float ym = 0.0f;
        for (unsigned i = 0; i < 4; ++i) { float a = d->y[i] < 0 ? -d->y[i] : d->y[i]; if (a > ym) ym = a; }
        key_idle = ym < 1e-4f + 4.0f * noise_l * drive_in;
    }
    if (key_idle) {
        /* The next note must start as it did when the silent blocks were
         * still rendered, so its starting point is set here:
         * - env (the filter contour, whose attack restarts from the current
         *   level) keeps falling as rendering would, env *= (1 - dec)^128 per
         *   block, and the LFO keeps running, with the cutoff g_prev at the
         *   block end; both stop at env < 1e-5, where the old code stopped
         *   rendering too (below).
         * - gate_amp 0 (it was < 1e-5 and falling), attack 0.
         * - ladder: 0 when it has rung out (below 1e-4); with AIR on it keeps
         *   its filtered noise (as the old code kept it once env < 1e-5).
         * - the DC blocker's own tail (from the signal's offset at the gate
         *   close, up to about -55 dBFS, 45 ms time constant) is still played
         *   here, sample by sample, as the old code did with a silent input;
         *   then the blocker and mute_gain rest at 0 / their target.
         * - the noise generator steps as AIR would draw it (integer only), so
         *   the noise and S&H values that follow are the same. */
        d->gate_amp = 0.0f; d->attack = 0;
        if (noise_l <= 0.0f) { d->y[0] = 0.0f; d->y[1] = 0.0f; d->y[2] = 0.0f; d->y[3] = 0.0f; }
        float dx = d->dc_x, dy = d->dc_y, mute_to = d->audible ? 1.0f : 0.0f;
        if (dx > 1e-7f || dx < -1e-7f || dy > 1e-7f || dy < -1e-7f) { /* below 1e-7 the output rounds to 0 */
            float mg = d->mute_gain, vol = (float)d->volume * (1.0f / 127), peak = 0.0f;
            vol *= vol;
            for (unsigned j = 0; j < FM1_BLOCK_FRAMES; ++j) {
                float dc = 0.9995f * dy - dx; dx = 0.0f; dy = dc;
                mg += (mute_to - mg) * 0.004f;
                float s = dc * vol * mg;
                float a = s < 0 ? -s : s; if (a > peak) peak = a;
                out[2 * j] += (int32_t)(s * out_l); out[2 * j + 1] += (int32_t)(s * out_r);
            }
            d->dc_x = dx; d->dc_y = dy; d->mute_gain = mg;
            d->meter = peak > d->meter ? peak : d->meter * 0.9f;
        } else {
            d->dc_x = 0.0f; d->dc_y = 0.0f; d->mute_gain = mute_to;
            d->meter *= 0.8f;
        }
        if (d->env <= 1e-5f) return;
    } else {
        unsigned any = d->env > 1e-5f || d->gated;
        for (unsigned i = 0; i < VOICES; ++i) any |= d->v[i].on && d->v[i].level > 1e-5f;
        if (!any) { d->meter *= 0.8f; return; }
    }
    float lfo_inc = 0.05f * fexp2((float)p[MD_RATE] * (1.0f / 127) * 8.64f) * ((float)SUB / FS); /* per sub-block */
    float cut = (float)p[MD_CUTOFF] * (1.0f / 127) * 8.7f;
    float contour = ((float)p[MD_CONTOUR] - 64.0f) * (5.0f / 63); /* +-5 octaves at full envelope */
    float amount = ((float)p[MD_AMOUNT] - 63.0f) * (1.0f / 63);
    float lfo_flt = amount > 0 ? amount * 2.5f : 0.0f, lfo_pitch = amount < 0 ? -amount * 0.6f : 0.0f; /* octaves / semitones */
    float dec = coef(ctl_time(p[MD_DECAY], 0.005f, 10.0f));
    if (key_idle) {
        float r = 1.0f - dec;
        for (unsigned i = 0; i < 7; ++i) r *= r; /* (1 - dec)^128 */
        for (unsigned base = 0; base < FM1_BLOCK_FRAMES; base += SUB) {
            lfo_step(d, lfo_inc, wave, &d->noise);
            if (noise_l > 0.0f) for (unsigned j = 0; j < SUB; ++j) (void)rnd(&d->noise); /* AIR's draws: keeps the generator in step */
        }
        d->env *= r;
        d->g_prev = filter_g(d, cut, contour, lfo_flt, d->env);
        return;
    }
    MD_COUNT(md_test_busy);
    float sq = (float)p[MD_SQR];
    float saw_l = (float)p[MD_SAW] * (1.0f / 127), shark_l = (float)p[MD_SHARK] * (1.0f / 127), tri_l = (float)p[MD_TRI] * (1.0f / 127);
    float sqr_l = (sq > 64.0f ? 1.0f : sq * (1.0f / 64)), width = 0.5f - (sq > 64.0f ? (sq - 64.0f) * (0.4f / 63) : 0.0f);
    saw_l *= saw_l; shark_l *= shark_l; sqr_l *= sqr_l; tri_l *= tri_l; /* audio taper */
    float sum = saw_l + shark_l + sqr_l + tri_l, norm = 1.0f / (sum > 1.0f ? sum : 1.0f);
    unsigned pink = p[MD_NOISE] < 64;
    float det = (float)p[MD_DETUNE] * (1.0f / 127); det = det * det * 0.5f; /* up to half a semitone */
    unsigned layers = p[MD_DETUNE] && mode != MD_UNISON ? 2U : 1U;
    float det_ratio = layers > 1 ? fexp2(det * (1.0f / 12)) : 0.0f;
    float glide = p[MD_GLIDE] ? coef(ctl_time(p[MD_GLIDE], 0.005f, 9.0f)) * SUB : 1.0f;
    if (glide > 1.0f) glide = 1.0f;
    float atk = coef(p[MD_ATTACK] ? ctl_time(p[MD_ATTACK], 0.001f, 12.3f) : 0.001f);
    float sus = (float)p[MD_SUSTAIN] * (1.0f / 127);
    /* PEAK grows slowly at first (res^1.6) and stops just short of the
     * ladder's self-oscillation (k 4): no sudden whistle half-way up. */
    float res = (float)p[MD_RESO] * (1.0f / 127), k = 3.7f * res * fsqrt_approx(res) * fsqrt_approx(fsqrt_approx(res));
    float comp = (1.0f + 0.4f * k) / (0.5f + 0.5f * drive); /* drive saturates, loudness stays close */
    float vol = (float)d->volume * (1.0f / 127); vol *= vol;
    float mute_to = d->audible ? 1.0f : 0.0f; /* MUTE / SOLO fade over ~6 ms */
    float amp_ramp = 1.0f / (0.004f * FS);
    /* The shape mix, fixed for the block: norm and the layer gain are folded
     * into the levels, and only shapes with a level are computed. */
    float scale = norm * (layers > 1 ? 0.7f : 1.0f);
    float c_saw = (saw_l + 0.5f * shark_l) * scale, c_tri = (tri_l + 0.5f * shark_l) * scale, c_sqr = sqr_l * scale;
    float dc_sqr = (2.0f * width - 1.0f) * c_sqr;
    unsigned use_saw = c_saw > 0.0f, use_tri = c_tri > 0.0f, use_sqr = c_sqr > 0.0f;
    /* per-sample state in locals for the block */
    float env = d->env, gate_amp = d->gate_amp, mute_gain = d->mute_gain, dc_x = d->dc_x, dc_y = d->dc_y;
    float pk0 = d->pink[0], pk1 = d->pink[1], pk2 = d->pink[2];
    uint32_t noise = d->noise;
    unsigned gated = d->gated, attack = d->attack;
    float peak = 0.0f;
    for (unsigned base = 0; base < FM1_BLOCK_FRAMES; base += SUB) {
        lfo_step(d, lfo_inc, wave, &noise);
        /* oscillator increments */
        float inc[VOICES][2];
        for (unsigned i = 0; i < VOICES; ++i) {
            struct voice *v = &d->v[i];
            if (!v->on) continue;
            v->pitch += (v->target - v->pitch) * glide;
            float semi = v->pitch - 69.0f + d->lfo_val * lfo_pitch;
            inc[i][0] = fexp2(semi * (1.0f / 12)) * (440.0f / FS);
            inc[i][1] = layers > 1 ? inc[i][0] * det_ratio : 0.0f;
            if (inc[i][0] > 0.45f) inc[i][0] = 0.45f;
            if (inc[i][1] > 0.45f) inc[i][1] = 0.45f;
        }
        /* the envelope (or gate) has closed: free the oscillators */
        if (!gated && (gate_vca ? gate_amp : env) < 1e-5f)
            for (unsigned i = 0; i < VOICES; ++i) { d->v[i].on = 0; d->v[i].level = 0; }
        /* filter cutoff, interpolated across the sub-block */
        float g1 = filter_g(d, cut, contour, lfo_flt, env);
        if (d->g_prev < 0) d->g_prev = g1;
        float g0 = d->g_prev; d->g_prev = g1;
        /* oscillators, one voice at a time across the sub-block */
        float xb[SUB];
        unsigned filled = 0;
        for (unsigned i = 0; i < VOICES; ++i) {
            struct voice *v = &d->v[i];
            if (!v->on) continue;
            float level = v->level, target = v->amp, ph0 = v->ph[0], ph1 = v->ph[1], dt0 = inc[i][0], dt1 = inc[i][1];
            unsigned held = v->gate, alive = 1;
            for (unsigned j = 0; j < SUB; ++j) {
                float w = 0.0f;
                if (alive) {
                    level += target > level ? (level + amp_ramp > target ? target - level : amp_ramp)
                                            : (level - amp_ramp < target ? target - level : -amp_ramp);
                    if (level <= 0.0f && !held) { level = 0.0f; alive = 0; }
                    else {
                        float o = osc(ph0, dt0, use_saw, use_tri, use_sqr, c_saw, c_tri, c_sqr, dc_sqr, width);
                        ph0 += dt0; if (ph0 >= 1.0f) ph0 -= 1.0f;
                        if (layers > 1) {
                            o += osc(ph1, dt1, use_saw, use_tri, use_sqr, c_saw, c_tri, c_sqr, dc_sqr, width);
                            ph1 += dt1; if (ph1 >= 1.0f) ph1 -= 1.0f;
                        }
                        w = o * level;
                    }
                }
                xb[j] = (filled ? xb[j] : 0.0f) + w;
            }
            filled = 1;
            v->level = level; v->ph[0] = ph0; v->ph[1] = ph1;
            if (!alive) v->on = 0;
        }
        float y0 = d->y[0], y1 = d->y[1], y2 = d->y[2], y3 = d->y[3];
        for (unsigned j = 0; j < SUB; ++j) {
            /* envelope: attack aims past 1 like an analog contour, then decays to sustain */
            if (gated) {
                if (attack) { env += (1.25f - env) * atk; if (env >= 1.0f) { env = 1.0f; attack = 0; } }
                else env += (sus - env) * dec;
            } else { attack = 0; env -= env * dec; }
            float ga = gated ? 1.0f : 0.0f;
            gate_amp += (ga - gate_amp) * 0.02f;
            float x = filled ? xb[j] : 0.0f;
            if (noise_l > 0.0f) {
                float n = rnd(&noise);
                if (pink) { /* economy pink: three leaky stages */
                    pk0 = 0.99765f * pk0 + n * 0.0990460f;
                    pk1 = 0.96300f * pk1 + n * 0.2965164f;
                    pk2 = 0.57000f * pk2 + n * 1.0526913f;
                    n = (pk0 + pk1 + pk2 + n * 0.1848f) * 0.25f;
                }
                x += n * noise_l;
            }
            /* 24 dB ladder, saturating stages (as LADDER in master_fx.c) */
            float g = g0 + (g1 - g0) * ((float)j * (1.0f / SUB));
            float u = ftanh(x * drive_in - k * y3) + 1e-20f;
            float t0 = fsat(y0), t1 = fsat(y1), t2 = fsat(y2), t3 = fsat(y3);
            y0 += g * (u - t0); y1 += g * (t0 - t1); y2 += g * (t1 - t2); y3 += g * (t2 - t3);
            float y = y3 * comp * (gate_vca ? gate_amp : env);
            /* DC blocker, then the track volume */
            float dc = y - dc_x + 0.9995f * dc_y; dc_x = y; dc_y = dc;
            mute_gain += (mute_to - mute_gain) * 0.004f;
            float s = dc * vol * mute_gain;
            float a = s < 0 ? -s : s; if (a > peak) peak = a;
            out[2 * (base + j)] += (int32_t)(s * out_l); out[2 * (base + j) + 1] += (int32_t)(s * out_r);
        }
        d->y[0] = y0; d->y[1] = y1; d->y[2] = y2; d->y[3] = y3;
    }
    d->env = env; d->gate_amp = gate_amp; d->mute_gain = mute_gain; d->dc_x = dc_x; d->dc_y = dc_y;
    d->pink[0] = pk0; d->pink[1] = pk1; d->pink[2] = pk2;
    d->noise = noise; d->attack = (uint8_t)attack;
    d->meter = peak > d->meter ? peak : d->meter * 0.9f;
}
FM1_AUDIO_CODE void micro_d_render(int32_t *buffer)
{
    if (!inited) return;
    for (unsigned lane = 0; lane < LANES; ++lane) { /* sequencer notes, per core; order kept within a lane (= per track) */
        uint32_t r = iq_read[lane], w = iq_write[lane];
        lane_barrier(); /* the index before the entries it covers */
        while (r != w) {
            uint32_t e = iqueue[lane][r & (QUEUE - 1U)];
            r = r + 1U;
            play_event(e);
        }
        lane_barrier(); /* entries read before the slots are given back */
        iq_read[lane] = r;
    }
    while (q_read != q_write) { /* live keys from the control task */
        uint32_t e = queue[q_read & (QUEUE - 1U)];
        q_read = q_read + 1U;
        play_event(e);
    }
    for (unsigned s = 0; s < MD_SLOTS; ++s) {
        unsigned t = slots[s].track; /* read once */
        if (t < GROOVE_TRACKS && owner[t] == s + 1U) render_slot(&slots[s], buffer);
    }
}
