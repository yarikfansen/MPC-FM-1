/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "audio_output.h"
#include "extras.h"
#include "instrument.h"
#include "micro_d.h"
#include "binaural.h"
#include "lang.h"
#include "keyscale.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* One sector (app/flash_layout.h): 16 records of 256 bytes (magic, slot, payload, CRC).
 * Version 1 ('FMX1') held 120 bytes; version 2 ('FMX2') adds LFO 2, CHAIN
 * repeats and SEQ variations; version 3 ('FMX3') the MICRO D tracks; 4 the
 * screen language; 5 ('FMX5') BINAURAL; 6 ('FMX6') the smooth pan per track.
 * Old records still load (new parts = defaults).
 * Power loss: a save first writes the whole new sector to the shared staging
 * sector (project.c) as a journal, then rewrites this sector, then marks the
 * journal done. A complete journal that is not done is authoritative: loads
 * read it, and the next write that reuses staging finishes the copy first
 * (extras_recover via groove_storage_prepare). So every slot keeps either its
 * old or its new extras whenever the power goes. */
enum { EXTRAS_OFFSET = GROOVE_EXTRAS_OFFSET, RECORD = EXTRAS_RECORD, MAGIC_V1 = 0x31584d46U, MAGIC_V2 = 0x32584d46U,
       MAGIC_V3 = 0x33584d46U, MAGIC_V4 = 0x34584d46U /* 'FMX4': + screen language */,
       MAGIC_V5 = 0x35584d46U /* 'FMX5': + BINAURAL */, MAGIC = 0x36584d46U /* 'FMX6': + pan 0..127 */,
       JOURNAL_MAGIC = 0x4a584d46U /* 'FMXJ' */, JOURNAL = GROOVE_PROJECT_SLOTS * FM1_FLASH_SECTOR_BYTES,
       PACKED = RECORD - 4 /* a journal record leaves out the slot field */ };
struct journal { uint32_t magic, crc, done; };
enum { JOURNAL_BODY = GROOVE_PROJECT_SLOTS * PACKED };
_Static_assert(176 + MD_STATE_BYTES + 1 == EXTRAS_BYTES_V4, "extras layout");
_Static_assert(EXTRAS_BYTES_V4 + BIN_STATE_BYTES + 1 == EXTRAS_BYTES_V5, "extras layout v5 + PLAY LED byte");
_Static_assert(EXTRAS_BYTES_V5 + 8 == EXTRAS_BYTES, "extras layout v6: pan per track");
_Static_assert(4 + 4 + EXTRAS_BYTES + 4 <= RECORD, "extras record");
_Static_assert(RECORD * GROOVE_PROJECT_SLOTS <= FM1_FLASH_SECTOR_BYTES, "extras sector");
_Static_assert(sizeof(struct journal) + JOURNAL_BODY <= FM1_FLASH_SECTOR_BYTES, "extras journal fits staging");
_Static_assert(EXTRAS_OFFSET >= GROOVE_SAMPLE_OFFSET + GROOVE_SAMPLE_LIMIT, "extras overlap the sample bank");
struct record { uint32_t magic, slot; uint8_t data[EXTRAS_BYTES]; uint32_t crc; };
_Static_assert(offsetof(struct record, crc) == ((8U + EXTRAS_BYTES + 3U) & ~3U), "extras CRC place");
_Static_assert(sizeof(struct record) == RECORD, "extras record size");
int groove_flash_tail_blocked(void); /* project.c: an old oversized sample bank */

static void collect(uint8_t *d)
{
    memset(d, 0, EXTRAS_BYTES);
    mfx_state_get(d);            /* 0..31 */
    lfo_state_get(d + 32);       /* 32..35 */
    arp_state_get(d + 36);       /* 36..91 */
    groove_screen_extras_get(d + 92); /* 92..117 */
    lfo2_state_get(d + 118);     /* 118..121 */
    groove_screen_extras2_get(d + 122); /* 122..175 (170..171: pan side R / L for older firmware) */
    d[172] = (uint8_t)key_state_get(); /* 172: GLOBAL KEY | MODE << 4, read back at power on (spare byte, 0xff before) */
    d[173] = 10; /* 173: was GLOBAL DISPLAY BRIGHT (removed, F11): stays 10 = full for older firmware; never read back */
    micro_d_state_get(d + 176);  /* 176..221 */
    unsigned peak = groove_screen_peak() / 4U; /* bits 3-7: audio peak in 4 % steps, read back as LAST RUN */
    d[222] = (uint8_t)(lang_get() | ui_theme_get() << 1 | (peak > 30U ? 30U : peak) << 3); /* 222: language (bit 0) and theme (bits 1-2), read back at power on */
    binaural_state_get(d + 223); /* 223..234 */
    d[235] = (uint8_t)groove_screen_play_led_get(); /* 235: green PLAY LED index + 1 */
    groove_screen_pan_get(d + 236); /* 236..243: pan 0..127 per track */
}
static void spread(const uint8_t *d)
{
    mfx_state_put(d);
    lfo_state_put(d + 32);
    arp_state_put(d + 36);
    groove_screen_extras_put(d + 92);
    lfo2_state_put(d + 118);
    groove_screen_extras2_put(d + 122);
    groove_screen_pan_put(d + 170, d + 236);
    micro_d_state_put(d + 176);
    binaural_state_put(d + 223);
    groove_screen_play_led_put(d[235]);
}
void extras_defaults(void)
{
    uint8_t d[EXTRAS_BYTES]; memset(d, 0xff, sizeof d); /* 0xff = keep module defaults */
    spread(d);
}
/* Payload bytes of a valid record of this slot, 0 if missing or damaged. The
 * CRC follows the data at the next 4-byte boundary (struct record padding)
 * and covers everything before it, padding included. */
