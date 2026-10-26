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
enum { REC_RATE = 32000, REC_MAX_FRAMES = 48000 /* 1.5 s */ };
int rec_arm(unsigned source, int threshold_db, unsigned frames); /* 0, or -1 no memory */
void rec_stop(void);           /* RUN -> DONE with what is there; ARMED -> IDLE */
void rec_free(void);
unsigned rec_state(void);
unsigned rec_frames(void);     /* recorded so far */
unsigned rec_limit(void);      /* frames asked for */
unsigned rec_level(void);      /* input peak 0..127 (decays), for the meter */
int rec_sample(unsigned i);    /* frame i of the take, 16-bit */
unsigned rec_ram_frames(void); /* longest take the RAM allows now (0 = not even 0.25 s) */
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
 * encode them in place; out[] then points into the take's buffer. */
int rec_encode(const unsigned *starts, unsigned n, struct rec_slice *out);
/* samples.c: replace pads first..first+count-1 in the sample bank. begin:
 * 0, -2 bank full, else error. Then call step once per control tick: 1 = more,
 * 0 = done, < 0 = failed; done/total for a progress bar. items must stay
 * valid until the job ends. The sampler is silent while it runs. */
int groove_samples_replace_begin(unsigned first, unsigned count, const struct rec_slice *items);
int groove_samples_replace_step(unsigned *done, unsigned *total);
unsigned groove_samples_room(unsigned first, unsigned count); /* bytes for new data */
unsigned groove_samples_pad_frames(unsigned pad);
#endif
