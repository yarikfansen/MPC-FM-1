/* Host check for the master FX load guard in steps (app/audio_output.c), the
 * shed fades and the sliced effect clearing (app/master_fx.c), freestanding,
 * no C library.
 * The test drives the audio clock (fm1_test_timer): the engine time of a
 * block is added before fm1_output_mix, MICRO D's in its render, each master
 * FX unit's after it runs (mfx_test_spend), clearing per byte zeroed
 * (mfx_test_zero), and the rest of mfx_process by audio_output.c
 * (fm1_test_mfx_cost).
 * master_fx.c is compiled into this file so its state can be inspected.
 * Exit 0 = every check passed; otherwise the number of the failing case
 * (112 = case 12b, 131..136 = the checks of case 13 in order, 141..144 =
 * case 14: command 23 version 2 diagnostics). */
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

#include "../app/master_fx.c"
int _fltused = 1; /* MSVC ABI marker for float code without a C library */

/* ---- fakes for everything else fm1_output_mix calls ---- */
int fm1_usb_audio_write(const int32_t *pcm, unsigned frames) { (void)pcm; (void)frames; return 0; }
int fm1_usb_audio_read(int32_t *pcm, unsigned frames, uint16_t gains_q7[2]) { (void)pcm; (void)frames; (void)gains_q7; return 0; }
int fm1_bluetooth_audio_read(int16_t *pcm, unsigned frames) { (void)pcm; (void)frames; return 0; }
unsigned fm1_engine_output_level(void) { return 127; }
static uint32_t md_cost, md_calls, md_seen; /* ticks one micro_d_render call takes, calls, clock at its start */
void micro_d_render(int32_t *buffer) { (void)buffer; ++md_calls; md_seen = fm1_test_timer; fm1_test_timer += md_cost; }
void rec_capture(const int32_t *usb, const int16_t *bt, const int32_t *mix) { (void)usb; (void)bt; (void)mix; }
unsigned rec_input_muted(void) { return 0; }
void binaural_render(int32_t *buffer) { (void)buffer; }
int fm1_flash_write(uint32_t offset, const void *data, size_t bytes) { (void)offset; (void)data; (void)bytes; return 0; }
int fm1_flash_erase(uint32_t offset) { (void)offset; return 0; }

/* ---- helpers ---- */
enum { BLOCK = 100000 }; /* ticks of one block: 1 % = 1000 ticks */
static int32_t buf[FM1_BLOCK_WORDS], dry[FM1_BLOCK_WORDS];
static unsigned max_zeroed; /* largest bytes zeroed in one block since the last reset */

/* Test costs: each master FX unit's own time (spent after it runs), and the
 * time sliced clearing takes per byte zeroed (ticks per KB). */
static uint32_t ucost[MFX_UNITS], clear_per_kb;
static void spend(unsigned u) { fm1_test_timer += ucost[u]; }
static void spend_zero(unsigned bytes) { fm1_test_timer += bytes * clear_per_kb / 1024U; }

/* 440 Hz sine at 0.5 full scale, by the rotation recurrence (no libm). */
static double s_prev, s_cur, s_k;
static void sine_start(void)
{
    const double w = 2.0 * 3.14159265358979323846 * 440.0 / 44100.0;
    double c = 1, t = 1, q = w * w; /* cos(w) by Taylor */
    for (int n = 1; n < 12; ++n) { t *= -q / ((2.0 * n - 1) * (2.0 * n)); c += t; }
    s_k = 2.0 * c; s_prev = 0.0;
    double sn = w, u = w; /* sin(w) */
    for (int n = 1; n < 12; ++n) { u *= -q / ((2.0 * n) * (2.0 * n + 1)); sn += u; }
    s_cur = sn;
}
static void sine_fill(void)
{
    for (unsigned f = 0; f < FM1_BLOCK_FRAMES; ++f) {
        int32_t v = (int32_t)(0.5 * s_cur * 8388607.0);
        double nx = s_k * s_cur - s_prev; s_prev = s_cur; s_cur = nx;
        buf[2 * f] = buf[2 * f + 1] = dry[2 * f] = dry[2 * f + 1] = v;
    }
}
static double prev_out[2], prev_dry[2], max_jump, max_dry_jump;
static unsigned have_prev, differs;
static double fabs_d(double x) { return x < 0 ? -x : x; }
static void track(void)
{
    for (unsigned f = 0; f < FM1_BLOCK_FRAMES; ++f)
        for (unsigned c = 0; c < 2; ++c) {
            double o = buf[2 * f + c] / 8388608.0, d = dry[2 * f + c] / 8388608.0;
            if (have_prev) {
                if (fabs_d(o - prev_out[c]) > max_jump) max_jump = fabs_d(o - prev_out[c]);
                if (fabs_d(d - prev_dry[c]) > max_dry_jump) max_dry_jump = fabs_d(d - prev_dry[c]);
            }
            if (fabs_d(o - d) > 0.001) differs = 1;
            prev_out[c] = o; prev_dry[c] = d;
        }
    have_prev = 1;
}