static unsigned record_bytes(const uint8_t *raw, unsigned slot)
{
    uint32_t magic, at, crc;
    memcpy(&magic, raw, 4); memcpy(&at, raw + 4, 4);
    unsigned n = magic == MAGIC ? EXTRAS_BYTES : magic == MAGIC_V5 ? EXTRAS_BYTES_V5 : magic == MAGIC_V4 ? EXTRAS_BYTES_V4
             : magic == MAGIC_V3 ? EXTRAS_BYTES_V3 : magic == MAGIC_V2 ? EXTRAS_BYTES_V2 : magic == MAGIC_V1 ? EXTRAS_BYTES_V1 : 0;
    if (!n || at != slot) return 0;
    unsigned m = (8U + n + 3U) & ~3U;
    memcpy(&crc, raw + m, 4);
    return crc == groove_crc32(~0U, raw, m) ? n : 0;
}
/* 1 = a complete journal that is not done yet, 0 = none, < 0 = read error. */
static int journal_active(void)
{
    struct journal j;
    int r = fm1_flash_read(JOURNAL, &j, sizeof j);
    if (r || j.magic != JOURNAL_MAGIC || j.done != ~0U) return r;
    uint8_t chunk[64];
    uint32_t crc = ~0U;
    for (unsigned at = 0; at < JOURNAL_BODY; at += sizeof chunk) {
        unsigned n = JOURNAL_BODY - at < sizeof chunk ? JOURNAL_BODY - at : sizeof chunk;
        r = fm1_flash_read(JOURNAL + sizeof j + at, chunk, n);
        if (r) return r;
        crc = groove_crc32(crc, chunk, n);
    }
    return ~crc == j.crc;
}
/* A journal record is the stored one without its slot field. */
static void unpack(uint8_t *raw, const uint8_t *packed, unsigned slot)
{
    uint32_t magic, at;
    memcpy(&magic, packed, 4);
    at = magic == ~0U ? ~0U : slot; /* an empty record stays all 0xff */
    memcpy(raw, packed, 4); memcpy(raw + 4, &at, 4); memcpy(raw + 8, packed + 4, PACKED - 4);
}
static int read_journal(uint8_t *image)
{
    uint8_t packed[PACKED];
    for (unsigned i = 0; i < GROOVE_PROJECT_SLOTS; ++i) {
        int r = fm1_flash_read(JOURNAL + sizeof(struct journal) + i * PACKED, packed, PACKED);
        if (r) return r;
        unpack(image + i * RECORD, packed, i);
    }
    return 0;
}
static int sector_matches(const uint8_t *image)
{
    uint8_t chunk[64];
    for (unsigned at = 0; at < FM1_FLASH_SECTOR_BYTES; at += sizeof chunk) {
        int r = fm1_flash_read(EXTRAS_OFFSET + at, chunk, sizeof chunk);
        if (r) return r;
        if (memcmp(chunk, image + at, sizeof chunk)) return 0;
    }
    return 1;
}
/* Rewrite the extras sector from image (journal already complete), verify,
 * then retire the journal. */
