/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "audio_output.h"
#include "fm1_engine_control.h"
#include "fm1_audio_io.h"
#include "master_fx.h"
#include "recorder.h"
#include "micro_d.h"
#include "binaural.h"
#ifndef FM1_HOST_TEST
#include "fm1_audio.h"
#include "fm1_device.h"
#endif

static unsigned output_gain;
/* Flash hand-shake: the ISR marks itself inside MICRO D; the writer raises
 * the hold and waits until the ISR is out before Flash stops answering.
 * MICRO D is RAM code now; the hold stays as a second guard. */
static volatile unsigned md_hold, md_busy;
/* A memory barrier without a library call (__sync_synchronize is Flash code
 * on this core and must not run in the ISR while Flash is written). */
static inline __attribute__((always_inline)) void barrier(void)
{
#ifndef FM1_HOST_TEST
    __asm__ volatile("csync" ::: "memory");
#else
    __asm__ volatile("" ::: "memory");
#endif
}
static int32_t usb_pcm[FM1_BLOCK_WORDS] __attribute__((aligned(32)));
static int16_t bluetooth_pcm[FM1_BLOCK_WORDS] __attribute__((aligned(32)));

/* Our part of the audio ISR (MICRO D, master FX, BINAURAL, USB/Bluetooth
 * mixing) in audio timer ticks, last block: the closed engine's own load
 * figures and voice governor never see it. */
static volatile uint32_t mix_ticks;
#ifdef FM1_HOST_TEST
/* Host tests drive the clock: fm1_test_timer is "now"; fm1_output_mix adds
 * fm1_test_mfx_cost to it while mfx_process runs and counts those runs. */
uint32_t fm1_test_timer, fm1_test_mfx_cost, fm1_test_mfx_runs;
#endif
static inline __attribute__((always_inline)) uint32_t timer_now(void)
{
#ifndef FM1_HOST_TEST
    return FM1_AUDIO_TIMER_COUNT;
#else
    return *(volatile uint32_t *)&fm1_test_timer;
#endif
}
static inline __attribute__((always_inline)) uint32_t elapsed(uint32_t start, uint32_t end)
{
#ifndef FM1_HOST_TEST
    return end >= start ? end - start : end + FM1_AUDIO_TIMER_PERIOD + 1U - start;
#else
    return end - start;
#endif
}
static inline __attribute__((always_inline)) uint32_t timer_since(uint32_t start)
{
    return elapsed(start, timer_now());
}
uint32_t fm1_output_mix_ticks(void) { return mix_ticks; }
volatile uint32_t fm1_mix_block_start, fm1_mix_block_ticks;
/* Load guard (audio ISR). It sheds master FX units (master_fx.h) one by one
 * in a fixed musical order and brings them back in reverse order. It never
 * touches MICRO D or the engine: it only decides which effects run.
 * Costs, all measured on the running audio (board ticks):
 *   unit_cost[u]  each unit's own time in the blocks it ran honestly
 *                 (mfx_units_timed: no FILTER TYPE crossfade);
 *   base_float    the rest of mfx_process when any unit runs (float in / out,
 *                 sub filter, limiter, chime), honest blocks only (no effect
 *                 memory cleared, mfx_clearing);
 *   base_dry      mfx_process when no unit runs (integer limiter);
 *   clear_cost    the most a block's sliced memory clearing has cost (an
 *                 effect coming back clears up to ~16 KB in ~4 KB slices).
 * unit_cost, base_float and base_dry are learned min-biased (learn_cost): a
 * lower measurement counts at once, a higher one moves the cost up by 1/8 of
 * the difference per block. Every timer read is wall time and the audio
 * interrupt is preempted by others (panel scan, USB, BT): one preempted
 * measurement must not make the effects look expensive in the next block
 * (device 11.11: ~2 500 sheds in 5 h with a light project).
 * A unit or clearing not measured yet counts as 1/32 of a block.
 * Per block, `now` = time from the block start to here (engine call +
 * MICRO D), `kept` = units on and not shed, `fading` = shed units still
 * fading out (they still run this block):
 *   shed:   now + cost(kept) > 75 % in OVER_BLOCKS blocks in a row -> the
 *           next kept unit in shed_order starts its fade-out (MFX_SHED_FADE
 *           blocks); then one unit per block while the blocks stay over. A
 *           single long block (a preempted interrupt) sheds nothing: under
 *           81.25 % it is no danger, above it the cut acts in that block;
 *   cut:    now + cost(kept + fading) > 81.25 % -> this block would come
 *           close to the 87.5 % drop limit: fading units, then kept units
 *           in order, stop at once (a small step in the sound instead of a
 *           dropped block, which is a gap);
 *   return: the last shed unit (reverse order) comes back once
 *           now + cost(kept + fading + it) + clear_cost < 64.8 % for
 *           CALM_BLOCKS in a row, and SHED_BLOCKS after the last shed /
 *           RETURN_BLOCKS after the last return: its clearing is paid for,
 *           and 64.8 % against 75 % leaves room, so it cannot re-trip by
 *           itself.
 * Units whose own costs together are under 1/32 of a block are not worth a
 * change in the sound: no shed (nor cut) while those of kept [+ fading] are
 * not above that. */
