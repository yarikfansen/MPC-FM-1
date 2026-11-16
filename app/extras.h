/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_EXTRAS_H
#define FM1_EXTRAS_H
#include <stdint.h>
/* Settings the stock project format has no room for (master effects, output
 * boost, LFO, our arpeggiator, CHAIN, library sound per track, pan) are kept per
 * pattern slot in one extra Flash sector (GROOVE_EXTRAS_OFFSET, app/flash_layout.h),
 * journaled through the shared staging sector (app/extras.c).
 * Control task only. A missing or damaged record simply means defaults. */
enum { EXTRAS_BYTES = 244, EXTRAS_BYTES_V5 = 236, EXTRAS_BYTES_V4 = 223, EXTRAS_BYTES_V3 = 222, EXTRAS_BYTES_V2 = 176,
       EXTRAS_BYTES_V1 = 120, EXTRAS_RECORD = 256 };
int extras_save(unsigned slot); /* writes Flash only when the slot's record changed */
int extras_load(unsigned slot);
int extras_load_voices(unsigned slot); /* defaults, except the MICRO D tracks and pan */
void extras_defaults(void);
/* Split load (SEQ CHAIN preload): read 0 = valid, 1 = none (defaults), < 0 = Flash error;
 * apply with raw = 0 sets defaults. */
int extras_read(unsigned slot, uint8_t raw[EXTRAS_RECORD]);
void extras_apply(unsigned slot, const uint8_t raw[EXTRAS_RECORD], unsigned voices_only);
/* Finish an interrupted extras save (called by groove_storage_prepare). */
int extras_recover(void);
/* Pieces collected from each module (fixed sizes inside EXTRAS_BYTES). */
void mfx_state_get(uint8_t out[32]);
void mfx_state_put(const uint8_t in[32]);
void lfo_state_get(uint8_t out[4]);
void lfo_state_put(const uint8_t in[4]);
void lfo2_state_get(uint8_t out[4]);
void lfo2_state_put(const uint8_t in[4]);
void arp_state_get(uint8_t out[56]);
void arp_state_put(const uint8_t in[56]);
void groove_screen_extras_get(uint8_t out[26]);
void groove_screen_extras_put(const uint8_t in[26]);
/* v7 additions (record version 2): CHAIN repeats, SEQ variations, spare. */
void groove_screen_extras2_get(uint8_t out[54]);
void groove_screen_extras2_put(const uint8_t in[54]);
/* v11: the audio peak (% of the block deadline) kept with the saved project,
 * shown as LAST RUN after the next power on. */
unsigned groove_screen_peak(void);
/* Green PLAY LED position (matrix index 0..43), stored +1; 0 / 0xff = none. Byte 235 (old records read it as 0 = none). */
unsigned groove_screen_play_led_get(void);
void groove_screen_play_led_put(unsigned stored);
void groove_screen_last_peak_set(unsigned percent);
/* v6: pan 0..127 per track (64 centre), sent as CC 10. side = the old R / L
 * field (two bits per track, 1 right, 2 left); value 0xff = take the side. */
void groove_screen_pan_get(uint8_t value[8]);
void groove_screen_pan_put(const uint8_t side[2], const uint8_t value[8]);

/* project.c: Flash writes silence the whole output for a moment, so deferred
 * writes (settings, autosave) wait until the transport is stopped and nothing
 * has sounded for a while. Control task. */
unsigned groove_storage_quiet(void);
/* project.c: touch = a key, button, knob or MIDI note now; rest = 20 s since
 * the last touch and quiet. Autosave and the settings save wait for rest. */
void groove_storage_touch(void);
unsigned groove_storage_rest(void);
/* project.c, SEQ CHAIN: read a slot (project, tuning, extras) into RAM during
 * the current loop, so loading it at the boundary reads no Flash. Saving that
 * slot drops the copy; drop frees it. */
int groove_project_preload(unsigned slot);
void groove_project_preload_drop(void);
#endif