/* One interrupt: engine time in percent of the block (MICRO D adds md_cost),
 * then our mix; fx_pc = the rest of mfx_process (float in / out, limiter).
 * Records: highest block load up to the end of mfx_process + our mix, blocks
 * with a cut, the units added to / removed from the shed set in order, and
 * that MICRO D rendered once per block at its full cost. */
static unsigned irq_sine, irqs, cuts, md_wrong, watch_fades, fade_bad;
static float heard_before[MFX_UNITS];
static uint32_t max_load;
static uint8_t added[64], removed[64];
static unsigned n_added, n_removed;
static void irq_t(uint32_t engine_ticks, unsigned fx_pc)
{
    if (irq_sine) sine_fill();
    else for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) buf[i] = 0;
    fm1_test_timer += 12345U;            /* time between blocks: any value */
    fm1_mix_block_start = fm1_test_timer;
    fm1_test_timer += engine_ticks;
    fm1_test_mfx_cost = fx_pc * (BLOCK / 100U);
    unsigned z = mfx_test_zeroed, before = fm1_output_fx_shed_mask(), md0 = md_calls;
    uint32_t t0 = fm1_test_timer;
    fm1_output_mix(buf);
    /* MICRO D untouched: one render per block, its time fully spent before the effects */
    if (md_calls != md0 + 1U || md_seen != t0) ++md_wrong;
    uint32_t load = fm1_test_timer - fm1_mix_block_start;
    if (load > max_load) max_load = load;
    if (shed_cut) ++cuts;
    /* Fades: a unit's heard gain (reached at the end of the block, ramped
     * per sample inside it) falls at most 1/MFX_SHED_FADE and rises at most
     * 1/16 per block, except in a block with a cut. */
    for (unsigned u = 0; u < MFX_UNITS; ++u) {
        float d = fx_heard[u] - heard_before[u];
        if (watch_fades && !shed_cut && (d < -1.0f / MFX_SHED_FADE - 1e-6f || d > 0.0625f + 1e-6f)) ++fade_bad;
        heard_before[u] = fx_heard[u];
    }
    unsigned after = fm1_output_fx_shed_mask();
    for (unsigned u = 0; u < MFX_UNITS; ++u) {
        unsigned bit = 1U << u;
        if ((after & bit) && !(before & bit) && n_added < 64) added[n_added++] = (uint8_t)u;
        if ((before & bit) && !(after & bit) && n_removed < 64) removed[n_removed++] = (uint8_t)u;
    }
    if (mfx_test_zeroed - z > max_zeroed) max_zeroed = mfx_test_zeroed - z;
    if (irq_sine) track();
    ++irqs;
}
static void irq(unsigned engine_pc, unsigned fx_pc) { irq_t(engine_pc * (BLOCK / 100U), fx_pc); }
static void events_clear(void) { n_added = n_removed = 0; cuts = 0; max_load = 0; }

/* One block of the sine through mfx_process; tracks jumps and bytes zeroed. */
static void fx_block(void)
{
    sine_fill();
    unsigned z = mfx_test_zeroed;
    mfx_process(buf);
    if (mfx_test_zeroed - z > max_zeroed) max_zeroed = mfx_test_zeroed - z;
    track();
}
static void set(unsigned fx, unsigned param, int value) { mfx_edit(fx, param, -255); mfx_edit(fx, param, value); }
/* Back to a clean master FX state: no effect on, nothing shed (also the
 * guard's set: it drops units that are off on the next block). */
static void fx_fresh(void)
{
    mfx_reset(); mfx_comp_edit(3, -255); mfx_lowcut_set(0);
    irq(10, 0);
    for (unsigned u = 0; u < MFX_UNITS; ++u) ucost[u] = 0;
    clear_per_kb = 0;
}

/* One effect (or two) switched on from MIX 0 on a running sine; fresh state.
 * Returns 0 when the byte, jump and audibility limits hold. */