static const uint8_t shed_order[MFX_UNITS] FM1_AUDIO_DATA = {
    MFX_VERB, MFX_CHORUS, MFX_PHASER, MFX_RING, /* tails, stereo and modulation first */
    MFX_EQ, MFX_U_COMP,                         /* then tone and dynamics */
    MFX_DRIVE, MFX_U_LOWCUT, MFX_FILTER};       /* the filter path last, FILTER the very last */
static uint32_t unit_cost[MFX_UNITS], base_float, base_dry, clear_cost, fx_ticks;
static uint32_t hold_blocks, calm_blocks, over_blocks;
static unsigned shed_mask;
static volatile uint32_t shed_count;
/* Diagnostics for command 23 (compares and stores only): see audio_output.h. */
static uint32_t guard_now_peak, lone_spikes;
volatile uint32_t fm1_panel_irq_peak;
_Static_assert(FM1_OUTPUT_LOAD_UNITS == MFX_UNITS, "command 23 carries one cost per unit");
uint32_t fm1_output_fx_shed_count(void) { return shed_count; }
unsigned fm1_output_fx_shed_mask(void) { return shed_mask; }
enum { SHED_BLOCKS = 345,  /* ~1 s after a shed before anything comes back */
       RETURN_BLOCKS = 64, /* ~186 ms between returns: clear, prime and fade in first */
       CALM_BLOCKS = 16,   /* ~46 ms in a row under the come-back level */
       OVER_BLOCKS = 3,    /* ~9 ms in a row over the shed level before a unit is shed */
       PEAK_BLOCKS = 172 }; /* ~0.5 s: one bucket of the windowed peak */
