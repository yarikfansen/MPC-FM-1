/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_RECORDER_H
#define FM1_RECORDER_H
#include "fm1.h"
/* Sampler recording (v7, agreed screens 21d/21e). The audio ISR hands every
 * block to rec_capture(); the control task arms, stops and reads the take.
 * The take is mono at 32 kHz (the sample bank's rate), one mu-law byte a
 * frame in one heap buffer, allocated on arming and freed by rec_free(). */
enum { REC_IDLE, REC_ARMED, REC_RUN, REC_DONE };
enum { REC_USB, REC_BT, REC_MIX, REC_SOURCES }; /* MIX = the synth's own output */
/* One take may fill the whole sample bank: (102 400 - 512 header) / 228 bytes
 * = 446 SEA5 chunks of 320 frames (4.46 s); samples.c checks this against the
 * layout. The heap decides the real length. Up to 1.5 s the rule is the one
 * RC1 had (REC_RESERVE_SHORT left free for USB / Bluetooth and the system);
 * a longer take must leave REC_RESERVE in one block, enough for what may
 * allocate while it is held (an engine created by a SEQ CHAIN switch: PLUCK
 * needs 33.3 KB; a settings save, 6.6 KB; USB / Bluetooth). */
enum { REC_RATE = 32000, REC_MIN_FRAMES = REC_RATE / 4, REC_MAX_FRAMES = 446 * 320,
       REC_SHORT_FRAMES = 48000, REC_RESERVE_SHORT = 12 * 1024, REC_RESERVE = 36 * 1024 };
int rec_arm(unsigned source, int threshold_db, unsigned frames); /* 0, or -1 no memory */
void rec_stop(void);           /* RUN -> DONE with what is there (the rest of the buffer is freed); ARMED -> IDLE */
void rec_free(void);
unsigned rec_state(void);
unsigned rec_frames(void);     /* recorded so far */
unsigned rec_limit(void);      /* frames asked for */
unsigned rec_level(void);      /* input peak 0..127 (decays), for the meter */
int rec_sample(unsigned i);    /* frame i of the take, 16-bit (0 once encoding has started) */
/* Longest take the RAM allows now (0 = not even 0.25 s). rec_ram_fit: from
 * the largest free heap block (read without allocating); rec_ram_frames: by
 * trying mallocs, for when the heap cannot be read. */
unsigned rec_ram_fit(unsigned largest_block);
unsigned rec_ram_frames(void);
/* Longest take a bank room of room_bytes holds when it may later be cut into
 * up to max_slices slices (each slice rounds up to whole 320-frame chunks). */
unsigned rec_bank_fit(unsigned room_bytes, unsigned max_slices);
/* The page sets these: listen = measure the chosen source's level; mute =
 * keep USB / Bluetooth out of the output (MONITOR OFF). */
void rec_listen(unsigned source, unsigned on, unsigned mute);
FM1_AUDIO_CODE unsigned rec_input_muted(void);
/* Audio ISR: usb / bt are this block's input (NULL when not read in full);
 * mix is the synth's own output before USB and Bluetooth are added. */
FM1_AUDIO_CODE void rec_capture(const int32_t *usb, const int16_t *bt, const int32_t *mix);
/* A finished sample for the bank: SEA5 data (228 bytes per 320 frames). */
struct rec_slice { const uint8_t *data; unsigned bytes, frames; char name[16]; };
/* Cut the take into n slices (start frames, ascending, starts[0] = 0) and
 * encode them in place, one chunk per step so the control task never stalls:
 * begin 0 / -1 bad cut; step 1 = more, 0 = done (out[] then points into the
 * take's buffer, shrunk to the encoded bytes), -1 = nothing to do. out[] must
 * stay valid until the last step. */
int rec_encode_begin(const unsigned *starts, unsigned n, struct rec_slice *out);
int rec_encode_step(unsigned *done, unsigned *total);
/* samples.c: replace pads first..first+count-1 in the sample bank. begin:
 * 0, -2 bank full, else error. Then call step once per control tick: 1 = more,
 * 0 = done, < 0 = failed; done/total for a progress bar. items must stay
 * valid until the job ends. The sampler is silent while it runs. */
int groove_samples_replace_begin(unsigned first, unsigned count, const struct rec_slice *items);
int groove_samples_replace_step(unsigned *done, unsigned *total);
unsigned groove_samples_room(unsigned first, unsigned count); /* bytes for new data */
unsigned groove_samples_pad_frames(unsigned pad);
#endif