static int copy_out(const uint8_t *image)
{
    int r = sector_matches(image);
    if (r == 0) {
        r = flash_erase_safe(EXTRAS_OFFSET);
        if (!r) r = flash_write_safe(EXTRAS_OFFSET, image, FM1_FLASH_SECTOR_BYTES);
        if (!r) r = sector_matches(image);
        if (r == 0) return FM1_FLASH_IO;
    }
    if (r < 0) return r;
    uint32_t done = 0;
    return flash_write_safe(JOURNAL + offsetof(struct journal, done), &done, sizeof done);
}
int extras_recover(void)
{
    int r = journal_active();
    if (r <= 0) return r;
    uint8_t *image = malloc(FM1_FLASH_SECTOR_BYTES);
    if (!image) return GROOVE_PROJECT_MEMORY;
    r = read_journal(image);
    if (!r) r = copy_out(image);
    free(image);
    return r;
}
int extras_read(unsigned slot, uint8_t raw[EXTRAS_RECORD])
{
    if (slot >= GROOVE_PROJECT_SLOTS) return -1;
    int r = journal_active();
    if (r > 0) {
        uint8_t packed[PACKED];
        r = fm1_flash_read(JOURNAL + sizeof(struct journal) + slot * PACKED, packed, PACKED);
        if (!r) unpack(raw, packed, slot);
    } else if (!r) r = fm1_flash_read(EXTRAS_OFFSET + slot * RECORD, raw, RECORD);
    if (r) return r;
    return record_bytes(raw, slot) ? 0 : 1;
}
int extras_save(unsigned slot)
{
    if (slot >= GROOVE_PROJECT_SLOTS) return -1;
    /* A sample bank larger than the new limit would overlap this sector. */
    if (groove_flash_tail_blocked()) return -2;
    struct record rec; memset(&rec, 0, sizeof rec); rec.magic = MAGIC; rec.slot = slot; collect(rec.data);
    rec.crc = groove_crc32(~0U, &rec, offsetof(struct record, crc));
    uint8_t *image = malloc(2U * FM1_FLASH_SECTOR_BYTES), *journal = image + FM1_FLASH_SECTOR_BYTES;
    if (!image) return -3;
    /* Finish an interrupted copy first: the sector is then current. */
    int r = groove_storage_prepare();
    if (!r) r = fm1_flash_read(EXTRAS_OFFSET, image, FM1_FLASH_SECTOR_BYTES);
    if (!r && memcmp(image + slot * RECORD, &rec, RECORD)) {
        /* Damaged records would load as defaults anyway; store them empty. */
        for (unsigned i = 0; i < GROOVE_PROJECT_SLOTS; ++i)
            if (!record_bytes(image + i * RECORD, i)) memset(image + i * RECORD, 0xff, RECORD);
        memcpy(image + slot * RECORD, &rec, RECORD);
        struct journal j = {JOURNAL_MAGIC, 0, ~0U};
        memset(journal, 0xff, FM1_FLASH_SECTOR_BYTES);
        for (unsigned i = 0; i < GROOVE_PROJECT_SLOTS; ++i) {
            uint8_t *p = journal + sizeof j + i * PACKED;
            memcpy(p, image + i * RECORD, 4); memcpy(p + 4, image + i * RECORD + 8, PACKED - 4);
        }
        j.crc = ~groove_crc32(~0U, journal + sizeof j, JOURNAL_BODY);
        memcpy(journal, &j, sizeof j);
        /* Staging holds nothing pending now (groove_storage_prepare). */
        r = flash_erase_safe(JOURNAL);
        if (!r) r = flash_write_safe(JOURNAL, journal, sizeof j + JOURNAL_BODY);
        if (!r) { r = journal_active(); r = r > 0 ? 0 : r ? r : FM1_FLASH_IO; }
        if (!r) r = copy_out(image);
    }
    free(image);
    return r;
}
void extras_apply(unsigned slot, const uint8_t raw[EXTRAS_RECORD], unsigned voices_only)
{
    unsigned n = raw ? record_bytes(raw, slot) : 0;
    if (!n) { extras_defaults(); return; }
    uint8_t d[EXTRAS_BYTES]; memset(d, 0xff, sizeof d); /* 0xff = keep module defaults */
    memcpy(d, raw + 8, n);
    if (voices_only) { extras_defaults(); micro_d_state_put(d + 176); if (d[222] != 0xff) { lang_set(d[222] & 1U); ui_theme_set((d[222] >> 1) & 3U); groove_screen_last_peak_set((d[222] >> 3) * 4U); }
        groove_screen_play_led_put(d[235]); groove_screen_pan_put(d + 170, d + 236);
        key_state_put(d[172]); } /* d[173] (old BRIGHT) is not applied any more (F11) */
    else spread(d);
}
static int load(unsigned slot, unsigned voices_only)
{
    uint8_t raw[RECORD];
    int r = extras_read(slot, raw);
    extras_apply(slot, r ? 0 : raw, voices_only);
    return r;
}
int extras_load(unsigned slot) { return load(slot, 0); }
/* Safe start: effects, LFO and arp stay at defaults, but a MICRO D track
 * keeps its voice (its host is stored silent in the project) and every
 * track its pan. */
int extras_load_voices(unsigned slot) { return load(slot, 1); }
