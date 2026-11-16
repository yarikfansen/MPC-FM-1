/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_UI_METER_H
#define FM1_UI_METER_H
#include <stdint.h>

/* F8 measurement hook (user 10.11: tearing / slow screen): how long the screen
 * work of the control task takes, read over USB (transfer command 25). Only
 * measures; changes nothing on the screen. Control task only.
 *   UI_DRAW    one draw() call: the frame's damage pass (draw_page into the
 *              saved cache), once per control poll when something changed;
 *   UI_COMPOSE one paint_compose() call: one band of 16 rows (draw_page clipped
 *              to the band + the theme pass);
 *   UI_FLUSH   one paint_flush() call: the bands of one poll, composes and DMA
 *              waits included.
 * Times are in the audio timer's ticks (the clock of commands 19 / 23) with
 * one wrap handled, plus whole milliseconds (fm1_millis) as a cross-check. */
enum { UI_DRAW, UI_COMPOSE, UI_FLUSH, UI_KINDS };
void ui_meter_add(unsigned kind, uint32_t ticks, uint32_t ms);

/* Reply of command 25 (little-endian), then the figures start again:
 *   0 UI_METER_VERSION (1); 1..4 ticks of one audio block (128 frames at
 *   44 118 Hz); 5..8 the audio timer's period register (0 in host builds:
 *   a time longer than the period reads short);
 *   then for draw, compose, flush (14 bytes each, from 9, 23, 37): calls u32,
 *   max ticks u32, average ticks u32 (rounded down), max ms u16 (stops at
 *   65535). A kind with no call since the last read is all zero. */
enum { UI_METER_VERSION = 1, UI_METER_BYTES = 51 };
unsigned ui_meter_read(uint8_t out[UI_METER_BYTES], uint32_t block_ticks, uint32_t timer_period);
#endif