static inline __attribute__((always_inline)) uint32_t cost_of(unsigned units, uint32_t block)
{
    if (!units) return base_dry;
    uint32_t c = base_float;
    for (unsigned u = 0; u < MFX_UNITS; ++u)
        if ((units >> u) & 1U) c += unit_cost[u] ? unit_cost[u] : block >> 5;
    return c;
}
static inline __attribute__((always_inline)) unsigned worth(unsigned units, uint32_t block)
{
    uint32_t c = 0;
    for (unsigned u = 0; u < MFX_UNITS; ++u)
        if ((units >> u) & 1U) c += unit_cost[u] ? unit_cost[u] : block >> 5;
    return c > (block >> 5);
}
static FM1_AUDIO_CODE void guard(uint32_t now, uint32_t block)
{
    unsigned on = mfx_units_on();
    shed_mask &= on; /* a unit the user switched off leaves the shed set */
    unsigned kept = on & ~shed_mask, fading = shed_mask & ~mfx_units_silent(), cut = 0;
    if (hold_blocks) --hold_blocks;
    uint32_t high = block - (block >> 2);                        /* 75 % */
    uint32_t limit = (block >> 1) + (block >> 2) + (block >> 4); /* 81.25 % */
    /* 1/2 + 1/8 + 1/64 + 1/128 = 64.8 %: shifts only (a division may be Flash library code) */
    uint32_t low = (block >> 1) + (block >> 3) + (block >> 6) + (block >> 7);
    unsigned acted = 0;
    uint32_t over_before = over_blocks;
    if (now > guard_now_peak) guard_now_peak = now;
    if (now + cost_of(kept, block) > high && worth(kept, block)) {
        if (over_blocks < OVER_BLOCKS) ++over_blocks;
        if (over_blocks >= OVER_BLOCKS) {
            for (unsigned i = 0; i < MFX_UNITS; ++i) {
                unsigned bit = 1U << shed_order[i];
                if (kept & bit) { shed_mask |= bit; kept &= ~bit; fading |= bit; ++shed_count; break; }
            }
            acted = 1;
        }
    } else over_blocks = 0;
    if (over_before == 1U && !over_blocks) ++lone_spikes; /* one block over, alone */
    if (now + cost_of(kept | fading, block) > limit && worth(kept | fading, block)) {
        for (unsigned i = 0; i < MFX_UNITS && now + cost_of(kept | fading, block) > limit; ++i) {
            unsigned bit = 1U << shed_order[i];
            if (fading & bit) { cut |= bit; fading &= ~bit; }
        }
        for (unsigned i = 0; i < MFX_UNITS && now + cost_of(kept | fading, block) > limit; ++i) {
            unsigned bit = 1U << shed_order[i];
            if (kept & bit) { shed_mask |= bit; cut |= bit; kept &= ~bit; ++shed_count; }
        }
        acted = 1;
    }
    if (acted) { hold_blocks = SHED_BLOCKS; calm_blocks = 0; }
    else if (shed_mask) {
        unsigned back = 0;
        for (unsigned i = MFX_UNITS; i-- > 0;)
            if ((shed_mask >> shed_order[i]) & 1U) { back = 1U << shed_order[i]; break; }
        uint32_t clear = clear_cost ? clear_cost : block >> 5;
        if (now + cost_of(kept | fading | back, block) + clear < low) { if (calm_blocks < CALM_BLOCKS) ++calm_blocks; }
        else calm_blocks = 0;
        if (!hold_blocks && calm_blocks >= CALM_BLOCKS) { shed_mask &= ~back; hold_blocks = RETURN_BLOCKS; calm_blocks = 0; }
    } else calm_blocks = 0;
    mfx_shed(shed_mask, cut);
}
/* Min-biased cost: lower (or first) at once, higher by 1/8 of the difference (+1, so it arrives). */
static inline __attribute__((always_inline)) void learn_cost(uint32_t *cost, uint32_t seen)
{
    if (!*cost || seen <= *cost) *cost = seen;
    else *cost += ((seen - *cost) >> 3) + 1U;
}
/* After mfx_process took `took` ticks: learn the costs from that block. */
static FM1_AUDIO_CODE void learn(uint32_t took)
{
    unsigned timed = mfx_units_timed();
    uint32_t sum = 0;
    for (unsigned u = 0; u < MFX_UNITS; ++u)
        if ((timed >> u) & 1U) sum += mfx_unit_ticks(u);
    uint32_t rest = took > sum ? took - sum : 0U;
    if (mfx_clearing()) {
        /* clearing time = the rest above the honest base (once that is known); keep the largest */
        uint32_t base = timed ? base_float : base_dry;
        if (base && rest > base && rest - base > clear_cost) clear_cost = rest - base;
        return;
    }
    fx_ticks = took;
    for (unsigned u = 0; u < MFX_UNITS; ++u)
        if ((timed >> u) & 1U) learn_cost(&unit_cost[u], mfx_unit_ticks(u));
    if (timed) learn_cost(&base_float, rest); else learn_cost(&base_dry, took);
}
static FM1_AUDIO_CODE uint32_t lap(uint32_t *mark)
{
    uint32_t now = timer_now(), d = elapsed(*mark, now);
    *mark = now;
    return d;
}
/* Windowed peak of the whole interrupt (block start to the end of our mix):
 * the highest block of the current bucket and of the one before. */
static volatile uint32_t peak_now, peak_before;
static uint32_t peak_count;
uint32_t fm1_output_peak_ticks(void)
{
    uint32_t a = peak_now, b = peak_before;
    return a > b ? a : b;
}
/* Peak-hold load figures for transfer command 23 (audio ISR writes, the
 * control task reads and clears them with interrupts held, as
 * fm1_audio_stats_read does): the engine call (block start to the entry of
 * fm1_output_mix), MICRO D render, our whole mix part. */