static double last_jump; /* measured max jump of the last fx_start, for the report */
static unsigned last_audible; /* blocks until all started effects were clean, faded in and the output differed */
static int fx_start(int a, int b, unsigned audible_within)
{
    fx_fresh(); sine_start(); have_prev = 0; differs = 0;
    for (unsigned k = 0; k < 20; ++k) fx_block();
    if (differs) return 1;
    max_zeroed = 0; max_jump = max_dry_jump = 0;
    set((unsigned)a, 3, 127); if (b >= 0) set((unsigned)b, 3, 127);
    unsigned k = 0, mp = mod_pos, rp = rv_ppos;
    for (; k < audible_within; ++k) {
        fx_block();
        if (fx_dirty[MFX_CHORUS] && (mod_pos != mp || fx_gain[MFX_CHORUS] != 0.0f)) return 2;
        if (fx_dirty[MFX_VERB] && (rv_ppos != rp || fx_gain[MFX_VERB] != 0.0f)) return 2;
        if (!fx_dirty[a] && fx_gain[a] > 0.0f && (b < 0 || (!fx_dirty[b] && fx_gain[b] > 0.0f)) && differs) break;
    }
    if (k == audible_within) return 3;
    last_audible = k + 1;
    for (unsigned n = 0; n < 100; ++n) fx_block();
    last_jump = max_jump;
    if (max_zeroed > 4200U || max_zeroed == 0) return 4;
    if (max_jump > max_dry_jump + 0.02) return 5;
    return 0;
}

/* The test scene (device figures 10.11, M2): FILTER ACID 7 %, REVERB 7 %,
 * CHORUS 3 %, EQ 2 %, COMP 1 % of the block, the rest of mfx_process 3 %;
 * MICRO D 10 %. All of them: 23 %. */
enum { SCENE_FX = 3 };
#define BIT(u) (1U << (u))
static const unsigned scene_units = BIT(MFX_FILTER) | BIT(MFX_VERB) | BIT(MFX_CHORUS) | BIT(MFX_EQ) | BIT(MFX_U_COMP);
static void scene(void)
{
    fx_fresh();
    set(MFX_FILTER, 3, 100); set(MFX_VERB, 3, 100); set(MFX_CHORUS, 3, 100);
    set(MFX_EQ, 1, 80); mfx_comp_edit(3, 100);
    ucost[MFX_FILTER] = 7000; ucost[MFX_VERB] = 7000; ucost[MFX_CHORUS] = 3000;
    ucost[MFX_EQ] = 2000; ucost[MFX_U_COMP] = 1000;
    clear_per_kb = 1000; /* 1 % per KB: a 4 KB slice costs 4 % */
    md_cost = 10U * (BLOCK / 100U);
    for (unsigned b = 0; b < 200; ++b) irq(30, SCENE_FX); /* clean, primed, faded in, measured */
}
/* Unit u fully in: running and heard at full gain. */
static unsigned unit_in(unsigned u)
{
    if ((mfx_units_silent() >> u) & 1U) return 0;
    if (!((mfx_units_timed() >> u) & 1U)) return 0;
    return fx_heard[u] == 1.0f;
}
/* Unit u out: not run, silent. */
static unsigned unit_out(unsigned u)
{
    return ((mfx_units_silent() >> u) & 1U) && !((mfx_units_timed() >> u) & 1U) && fx_heard[u] == 0.0f;
}

