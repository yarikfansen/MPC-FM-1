/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_EXTRAS_H
#define FM1_EXTRAS_H
#include <stdint.h>
/* Settings the stock project format has no room for (master effects, output
 * boost, LFO, our arpeggiator, CHAIN, library sound per track) are kept per
 * pattern slot in one extra Flash sector (GROOVE_EXTRAS_OFFSET, app/flash_layout.h).
 * Control task only. A missing or damaged record simply means defaults. */
enum { EXTRAS_BYTES = 176, EXTRAS_BYTES_V1 = 120 };
int extras_save(unsigned slot);
int extras_load(unsigned slot);
void extras_defaults(void);
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
#endif
