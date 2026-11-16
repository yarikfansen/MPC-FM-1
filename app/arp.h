/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_ARP_H
#define FM1_ARP_H
#include <stdint.h>
/* App-side arpeggiator, run from the control task (~10 ms ticks).
 * Modes: UP, DOWN, UP-DOWN inclusive (ends repeat), UP-DOWN exclusive,
 * RANDOM, AS PLAYED. Humanize shifts and softens off-beat notes only:
 * the first note of the pattern (accent) and on-beat notes stay exact.
 * v13: MUTATE replaces a share of the notes (VAR picks how), CHANCE plays the cycle through a
 * polyrhythm pattern of 3, 4, 5 or 7 steps. */
/* ARP_PARAMS = the six stored in the project's extras (7 bytes a track with the on flag);
 * the three after them (v13 randomisation page) live only in RAM and start at 0 with every project. */
enum { ARP_RATE, ARP_MODE, ARP_OCT, ARP_GATE, ARP_HUMAN, ARP_HOLD, ARP_PARAMS,
       ARP_MUTATE = ARP_PARAMS, ARP_VAR, ARP_CHANCE, ARP_ALL };
struct arp_event { uint8_t status, note, velocity; };
/* The sequencer's swing (0..50): off-beat arpeggio notes start that share of a step late. Call before arp_tick. */
void arp_swing_set(unsigned percent);
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
/* v13 CHANCE variants: polyrhythm patterns of 3, 4, 5 and 7 steps (indices into the note cycle).
 * Returns the length of variant 0..3 and copies its indices to out (7 at most). */
unsigned arp_poly_pattern(unsigned variant, uint8_t out[7]);
/* Page rules of the ARP button (user 10.11, F1 / F2), pure so the host test can check them.
 * The ARP press that opens the page: the new LATCH. An arp that was off starts with LATCH off;
 * an arp already on keeps the LATCH it has. */
unsigned arp_press_latch(unsigned arp_was_on, unsigned latch);
/* User 11.11: leaving the ARP page no longer stops the arp (the F1 leave rule is gone); it goes off only by
 * holding ARP, by K4 on the ARP page and when REC ends (F2). */
/* Hold ARP (600 ms): the arp's new state = the opposite of its state before the press. */
unsigned arp_hold_toggle(unsigned arp_was_on);
/* REC on with the sequencer stopped and our arp feeding this track: 1 = pause the engine's step recording
 * (the arp only sounds, nothing is written); it goes on when the sequencer plays (playing = playing now or
 * PLAY starts it in this tick). */
unsigned arp_rec_pause(unsigned writing, unsigned playing, unsigned arp_feeds);
/* SEQ grid (REC off): 1 = a key writes its note into the cursor step (today's grid); 0 = the key only feeds
 * the arp (our arp on, melodic track). */
unsigned arp_grid_writes(unsigned arp_on, unsigned pads);
/* While REC is on, PLAY starting the sequencer does not restart the arp's note cycle: it goes on from the
 * next note it was at (the grid timing still restarts with PLAY). Off = today's restart from the first note. */
void arp_keep_note(unsigned on);
/* REC follow, once per control tick: writing = REC now, arp_mask = tracks whose arp is on now.
 * Returns the tracks whose arp must go off because REC just ended (1 -> 0) and that track's arp
 * was on at some point of this take (0 otherwise). Keeps the take's state between calls. */
unsigned arp_rec_follow(unsigned writing, unsigned arp_mask);
/* Preview of the pattern for drawing: up to max notes (MIDI), returns count. */
unsigned arp_pattern(unsigned track, const uint8_t *held, unsigned count, uint8_t *notes, unsigned max);
#endif
