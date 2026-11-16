/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_MICRO_D_H
#define FM1_MICRO_D_H
#include "fm1.h"
#include <stdint.h>
/* MICRO D: our paraphonic virtual analog (after the Behringer D Mini). Up to
 * four oscillators, one per note, share one ladder filter and one envelope.
 * A MICRO D track keeps a silent PSG host in the closed instrument (volume 0,
 * MIDI-only mode), so its sequencer, arpeggiator and recording work as for
 * any track; its notes arrive through groove_midi_note and are rendered here
 * into the mix before the master effects. At most MD_SLOTS tracks. */
enum { MD_SLOTS = 2, MD_PARAMS = 20, MD_PAGES = 5, MD_SLOT_BYTES = 23, MD_STATE_BYTES = MD_SLOTS * MD_SLOT_BYTES };
enum { MD_SAW, MD_SHARK, MD_SQR, MD_TRI, MD_DETUNE, MD_NOISE, MD_GLIDE, MD_VCA,
       MD_CUTOFF, MD_RESO, MD_CONTOUR, MD_DRIVE, MD_ATTACK, MD_DECAY, MD_SUSTAIN, MD_RETRIG,
       MD_RATE, MD_WAVE, MD_AMOUNT, MD_MODE };
enum { MD_MONO, MD_PARA2, MD_PARA3, MD_PARA4, MD_UNISON };
/* Control task unless noted. micro_d_note / micro_d_render run in the audio
 * interrupt from RAM (FM1_AUDIO_CODE, about 5.6 KB of the engines' heap). */
int micro_d_track(unsigned track);
unsigned micro_d_count(void);
int micro_d_attach(unsigned track); /* 0, or -1 when MD_SLOTS tracks are taken */
void micro_d_detach(unsigned track);
unsigned micro_d_value(unsigned track, unsigned param);
unsigned micro_d_max(unsigned param);
void micro_d_edit(unsigned track, unsigned param, int delta);
const char *micro_d_label(unsigned param);
void micro_d_text(unsigned track, unsigned param, char out[12]);
void micro_d_audible(unsigned track, unsigned audible); /* from the mixer's MUTE / SOLO */
void micro_d_pan(unsigned track, unsigned value); /* mixer pan 0..127: 64 centre, 0 left only, 127 right only */
unsigned micro_d_volume(unsigned track);
void micro_d_volume_edit(unsigned track, int delta);
unsigned micro_d_preset(unsigned track);
unsigned micro_d_preset_count(void);
const char *micro_d_preset_name(unsigned preset);
void micro_d_preset_load(unsigned track, unsigned preset);
unsigned micro_d_voices(unsigned track, uint8_t notes[4]); /* gated voices -> notes, returns a bit mask */
unsigned micro_d_level(unsigned track);                    /* 0..127 output meter */
void micro_d_live(unsigned track, unsigned note, unsigned velocity); /* keys while the ARP is off */
/* Control task, every poll: about every 250 ms a host volume that is not 0 any more is set back to 0. */
void micro_d_host_guard(uint32_t now_ms);
void micro_d_state_get(uint8_t out[MD_STATE_BYTES]);
void micro_d_state_put(const uint8_t in[MD_STATE_BYTES]); /* 0xff bytes = no MICRO D */
FM1_AUDIO_CODE int micro_d_owns(unsigned track);
/* Audio context (sequencer notes from the MIDI router): RAM code that only
 * queues the note; micro_d_render plays it. Safe while Flash is written. */
FM1_AUDIO_CODE void micro_d_note(unsigned track, unsigned note, unsigned velocity);
FM1_AUDIO_CODE void micro_d_render(int32_t *buffer); /* adds into FM1_BLOCK_FRAMES stereo Q23 */
#endif