static uint32_t load_engine_peak, load_micro_d_peak, load_mix_peak;
void fm1_output_load_read(struct fm1_output_load *out)
{
    if (!out) return;
#ifndef FM1_HOST_TEST
    uint32_t irq = fm1_device_irq_save();
#endif
    out->shed_count = shed_count; out->shedding = shed_mask != 0; out->fx_ticks = fx_ticks;
    out->block_ticks = fm1_mix_block_ticks;
    out->engine_peak = load_engine_peak; out->micro_d_peak = load_micro_d_peak; out->mix_peak = load_mix_peak;
    load_engine_peak = load_micro_d_peak = load_mix_peak = 0;
    for (unsigned u = 0; u < MFX_UNITS; ++u) out->unit_cost[u] = unit_cost[u];
    out->base_float = base_float; out->base_dry = base_dry; out->clear_cost = clear_cost;
    out->guard_now_peak = guard_now_peak; out->lone_spikes = lone_spikes;
    out->panel_irq_peak = fm1_panel_irq_peak;
    guard_now_peak = 0; fm1_panel_irq_peak = 0;
#ifndef FM1_HOST_TEST
    fm1_device_irq_restore(irq);
#endif
}
uint32_t fm1_output_timer(void) { return timer_now(); }
uint32_t fm1_output_timer_since(uint32_t start) { return timer_since(start); }
uint32_t fm1_output_timer_period(void)
{
#ifndef FM1_HOST_TEST
    return FM1_AUDIO_TIMER_PERIOD;
#else
    return 0;
#endif
}
uint32_t fm1_output_block_ticks(void)
{
#ifndef FM1_HOST_TEST
    return (uint32_t)((uint64_t)fm1_device_clock() * FM1_BLOCK_FRAMES / FM1_AUDIO_HARDWARE_RATE);
#else
    return 0;
#endif
}
void fm1_output_reset(void) { output_gain = 0; mfx_clock_set(lap); }
static inline __attribute__((always_inline)) int32_t limit(int64_t value)
{
    /* Bias signed 24-bit PCM into [0, 2^24). Unsigned arithmetic also
     * defines the check at the int64 limits, without two wide compares. */
    if (((uint64_t)value + 8388608U) >> 24)
        return value < 0 ? -8388608 : 8388607;
    return (int32_t)value;
}
/* Master gain and the 24-bit clip; a whole USB input block is added before
 * the clip. gain <= 127 (output_gain is clamped), left / right <= 65 024.
 * With USB input the sum needs 64 bits (int32 x 17 bits). Without it 32 bits
 * give the same result for EVERY int32 input (BINAURAL can add above 24 bits):
 * with x = 128 * h + l, h = x >> 7 (floor), l = x & 127 in 0..127:
 *   (x * gain) >> 7 = floor((128 * h * gain + l * gain) / 128)
 *                   = h * gain + ((l * gain) >> 7)        (h * gain is whole)
 * and |h * gain| <= 2^24 * 127 < 2^31, l * gain <= 127 * 127: no overflow.
 * Proof by test: tests/t_output_loop.c (old loop against this one). */
