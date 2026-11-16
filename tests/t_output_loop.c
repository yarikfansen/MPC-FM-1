/* Host proof for audit A7: the output loop of app/audio_output.c
 * (output_scale: master gain, USB input, 24-bit clip) gives exactly the
 * result of the loop it replaced (fd30bfe, 64-bit arithmetic for every word),
 * for every gain 0..127, every USB state and every int32 input, including
 * full scale and values above 24 bits (BINAURAL can add there).
 * 1. output_scale directly: every gain x edge values (all bit patterns
 *    around 0, +-2^7, +-2^23, +-2^24, the int32 limits, every low-7-bit
 *    residue next to them) x USB states (none, short read, reset -2, a whole
 *    block with edge and random gains / samples).
 * 2. The same with random data: full int32 range, 24-bit range, around the
 *    clip level.
 * 3. A dense sweep of whole ranges at a few gains (every value near the clip
 *    points and the int32 limits).
 * 4. Through fm1_output_mix with fakes (gain ramp after a reset, level
 *    changes, USB and Bluetooth input): equal to a model built on the old
 *    loop, block by block.
 * audio_output.c is compiled into this file so the static loop and its USB
 * buffer can be reached. Freestanding, no C library. Exit 0 = all equal;
 * otherwise the number of the failing part. */
#include "audio_output.h"
#include "fm1_audio_io.h"
#include "fm1_engine_control.h"
#include "master_fx.h"
#include "micro_d.h"
#include "recorder.h"
#include "binaural.h"
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

#include "../app/audio_output.c"

/* ---- the loop as it was at fd30bfe (app/audio_output.c:255-262, 307-312), kept word for word ---- */
static inline int32_t old_limit(int64_t value)
{
    if (((uint64_t)value + 8388608U) >> 24)
        return value < 0 ? -8388608 : 8388607;
    return (int32_t)value;
}
static __attribute__((noinline)) void old_loop(int32_t *output, unsigned output_gain_, int usb_frames,
                                               const int32_t *usb, unsigned left, unsigned right)
{
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
        int64_t value = ((int64_t)output[i] * output_gain_) >> 7;
        if (usb_frames == FM1_BLOCK_FRAMES)
            value += ((int64_t)usb[i] * ((i & 1U) ? right : left)) >> 7;
        output[i] = usb_frames == -2 ? 0 : old_limit(value);
    }
}
static __attribute__((noinline)) void new_loop(int32_t *output, unsigned gain, int usb_frames, unsigned left, unsigned right)
{
    output_scale(output, gain, usb_frames, left, right);
}

/* ---- fakes for fm1_output_mix (part 4) ---- */
static int fake_usb_frames, fake_bt;
static uint16_t fake_gain[2];
static int32_t fake_usb[FM1_BLOCK_WORDS];
static int16_t fake_btpcm[FM1_BLOCK_WORDS];
static unsigned fake_level;
int fm1_usb_audio_write(const int32_t *pcm, unsigned frames) { (void)pcm; (void)frames; return 0; }
int fm1_usb_audio_read(int32_t *pcm, unsigned frames, uint16_t gains_q7[2])
{
    for (unsigned i = 0; i < frames * 2U; ++i) pcm[i] = fake_usb[i];
    gains_q7[0] = fake_gain[0]; gains_q7[1] = fake_gain[1];
    return fake_usb_frames;
}
int fm1_bluetooth_audio_read(int16_t *pcm, unsigned frames)
{
    for (unsigned i = 0; i < frames * 2U; ++i) pcm[i] = fake_btpcm[i];
    return fake_bt ? (int)frames : 0;
}
unsigned fm1_engine_output_level(void) { return fake_level; }
void micro_d_render(int32_t *buffer) { (void)buffer; }
void mfx_process(int32_t *buffer) { (void)buffer; }
unsigned mfx_clearing(void) { return 0; }
unsigned mfx_units_on(void) { return 0; }
void mfx_shed(unsigned fade, unsigned cut) { (void)fade; (void)cut; }
unsigned mfx_units_silent(void) { return 0; }
unsigned mfx_units_timed(void) { return 0; }
uint32_t mfx_unit_ticks(unsigned unit) { (void)unit; return 0; }
void mfx_clock_set(uint32_t (*lap_)(uint32_t *mark)) { (void)lap_; }
void rec_capture(const int32_t *usb, const int16_t *bt, const int32_t *mix) { (void)usb; (void)bt; (void)mix; }
unsigned rec_input_muted(void) { return 0; }
void binaural_render(int32_t *buffer) { (void)buffer; }
int fm1_flash_write(uint32_t offset, const void *data, size_t bytes) { (void)offset; (void)data; (void)bytes; return 0; }
int fm1_flash_erase(uint32_t offset) { (void)offset; return 0; }

