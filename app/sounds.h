/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_SOUNDS_H
#define FM1_SOUNDS_H
#include <stdint.h>
/* Our own sound library, baked into the firmware, one list per engine.
 * Control task only. */
unsigned sounds_count(unsigned engine);           /* 0 = engine has no library */
int sounds_step(unsigned track, int delta, uint32_t now); /* choose next/previous; 0 ok (loads in sounds_tick) */
int sounds_tick(uint32_t now);                    /* call every control tick; 1 = a sound was loaded */
int sounds_apply(unsigned track, unsigned index); /* load sound index of the track's engine */
const char *sounds_name(unsigned track);          /* NULL when the track holds no library sound */
void sounds_forget(unsigned track);
unsigned sounds_id(unsigned track);               /* changes whenever the shown name changes */
void sounds_state_get(uint8_t out[8]);
void sounds_state_put(const uint8_t in[8]);
#endif