static inline __attribute__((always_inline)) void output_scale(int32_t *output, unsigned gain, int usb_frames,
                                                               unsigned left, unsigned right)
{
    if (usb_frames == FM1_BLOCK_FRAMES) {
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
            int64_t value = ((int64_t)output[i] * gain) >> 7;
            value += ((int64_t)usb_pcm[i] * ((i & 1U) ? right : left)) >> 7;
            output[i] = limit(value);
        }
        return;
    }
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
        int32_t x = output[i];
        int32_t value = (x >> 7) * (int32_t)gain + (int32_t)(((uint32_t)(x & 127) * gain) >> 7);
        output[i] = usb_frames == -2 ? 0 : value > 8388607 ? 8388607 : value < -8388608 ? -8388608 : value;
    }
}
FM1_AUDIO_CODE void fm1_output_mix(int32_t *output)
{
    if (!output) return;
    uint32_t start = timer_now();
    uint32_t engine = elapsed(fm1_mix_block_start, start); /* the engine call before us */
    md_busy = 1;
    barrier();
    if (!md_hold) micro_d_render(output); /* MICRO D tracks join the instrument mix */
    barrier();
    md_busy = 0;
    uint32_t micro_d = timer_since(start);
    /* Load guard: decide which master FX units run this block (see guard). */
    uint32_t block = fm1_mix_block_ticks;
    if (block) guard(timer_since(fm1_mix_block_start), block);
    else { shed_mask = 0; mfx_shed(0, 0); }
    uint32_t fx_start = timer_now();
    mfx_process(output); /* master insert effects on the instrument mix */
#ifdef FM1_HOST_TEST
    fm1_test_timer += fm1_test_mfx_cost; ++fm1_test_mfx_runs;
#endif
    learn(timer_since(fx_start));
    unsigned target = fm1_engine_output_level();
    if (target > 127U) target = 127U;
    int delta = (int)target - (int)output_gain;
    output_gain = (unsigned)((int)output_gain + (delta > 8 ? 8 : delta < -8 ? -8 : delta));

    uint16_t usb_gain[2] = {0, 0};
    int usb_frames = fm1_usb_audio_read(usb_pcm, FM1_BLOCK_FRAMES, usb_gain);
    int bt_ok = fm1_bluetooth_audio_read(bluetooth_pcm, FM1_BLOCK_FRAMES) == FM1_BLOCK_FRAMES;
    /* Sampler recording takes the inputs and the synth's own mix (before the
     * inputs are added); MONITOR OFF keeps the inputs out of the output. */
    rec_capture(usb_frames == FM1_BLOCK_FRAMES ? usb_pcm : NULL, bt_ok ? bluetooth_pcm : NULL, output);
    if (rec_input_muted()) { usb_frames = 0; bt_ok = 0; }
    /* BINAURAL here, after rec_capture and before the output gain: its tones
     * and noise reach neither the sampler recording nor the master FX (a mono
     * or modulated effect would smear the L/R difference), and they follow the
     * master volume. Inside, the crossfeed (B1) runs first on the instrument
     * mix, then the tones (A) are added, so the crossfeed never blends the two
     * ears of the beat. Off = no cost. */
    binaural_render(output);
    unsigned left = ((unsigned)usb_gain[0] * output_gain) >> 7;
    unsigned right = ((unsigned)usb_gain[1] * output_gain) >> 7;
    /* Accumulate USB before clipping, then add Bluetooth.
     * Discard the block if the USB stream resets. */
    output_scale(output, output_gain, usb_frames, left, right);
    if (bt_ok) {
        /* output is already clipped to 24 bits. Adding signed 16-bit PCM
         * times at most 254 fits in 25 bits, so this stage needs no int64. */
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
            int32_t value = output[i] + (int32_t)bluetooth_pcm[i] * (int32_t)(output_gain * 2U);
            output[i] = value > 8388607 ? 8388607 : value < -8388608 ? -8388608 : value;
        }
    }
    /* USB gets the full mix; the board output (speaker + headphones) always
     * plays it too (F12: the recording mute of 7715441 is removed). */
    (void)fm1_usb_audio_write(output, FM1_BLOCK_FRAMES);
    uint32_t mix = timer_since(start);
    mix_ticks = mix;
    /* Peak hold for command 23: compares only, cleared by fm1_output_load_read. */
    if (engine > load_engine_peak) load_engine_peak = engine;
    if (micro_d > load_micro_d_peak) load_micro_d_peak = micro_d;
    if (mix > load_mix_peak) load_mix_peak = mix;
    uint32_t whole = timer_since(fm1_mix_block_start);
    if (whole > peak_now) peak_now = whole;
    if (++peak_count >= PEAK_BLOCKS) { peak_before = peak_now; peak_now = 0; peak_count = 0; }
}
static void hold(unsigned on)
{
    md_hold = on;
    barrier();
    while (on && md_busy) { }
}
int flash_erase_safe(uint32_t offset) { hold(1); int r = fm1_flash_erase(offset); hold(0); return r; }
int flash_write_safe(uint32_t offset, const void *data, size_t bytes) { hold(1); int r = fm1_flash_write(offset, data, bytes); hold(0); return r; }