/* ---- data ---- */
static uint64_t rng = 0x9e3779b97f4a7c15ULL;
static uint32_t rnd(void)
{
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}
static int32_t edges[4096];
static unsigned n_edges;
static void edge(int64_t v)
{
    if (v < -2147483648LL || v > 2147483647LL || n_edges >= 4096) return;
    edges[n_edges++] = (int32_t)v;
}
static void make_edges(void)
{
    static const int64_t centres[] = {0, 128, -128, 8388607, -8388608, 16777216, -16777216, 33554432, -33554432,
        8388608LL * 128 / 127, -8388608LL * 128 / 127, 1073741824, -1073741824, 2147483647LL, -2147483648LL,
        66052LL * 127, -66052LL * 127, 8454144, -8454144};
    for (unsigned c = 0; c < sizeof centres / sizeof *centres; ++c)
        for (int d = -130; d <= 130; ++d) edge(centres[c] + d); /* every residue of the low 7 bits on both sides */
}
static unsigned long long compared;
static int32_t a[FM1_BLOCK_WORDS], b[FM1_BLOCK_WORDS], src[FM1_BLOCK_WORDS];
/* One block: src through both loops with the same USB state; 0 = equal. */
static int both(unsigned gain, int usb_frames, unsigned left, unsigned right)
{
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) a[i] = b[i] = src[i];
    old_loop(a, gain, usb_frames, usb_pcm, left, right);
    new_loop(b, gain, usb_frames, left, right);
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) if (a[i] != b[i]) return 1;
    compared += FM1_BLOCK_WORDS;
    return 0;
}
static const int usb_states[] = {0, -2, FM1_BLOCK_FRAMES, -1, 37, 127, 129};
enum { USB_STATES = sizeof usb_states / sizeof *usb_states };
static int32_t pick(unsigned kind)
{
    uint32_t r = rnd();
    switch (kind % 5U) {
    case 0: return (int32_t)r;                                       /* any int32 */
    case 1: return (int32_t)(r & 0xffffffU) - 8388608;               /* 24 bits */
    case 2: return (int32_t)(r & 0x1ffffffU) - 16777216;             /* 25 bits: BINAURAL on top of full scale */
    case 3: return (int32_t)(r % 4096U) - 2048 + ((r >> 12) & 1U ? 8454144 : -8454144); /* at the clip point of gain 127 */
    default: return edges[r % n_edges];
    }
}
static void usb_fill(unsigned kind)
{
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) usb_pcm[i] = pick(kind + i);
}
static unsigned usb_gain_pick(unsigned n, unsigned gain)
{
    static const uint16_t g[] = {0, 1, 127, 128, 129, 255, 256, 32767, 32768, 65535};
    unsigned raw = n < 10U ? g[n] : rnd() & 0xffffU;
    return (raw * gain) >> 7; /* as fm1_output_mix makes left / right */
}

