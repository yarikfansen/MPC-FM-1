/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_BLACKBOX_H
#define FM1_BLACKBOX_H
#include <stdint.h>

/* Black box and load log (user 10.11: reboots on MASTER FX MIX with no known
 * cause).
 *
 * The record lives in a few bytes of RAM above the heap that the start-up
 * code neither clears nor loads, so after a restart WITHOUT a power cut
 * (crash, watchdog, reset) the next start can still read what the last
 * session was doing: page, last control, audio load, dropped blocks, time
 * alive. After a real power-off the bytes are garbage, the checksum fails and
 * the start counts as a cold one. Nothing here writes Flash, so it can run
 * while sound plays. Control task only.
 *
 * The load log is a short list of the latest overload events of this session
 * (RAM only). */
enum { BB_NO_CONTROL = 255,
       BB_FLAG_SAFE = 1,    /* the session ran in SAFE mode */
       BB_FLAG_PLAYING = 2, /* the sequencer was running */
       BB_FLAG_USB = 4 };   /* USB was connected (bus power) */
struct bb_record {
    uint32_t magic;
    uint32_t uptime_ms;    /* time since that start */
    uint32_t control_ms;   /* uptime when the last control moved */
    uint16_t restarts;     /* restarts in a row without a power cut, this session included */
    uint16_t overruns;     /* audio blocks dropped in that session (stops at 65535) */
    uint16_t sheds;        /* load guard trips, "CPU FULL" */
    uint8_t page, control; /* page open; last control: 0..6 encoder, 16..29 button, BB_NO_CONTROL */
    uint8_t cpu, cpu_peak; /* audio load at the last update and its highest value, percent (stops at 255) */
    uint8_t flags, version;
    uint32_t crc;
};
_Static_assert(sizeof(struct bb_record) == 28, "black box record layout");

/* Kinds of load log entries. */
enum { BB_EVENT_DROP = 1, /* audio blocks were dropped */
       BB_EVENT_SHED,     /* the load guard bypassed the master FX */
       BB_EVENT_HIGH };   /* load went over BB_HIGH_PERCENT */
enum { BB_EVENTS = 8, BB_HIGH_PERCENT = 85, BB_UPDATE_MS = 100 };
struct bb_event { uint32_t uptime_ms; uint8_t kind, page, control, cpu; uint16_t count; };

/* First call of a start. safe: SAFE mode (bb_poll's flags replace it later). Returns 1 when the previous session
 * ended without a power cut and its record was readable. */
unsigned bb_start(uint32_t now_ms, unsigned safe);
/* The previous session's record, or 0 after a cold start. */
const struct bb_record *bb_last(void);
/* A control moved: 0..6 encoder, 16 + n button. */
void bb_control(unsigned control, uint32_t now_ms);
/* Every control poll; the record is rewritten every BB_UPDATE_MS.
 * overruns and sheds are the running totals since power-on. */
void bb_poll(uint32_t now_ms, unsigned page, unsigned cpu_percent, uint32_t overruns, uint32_t sheds, unsigned flags);
/* Load log, newest first: entry 0..BB_EVENTS-1, 0 when there is none. */
const struct bb_event *bb_event(unsigned index);

/* USB readout (transfer command 24, read-only, control task). part 0: header
 * + this session's record + the previous session's record; part 1: load log
 * entries 0..3, part 2: entries 4..7 (newest first). Fills BB_READ_BYTES
 * bytes, little-endian, and returns BB_READ_BYTES; an unknown part returns 0.
 *   bytes 0..3 (every part): BB_READ_VERSION, part, have (bit 0: this
 *     session's record is running, bit 1: a previous record was readable),
 *     number of load log entries 0..8.
 *   part 0: 4..23 this session, 24..43 previous session (zero if none), each:
 *     uptime_ms u32, control_ms u32, restarts u16, overruns u16, sheds u16,
 *     page, control, cpu, cpu_peak, flags, version (u8 each) = 20 bytes.
 *   part 1 / 2: 4 entries of 10 bytes from byte 4: uptime_ms u32, kind,
 *     page, control, cpu (u8 each), count u16; kind 0 = no entry. */
enum { BB_READ_VERSION = 1, BB_READ_BYTES = 44, BB_READ_PARTS = 3 };
unsigned bb_read(unsigned part, uint8_t out[BB_READ_BYTES]);

#ifdef FM1_HOST_TEST
struct bb_record *bb_test_ram(void);
void bb_test_reset(void); /* module state as after a restart; the record bytes stay */
#endif
#endif
