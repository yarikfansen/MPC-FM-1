/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_ARP_H
#define FM1_ARP_H
#include <stdint.h>
/* App-side arpeggiator, run from the control task (~10 ms ticks).
 * Modes: UP, DOWN, UP-DOWN inclusive (ends repeat), UP-DOWN exclusive,
 * RANDOM, AS PLAYED. Humanize shifts and softens off-beat notes only:
 * the first note of the pattern (accent) and on-beat notes stay exact. */
enum { ARP_RATE, ARP_MODE, ARP_OCT, ARP_GATE, ARP_HUMAN, ARP_HOLD, ARP_PARAMS };
struct arp_event { uint8_t status, note, velocity; };
unsigned arp_enabled(unsigned track);
void arp_set(unsigned track, unsigned on);
unsigned arp_get(unsigned track, unsigned param);
void arp_edit(unsigned track, unsigned param, int delta);
void arp_value_name(unsigned track, unsigned param, char out[12]);
const char *arp_param_name(unsigned param);
/* held: notes currently held for the track, in press order. Returns events to send. */
unsigned arp_tick(unsigned track, unsigned channel, uint32_t now_ms, unsigned tempo, unsigned playing,
                  const uint8_t *held, unsigned count, struct arp_event *out, unsigned max);
/* Stop sounding notes (track change, disable). */
unsigned arp_stop(unsigned track, unsigned channel, struct arp_event *out, unsigned max);
/* Preview of the pattern for drawing: up to max notes (MIDI), returns count. */
unsigned arp_pattern(unsigned track, const uint8_t *held, unsigned count, uint8_t *notes, unsigned max);
#endif