int main(void)
{
    make_edges();
    /* 1: every gain x every edge value x every USB state */
    for (unsigned gain = 0; gain <= 127; ++gain)
        for (unsigned at = 0; at < n_edges; at += FM1_BLOCK_WORDS) {
            for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) src[i] = edges[(at + i) % n_edges];
            for (unsigned u = 0; u < USB_STATES; ++u) {
                usb_fill(at + u);
                if (both(gain, usb_states[u], usb_gain_pick((at / FM1_BLOCK_WORDS + u) % 12U, gain), usb_gain_pick((gain + u) % 12U, gain))) return 1;
            }
        }
    /* 2: random data, every gain, every USB state */
    for (unsigned round = 0; round < 40; ++round)
        for (unsigned gain = 0; gain <= 127; ++gain)
            for (unsigned u = 0; u < USB_STATES; ++u) {
                for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) src[i] = pick(round + (i >> 5));
                usb_fill(round + u);
                if (both(gain, usb_states[u], usb_gain_pick(rnd() % 14U, gain), usb_gain_pick(rnd() % 14U, gain))) return 2;
            }
    /* 3: dense sweeps without USB input: every value of +-2^18 around 0, the
     * 24-bit limits, the clip points and the int32 limits, at eight gains */
    { static const unsigned gains[] = {0, 1, 2, 63, 64, 100, 126, 127};
      static const int64_t from[] = {-262144, 8388608 - 262144, -8388608 - 262144, 8454144 - 262144, -8454144 - 262144,
                                     2147483647LL - 524287, -2147483648LL};
      for (unsigned g = 0; g < 8; ++g)
          for (unsigned f = 0; f < sizeof from / sizeof *from; ++f)
              for (int64_t v = from[f]; v < from[f] + 524288; v += FM1_BLOCK_WORDS) {
                  for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) src[i] = (int32_t)(v + i);
                  if (both(gains[g], 0, 0, 0)) return 3;
              } }
    /* 4: through fm1_output_mix. The model: the gain slew of fm1_output_mix,
     * the old loop, then the Bluetooth stage (unchanged code). */
    { unsigned model_gain = 0;
      fm1_output_reset();
      for (unsigned blk = 0; blk < 6000; ++blk) {
          if (!(blk % 23U)) fake_level = blk % 3U ? rnd() % 128U : rnd() % 300U; /* also above 127: clamped */
          fake_usb_frames = usb_states[rnd() % USB_STATES];
          fake_bt = (rnd() & 3U) == 0;
          fake_gain[0] = (uint16_t)(blk % 7U ? rnd() : 65535U); fake_gain[1] = (uint16_t)(blk % 5U ? rnd() >> 3 : 128U);
          for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
              src[i] = pick(blk + (i >> 4)); fake_usb[i] = pick(blk + 2U + (i >> 3));
              fake_btpcm[i] = (int16_t)rnd();
          }
          for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) a[i] = b[i] = src[i];
          /* model */
          unsigned target = fake_level > 127U ? 127U : fake_level;
          int delta = (int)target - (int)model_gain;
          model_gain = (unsigned)((int)model_gain + (delta > 8 ? 8 : delta < -8 ? -8 : delta));
          unsigned left = ((unsigned)fake_gain[0] * model_gain) >> 7, right = ((unsigned)fake_gain[1] * model_gain) >> 7;
          old_loop(a, model_gain, fake_usb_frames, fake_usb, left, right);
          if (fake_bt)
              for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
                  int32_t value = a[i] + (int32_t)fake_btpcm[i] * (int32_t)(model_gain * 2U);
                  a[i] = value > 8388607 ? 8388607 : value < -8388608 ? -8388608 : value;
              }
          fm1_output_mix(b);
          if (output_gain != model_gain) return 4;
          for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) if (a[i] != b[i]) return 4;
          compared += FM1_BLOCK_WORDS;
      } }
#ifdef T_REPORT
    return (int)(compared / 1000000ULL); /* millions of words compared */
#endif
    return 0;
}
