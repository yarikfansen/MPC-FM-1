/* SPDX-License-Identifier: MIT */
#ifndef FM1_SEA_REC_H
#define FM1_SEA_REC_H
#include <stdint.h>
/* SEA5 (the sample bank's codec): 320 mono 32 kHz frames -> 228 bytes.
 * The encoder state runs on from chunk to chunk of one sample. */
enum { SEA_REC_FRAMES = 320, SEA_REC_BYTES = 228 };
struct sea_rec { int32_t state[8]; unsigned previous; };
void sea_rec_start(struct sea_rec *e);
void sea_rec_chunk(struct sea_rec *e, const int16_t pcm[SEA_REC_FRAMES], uint8_t out[SEA_REC_BYTES]);
#endif
