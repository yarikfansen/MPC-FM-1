/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_AUDIO_OUTPUT_H
#define FM1_AUDIO_OUTPUT_H
#include "fm1.h"
/* Reset before start or while the audio ISR is excluded. */
void fm1_output_reset(void);
/* Finish one synthesized block: master gain, USB, Bluetooth, USB recording.
 * Audio ISR only; output holds FM1_BLOCK_WORDS Q23 samples. */
FM1_AUDIO_CODE void fm1_output_mix(int32_t *output);
/* Control task: ticks our mix took in the last block, and the ticks of one
 * block in the same clock (that of fm1_audio_stats; 0 in host builds). */
uint32_t fm1_output_mix_ticks(void);
uint32_t fm1_output_block_ticks(void);
/* Control task timing (F8 hook, ui_meter.h): the audio timer now, ticks since `start` (one wrap handled, as
 * for the load figures), and the timer's period register (0 in host builds). Not audio code. */
uint32_t fm1_output_timer(void);
uint32_t fm1_output_timer_since(uint32_t start);
uint32_t fm1_output_timer_period(void);
/* Load guard. The board's audio ISR writes the tick its block started and
 * the ticks of one block. When the engine + MICRO D leave too little of the
 * block for the master FX, the guard takes effects out one by one (fade-out),
 * reverb first and FILTER last, and brings them back in reverse order once
 * the load has been low for a while (CPU FULL), instead of overrunning the
 * interrupt. MICRO D is never thinned. Control task: how many effects it
 * took out since start, and the units out now (master_fx.h, bit = unit). */
extern volatile uint32_t fm1_mix_block_start, fm1_mix_block_ticks;
uint32_t fm1_output_fx_shed_count(void);
unsigned fm1_output_fx_shed_mask(void);
/* Control task: the highest whole-interrupt time (block start to the end of
 * fm1_output_mix) of the last 172..344 blocks (~0.5..1 s), in the ticks of
 * fm1_output_block_ticks. */
uint32_t fm1_output_peak_ticks(void);
/* Control task (transfer command 23): effects the load guard took out since
 * start (one count per effect), whether any is out now, the effects' last
 * honest cost (whole mfx_process), the ticks of one block,
 * and three peaks since the last read in the same clock: the engine call
 * (block start to fm1_output_mix), MICRO D render, our whole mix part. Each
 * read clears the peaks (interrupts held, so no block is lost). */
enum { FM1_OUTPUT_LOAD_UNITS = 9 }; /* = MFX_UNITS (checked in audio_output.c) */
struct fm1_output_load {
    uint32_t shed_count, fx_ticks, block_ticks, engine_peak, micro_d_peak, mix_peak;
    unsigned shedding;
    /* Reply version 2 (audit A1), same clock. What the guard has learned
     * (0 = not measured yet): each master FX unit's own cost (index = unit
     * of master_fx.h), the rest of mfx_process with / without a unit running,
     * the dearest memory clearing. guard_now_peak = the longest block start ->
     * guard() (engine call + MICRO D + whatever preempted them), since the
     * last read. lone_spikes = blocks over the guard's 75 % shed level with
     * the block before and the block after under it (preemption, not load),
     * since start. panel_irq_peak = the longest single run of our panel scan
     * interrupt (board/device.c, wall time), since the last read. */
    uint32_t unit_cost[FM1_OUTPUT_LOAD_UNITS], base_float, base_dry, clear_cost;
    uint32_t guard_now_peak, lone_spikes, panel_irq_peak;
};
/* Written by the panel scan interrupt (compare + store), read and cleared by
 * fm1_output_load_read with interrupts held. */
extern volatile uint32_t fm1_panel_irq_peak;
void fm1_output_load_read(struct fm1_output_load *out);
#ifdef FM1_HOST_TEST
/* Host tests: the audio clock, the ticks one mfx_process call takes, and
 * how many times it ran (see audio_output.c). */
extern uint32_t fm1_test_timer, fm1_test_mfx_cost, fm1_test_mfx_runs;
#endif
/* Flash erase / write for the control task. The audio ISR skips MICRO D (a
 * few ms of silence on its tracks) while Flash is busy. */
int flash_erase_safe(uint32_t offset);
int flash_write_safe(uint32_t offset, const void *data, size_t bytes);
#endif