int main(void)
{
    fm1_output_reset();
    fm1_mix_block_ticks = BLOCK;
    mfx_test_spend = spend;
    mfx_test_zero = spend_zero;

    /* 1: a lasting load, reached gradually: engine + MICRO D rise from 40 %
     * to 60 % of the block (1 % per 40 blocks) and stay there for 2000
     * blocks; all effects together would make 83 %. The guard takes out
     * REVERB, then CHORUS (fades, no cut), and stops there: FILTER, EQ and
     * COMP keep running at full gain. No block above 81.25 %. No click: the
     * output never jumps more than the dry sine's own step + 0.02 FS. */
    scene();
    uint32_t sheds = fm1_output_fx_shed_count();
    if (fm1_output_fx_shed_mask()) return 1;
    for (unsigned u = 0; u < MFX_UNITS; ++u) if (((scene_units >> u) & 1U) && !unit_in(u)) return 1;
    sine_start(); irq_sine = 1; have_prev = 0; max_jump = max_dry_jump = 0;
    for (unsigned b = 0; b < 40; ++b) irq(30, SCENE_FX);
    watch_fades = 1; fade_bad = 0;
    events_clear(); max_jump = max_dry_jump = 0;
    for (unsigned e = 30; e <= 50; ++e) for (unsigned b = 0; b < 40; ++b) irq(e, SCENE_FX);
    for (unsigned b = 0; b < 2000; ++b) irq(50, SCENE_FX);
    if (fm1_output_fx_shed_mask() != (BIT(MFX_VERB) | BIT(MFX_CHORUS))) return 1;
    if (n_added != 2 || added[0] != MFX_VERB || added[1] != MFX_CHORUS || n_removed) return 1;
    if (fm1_output_fx_shed_count() != sheds + 2U || cuts) return 1;
    if (!unit_in(MFX_FILTER) || !unit_in(MFX_EQ) || !unit_in(MFX_U_COMP)) return 1;
    if (!unit_out(MFX_VERB) || !unit_out(MFX_CHORUS)) return 1;
    if (max_load > (BLOCK >> 1) + (BLOCK >> 2) + (BLOCK >> 4)) return 1;
    if (max_jump > max_dry_jump + 0.02 || fade_bad) return 1;
#ifdef T_REPORT
    if (T_REPORT == 7) return (int)(max_jump * 1000);
    if (T_REPORT == 8) return (int)(max_load / 100U);
#endif

    /* 2: the load falls to 35 %: the effects come back in reverse order
     * (CHORUS, then REVERB), not before ~1 s after the last shed, from clean
     * memory sliced (<= 4200 bytes a block), with no new shed in 3000 blocks
     * (no re-trip: the come-back test counts the effect's own cost and its
     * clearing); no click; all at full gain at the end. */
    events_clear(); max_zeroed = 0; max_jump = max_dry_jump = 0;
    { unsigned first = 0, second = 0;
      for (unsigned b = 0; b < 3000; ++b) {
          irq(25, SCENE_FX);
          if (!first && n_removed) first = b + 1U;
          if (!second && n_removed > 1) second = b + 1U;
      }
      if (n_removed != 2 || removed[0] != MFX_CHORUS || removed[1] != MFX_VERB || n_added) return 2;
      /* the last shed was long ago: back after 16 calm blocks, the next 64 blocks later */
      if (first < 16 || first > 18 || second < first + 64U || second > first + 66U) return 2; }
    if (fm1_output_fx_shed_count() != sheds + 2U || fm1_output_fx_shed_mask() || cuts) return 2;
    for (unsigned u = 0; u < MFX_UNITS; ++u) if (((scene_units >> u) & 1U) && !unit_in(u)) return 2;
    if (max_zeroed > 4200U || max_zeroed == 0 || fx_dirty[MFX_VERB] || fx_dirty[MFX_CHORUS]) return 2;
    if (max_jump > max_dry_jump + 0.02 || fade_bad) return 2;
    watch_fades = 0;

    /* 2b: right after a shed nothing comes back for ~1 s (345 blocks), even
     * when the load falls at once */
    scene(); events_clear();
    irq(75, SCENE_FX); /* everything out at once */
    { unsigned first = 0;
      for (unsigned b = 0; b < 1000 && !first; ++b) { irq(25, SCENE_FX); if (n_removed) first = b + 2U; }
      if (first < 345 || first > 345 + 2 || removed[0] != MFX_FILTER) return 2; }

    /* 2c: an effect comes back only when its own cost and its clearing fit
     * too: at 38 % REVERB stays out (38 + 23 + 4 clearing > 64.8), at 40 %
     * too (40 + 23 > 64.8), while everything else returns. */
    for (unsigned e = 28; e <= 30; e += 2) {
        scene();
        irq(75, SCENE_FX);
        watch_fades = 1; fade_bad = 0; /* FILTER, COMP, EQ, CHORUS fade back in */
        for (unsigned b = 0; b < 1500; ++b) irq(e, SCENE_FX);
        watch_fades = 0;
        if (fm1_output_fx_shed_mask() != BIT(MFX_VERB) || fade_bad) return 2;
    }

    /* 3: no re-trip loop at any lasting level: after a burst to 85 %, the
     * load settles at 40..64 % for 3000 blocks; in the last 2000 the set of
     * units out does not change (nothing leaves, nothing returns). */
    for (unsigned e = 30; e <= 54; e += 2) {
        scene();
        irq(75, SCENE_FX); /* burst */
        for (unsigned b = 0; b < 1000; ++b) irq(e, SCENE_FX);
        events_clear();
        for (unsigned b = 0; b < 2000; ++b) irq(e, SCENE_FX);
        if (n_added || n_removed) return 3;
    }

    /* 4: extreme load, reached gradually (engine + MICRO D 40 -> 95 %, 1 %
     * per 30 blocks): the units leave one by one in the fixed order, REVERB
     * first and FILTER last, and FILTER still runs while engine + MICRO D +
     * its own cost + the float path fit under 75 %. */
    scene(); irq_sine = 0;
    events_clear();
    { unsigned filter_gone_at = 0;
      for (unsigned e = 30; e <= 85; ++e)
          for (unsigned b = 0; b < 30; ++b) {
              irq(e, SCENE_FX);
              if (!filter_gone_at && ((fm1_output_fx_shed_mask() >> MFX_FILTER) & 1U)) filter_gone_at = e + 10U;
          }
      static const uint8_t want[5] = {MFX_VERB, MFX_CHORUS, MFX_EQ, MFX_U_COMP, MFX_FILTER};
      if (n_added != 5) return 4;
      for (unsigned i = 0; i < 5; ++i) if (added[i] != want[i]) return 4;
      /* FILTER goes once 10 + e + 3 + 7 > 75, i.e. at 66 %, not earlier */
      if (filter_gone_at != 66U) return 4; }
    if (!unit_out(MFX_FILTER)) return 4;
    /* 4b: a sudden jump from 40 % to 85 %: everything is cut in that same
     * block (the effects cannot save it, but they do not add to it). To
     * 70 %: REVERB, CHORUS and EQ are cut at once (the block stays under
     * 81.25 %); the blocks stay over 75 % (81 %), so on the third block in a
     * row COMP and, one block later, FILTER fade out: the last. */
    scene(); events_clear();
    irq(75, SCENE_FX);
    if (fm1_output_fx_shed_mask() != scene_units || shed_cut != scene_units) return 4;
    scene(); events_clear();
    irq(60, SCENE_FX);
    if (shed_cut != (BIT(MFX_VERB) | BIT(MFX_CHORUS) | BIT(MFX_EQ)) || fm1_output_fx_shed_mask() != shed_cut) return 4;
    if (!((mfx_units_timed() >> MFX_FILTER) & 1U) || !((mfx_units_timed() >> MFX_U_COMP) & 1U)) return 4;
    irq(60, SCENE_FX); /* second block over 75 %: nothing more yet */
    if (fm1_output_fx_shed_mask() != (BIT(MFX_VERB) | BIT(MFX_CHORUS) | BIT(MFX_EQ)) || shed_cut) return 4;
    irq(60, SCENE_FX); /* third */
    if (fm1_output_fx_shed_mask() != (BIT(MFX_VERB) | BIT(MFX_CHORUS) | BIT(MFX_EQ) | BIT(MFX_U_COMP)) || shed_cut) return 4;
    irq(60, SCENE_FX);
    if (fm1_output_fx_shed_mask() != scene_units || shed_cut) return 4;
    for (unsigned b = 0; b < 10; ++b) irq(60, SCENE_FX);
    if (cuts != 1 || !unit_out(MFX_FILTER) || !unit_out(MFX_U_COMP)) return 4;
    if (max_load > (BLOCK >> 1) + (BLOCK >> 2) + (BLOCK >> 4)) return 4;

    /* 5: units that cost under 1/32 of a block together (COMP 1 %, LOW CUT
     * 1 %) are never taken out, whatever the engine time */
    fx_fresh(); mfx_comp_edit(3, 100); mfx_lowcut_set(1);
    ucost[MFX_U_COMP] = 1000; ucost[MFX_U_LOWCUT] = 1000;
    events_clear();
    for (unsigned e = 0; e <= 100; e += 5)
        for (unsigned b = 0; b < 40; ++b) irq(e, 3);
    if (n_added || fm1_output_fx_shed_mask()) return 5;

    /* 6: a block that clears effect memory (here 4 % per 4 KB slice) does not
     * count as running cost: effects starting at 50 % + 23 % do not shed */
    scene(); fx_fresh(); md_cost = 10U * (BLOCK / 100U);
    ucost[MFX_FILTER] = 7000; ucost[MFX_VERB] = 7000; ucost[MFX_CHORUS] = 3000; ucost[MFX_EQ] = 2000; ucost[MFX_U_COMP] = 1000;
    clear_per_kb = 1000;
    for (unsigned b = 0; b < 50; ++b) irq(40, SCENE_FX);
    events_clear();
    set(MFX_FILTER, 3, 100); set(MFX_VERB, 3, 100); set(MFX_CHORUS, 3, 100); set(MFX_EQ, 1, 80); mfx_comp_edit(3, 100);
    { unsigned saw_clear = 0;
      for (unsigned b = 0; b < 300; ++b) { irq(40, SCENE_FX); saw_clear |= mfx_clearing(); }
      if (!saw_clear || n_added) return 6; }

    /* 7: windowed peak: one 90 % block among 40 % blocks reads 90 % for at
     * least 172 and at most 344 blocks, then 40 % (several bucket phases) */
    fx_fresh(); md_cost = 0;
    for (unsigned phase = 0; phase < 172; phase += 57) {
        for (unsigned b = 0; b < 400 + phase; ++b) irq(40, 0);
        if (fm1_output_peak_ticks() != 40U * (BLOCK / 100U)) return 7;
        irq(90, 0);
        unsigned high = 0;
        while (fm1_output_peak_ticks() == 90U * (BLOCK / 100U) && high < 1000) { ++high; irq(40, 0); }
        if (high < 172 || high > 344 || fm1_output_peak_ticks() != 40U * (BLOCK / 100U)) return 7;
    }

    /* 8: reverb MIX 0 -> 127 and chorus MIX 0 -> 127 in the same block, a
     * 0.5 FS 440 Hz sine running: never more than 4200 bytes zeroed in a
     * block, no jump over the dry signal's own plus 0.02 FS, both effects
     * audible within 80 blocks (chorus priming + 16-block fade), and not run
     * while their memory is dirty. Then each effect by itself. */
    if (fx_start(MFX_VERB, MFX_CHORUS, 80)) return 8;
    if (fx_start(MFX_CHORUS, -1, 80)) return 8;
    if (fx_start(MFX_VERB, -1, 40)) return 8;
#ifdef T_REPORT /* scratch builds: exit code = a measured figure */
    if (T_REPORT == 1) { fx_start(MFX_VERB, MFX_CHORUS, 80); return (int)(last_jump * 1000); }
    if (T_REPORT == 2) { fx_start(MFX_CHORUS, -1, 80); return (int)(last_jump * 1000); }
    if (T_REPORT == 3) { fx_start(MFX_VERB, -1, 40); return (int)(last_jump * 1000); }
    if (T_REPORT == 4) { fx_start(MFX_CHORUS, -1, 80); return (int)last_audible; }
    if (T_REPORT == 5) { fx_start(MFX_VERB, -1, 40); return (int)last_audible; }
#endif

    /* 9: a reverb TYPE change while it runs: same byte and jump limits */
    fx_fresh();
    for (unsigned b = 0; b < 20; ++b) fx_block();
    set(MFX_VERB, 3, 127);
    for (unsigned b = 0; b < 200; ++b) fx_block();
    max_zeroed = 0; max_jump = max_dry_jump = 0;
    mfx_edit(MFX_VERB, 0, 1);
    { unsigned b = 0;
      for (; b < 80 && !(fx_type[MFX_VERB] == prm[MFX_VERB][0] && fx_gain[MFX_VERB] >= 1.0f); ++b) fx_block();
      if (b == 80) return 9; }
    for (unsigned b = 0; b < 40; ++b) fx_block();
    if (max_zeroed > 4200U || max_zeroed == 0) return 9;
    if (max_jump > max_dry_jump + 0.02) return 9;
#ifdef T_REPORT
    if (T_REPORT == 6) return (int)(max_jump * 1000);
#endif

    /* 10: load figures for transfer command 23: engine call, MICRO D and
     * whole-mix peaks since the last read, cleared by each read; guard
     * state, effects cost (whole mfx_process, last honest block) and block
     * length as they are now */
    { struct fm1_output_load l;
      fx_fresh();
      md_cost = 5U * (BLOCK / 100U);
      fm1_output_load_read(&l); /* clear */
      for (unsigned b = 0; b < 5; ++b) irq(40, 10);
      irq(55, 10);
      md_cost = 9U * (BLOCK / 100U); irq(40, 10); md_cost = 5U * (BLOCK / 100U); /* one heavier MICRO D block */
      for (unsigned b = 0; b < 3; ++b) irq(40, 10);
      fm1_output_load_read(&l); /* each peak held from its own block, not the last one */
      if (l.engine_peak != 55000U || l.micro_d_peak != 9000U || l.mix_peak != 19000U) return 10;
      if (l.shedding || l.fx_ticks != 10000U || l.block_ticks != BLOCK || l.shed_count != fm1_output_fx_shed_count()) return 10;
      fm1_output_load_read(&l); /* nothing ran since: all peaks cleared */
      if (l.engine_peak || l.micro_d_peak || l.mix_peak) return 10;
      irq(20, 10);
      fm1_output_load_read(&l); /* lower than the block read before: the peak restarted */
      if (l.engine_peak != 20000U || l.micro_d_peak != 5000U || l.mix_peak != 15000U) return 10;
      scene();
      uint32_t before = fm1_output_fx_shed_count();
      irq(75, SCENE_FX); /* everything out at once: five effects counted */
      fm1_output_load_read(&l);
      if (!l.shedding || l.shed_count != before + 5U || l.engine_peak != 75000U) return 10; }

    /* 12 (device 11.11: ~2 500 sheds in 5 h with a light project): single
     * long blocks are preemption, not load. Light scene: engine 2 %, no
     * MICRO D, effects 23 % -> 25 % of the block. */
    scene(); md_cost = 0;
    for (unsigned b = 0; b < 50; ++b) irq(2, SCENE_FX);
    { uint32_t before = fm1_output_fx_shed_count();
      events_clear();
      /* a one-block spike of +55 % (80 %: over 75, under the 81.25 % cut): no shed, no cut */
      irq(57, SCENE_FX);
      if (n_added || cuts || fm1_output_fx_shed_mask()) return 12;
      for (unsigned b = 0; b < 20; ++b) irq(2, SCENE_FX);
      /* two blocks in a row: still nothing */
      irq(57, SCENE_FX); irq(57, SCENE_FX);
      for (unsigned b = 0; b < 20; ++b) irq(2, SCENE_FX);
      if (n_added || cuts || fm1_output_fx_shed_mask()) return 12;
      /* a spike in every second block for 2000 blocks: never 3 in a row, no shed */
      for (unsigned b = 0; b < 1000; ++b) { irq(57, SCENE_FX); irq(2, SCENE_FX); }
      /* two over, one under, again and again: no shed either */
      for (unsigned b = 0; b < 500; ++b) { irq(57, SCENE_FX); irq(57, SCENE_FX); irq(2, SCENE_FX); }
      if (n_added || cuts || fm1_output_fx_shed_mask() || fm1_output_fx_shed_count() != before) return 12;
      /* three blocks in a row: REVERB starts to fade exactly in the third (no cut) */
      irq(57, SCENE_FX); irq(57, SCENE_FX);
      if (n_added) return 12;
      irq(57, SCENE_FX);
      if (n_added != 1 || added[0] != MFX_VERB || cuts || fm1_output_fx_shed_count() != before + 1U) return 12;
      irq(57, SCENE_FX); /* 57 + 16 = 73 % without REVERB: not over any more */
      if (n_added != 1) return 12;
      for (unsigned b = 0; b < 20; ++b) irq(2, SCENE_FX); }
    /* 12b: a lasting overload sheds one unit per block from the third block on */
    scene(); events_clear();
    irq(46, SCENE_FX); irq(46, SCENE_FX); /* 10 + 46 + 23 = 79 %: over, under the cut */
    if (n_added) return 12;
    irq(46, SCENE_FX);
    if (n_added != 1 || added[0] != MFX_VERB) return 12;
    irq(52, SCENE_FX); /* 62 + 16 = 78 %: still over -> CHORUS at once (the fading REVERB is cut: 85 %) */
    if (n_added != 2 || added[1] != MFX_CHORUS) return 112;
    irq(56, SCENE_FX); /* 66 + 13 = 79 % -> EQ */
    if (n_added != 3 || added[2] != MFX_EQ) return 112;
    /* 12c: the cut does not wait: one single block near the drop limit
     * (10 + 50 + 23 = 83 % > 81.25 %) cuts in that same block. */
    scene(); events_clear();
    irq(50, SCENE_FX);
    if (cuts != 1 || !(shed_cut & BIT(MFX_VERB)) || !fm1_output_fx_shed_mask()) return 12;
    if (max_load > (BLOCK >> 1) + (BLOCK >> 2) + (BLOCK >> 4)) return 12;

    /* 13: costs are learned min-biased. One preempted measurement (REVERB
     * reads 30 % instead of 7 % in one block; then the rest of mfx_process
     * 30 % instead of 3 %) does not shed or cut in the blocks after it. Old
     * rule: 63 + 23 = 86 % in the next block -> shed and cut. */
    scene(); events_clear(); /* 30 + 10 + 23 = 63 % */
    ucost[MFX_VERB] = 30000; irq(30, SCENE_FX); ucost[MFX_VERB] = 7000;
    for (unsigned b = 0; b < 50; ++b) irq(30, SCENE_FX);
    if (n_added || cuts || fm1_output_fx_shed_mask()) return 131;
    irq(30, 30);
    for (unsigned b = 0; b < 50; ++b) irq(30, SCENE_FX);
    if (n_added || cuts || fm1_output_fx_shed_mask()) return 132;
    /* ... three of them in a row do not either (the cost moves 1/8 per block) */
    ucost[MFX_VERB] = 30000; irq(30, SCENE_FX); irq(30, SCENE_FX); irq(30, SCENE_FX); ucost[MFX_VERB] = 7000;
    for (unsigned b = 0; b < 50; ++b) irq(30, SCENE_FX);
    if (n_added || cuts || fm1_output_fx_shed_mask()) return 133;
    /* 13b: a cost that really went up is learned: REVERB 7 % -> 21 % for good
     * (63 -> 77 %) is shed, not in the first blocks, but within 60 (~170 ms). */
    scene(); events_clear();
    ucost[MFX_VERB] = 21000;
    { unsigned at = 0;
      for (unsigned b = 0; b < 60 && !at; ++b) { irq(30, SCENE_FX); if (n_added) at = b + 1U; }
      if (at < 8 || added[0] != MFX_VERB || cuts) return 134; }
    /* back to the scene's costs: REVERB returns at a low load and is measured again */
    ucost[MFX_VERB] = 7000;
    for (unsigned b = 0; b < 600; ++b) irq(5, SCENE_FX);
    if (fm1_output_fx_shed_mask() || !unit_in(MFX_VERB)) return 134;
    /* 13c: a lower cost counts at once. REVERB falls from 7 % to 1 % (measured
     * in one block), then the engine rises by 5 %: 10 + 45 + 17 = 72 %, no
     * shed. Learned slowly it would still count ~6 %: 77 % for many blocks. */
    scene(); events_clear();
    for (unsigned b = 0; b < 20; ++b) irq(40, SCENE_FX); /* 73 % */
    ucost[MFX_VERB] = 1000; irq(40, SCENE_FX);
    for (unsigned b = 0; b < 100; ++b) irq(45, SCENE_FX);
    if (n_added || cuts || fm1_output_fx_shed_mask()) return 135;
    /* the same for the rest of mfx_process (3 % -> 1 %) */
    scene(); events_clear();
    for (unsigned b = 0; b < 20; ++b) irq(40, SCENE_FX);
    irq(40, 1);
    for (unsigned b = 0; b < 100; ++b) irq(43, 1); /* 10 + 43 + 20 + 1 = 74 %; with 3 %: 76 % */
    if (n_added || cuts || fm1_output_fx_shed_mask()) return 136;

    /* 14 (audit A1): diagnostics of command 23 version 2. 141 = what the
     * guard learned, 142 = guard_now_peak, 143 = lone_spikes, 144 = the
     * panel interrupt's peak. */
    { struct fm1_output_load l;
      scene(); md_cost = 0;
      for (unsigned b = 0; b < 50; ++b) irq(2, SCENE_FX);
      fm1_output_load_read(&l);
      /* each unit's own cost, the rest of mfx_process with units on, and a clearing were learned */
      if (l.unit_cost[MFX_FILTER] != 7000U || l.unit_cost[MFX_VERB] != 7000U || l.unit_cost[MFX_CHORUS] != 3000U ||
          l.unit_cost[MFX_EQ] != 2000U || l.unit_cost[MFX_U_COMP] != 1000U || l.base_float != SCENE_FX * 1000U) return 141;
      if (!l.clear_cost || l.clear_cost > 6000U) return 141; /* a 4 KB slice = 4 % in this scene */
      /* reading changes nothing the guard uses */
      { struct fm1_output_load m; fm1_output_load_read(&m);
        for (unsigned u = 0; u < MFX_UNITS; ++u) if (m.unit_cost[u] != l.unit_cost[u]) return 141;
        if (m.base_float != l.base_float || m.base_dry != l.base_dry || m.clear_cost != l.clear_cost) return 141;
        if (m.guard_now_peak) return 142; } /* no block since the read before */
      /* guard_now_peak = the longest block start -> guard (engine + MICRO D), held from its own block */
      md_cost = 4U * (BLOCK / 100U);
      irq(2, SCENE_FX); irq(31, SCENE_FX); irq(5, SCENE_FX);
      fm1_output_load_read(&l);
      if (l.guard_now_peak != 35000U) return 142;
      irq(7, SCENE_FX);
      fm1_output_load_read(&l); /* lower than before: the peak restarted at the read */
      if (l.guard_now_peak != 11000U) return 142;
      md_cost = 0;
      /* lone_spikes: one block over 75 % between two blocks under it */
      uint32_t lone = l.lone_spikes, sheds = fm1_output_fx_shed_count();
      irq(57, SCENE_FX);
      fm1_output_load_read(&l);
      if (l.lone_spikes != lone) return 143; /* not known before the next block */
      irq(2, SCENE_FX);
      fm1_output_load_read(&l);
      if (l.lone_spikes != lone + 1U) return 143;
      fm1_output_load_read(&l);
      if (l.lone_spikes != lone + 1U) return 143; /* a count since start: a read does not clear it */
      irq(57, SCENE_FX); irq(57, SCENE_FX); irq(2, SCENE_FX); irq(2, SCENE_FX); /* two in a row: not alone */
      fm1_output_load_read(&l);
      if (l.lone_spikes != lone + 1U) return 143;
      for (unsigned b = 0; b < 10; ++b) { irq(57, SCENE_FX); irq(2, SCENE_FX); }
      fm1_output_load_read(&l);
      if (l.lone_spikes != lone + 11U || fm1_output_fx_shed_count() != sheds) return 143;
      irq(57, SCENE_FX); irq(57, SCENE_FX); irq(57, SCENE_FX); /* a real overload: shed, no spike counted */
      for (unsigned b = 0; b < 20; ++b) irq(2, SCENE_FX);
      fm1_output_load_read(&l);
      if (l.lone_spikes != lone + 11U || fm1_output_fx_shed_count() != sheds + 1U) return 143;
      /* the panel interrupt's peak (written by board/device.c) is handed over and cleared */
      fm1_panel_irq_peak = 1234U;
      fm1_output_load_read(&l);
      if (l.panel_irq_peak != 1234U || fm1_panel_irq_peak) return 144;
      fm1_output_load_read(&l);
      if (l.panel_irq_peak) return 144; }

    /* MICRO D was rendered once in every block at its full cost */
    if (md_wrong) return 11;
    return 0;
}
