/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "extras.h"
#include "instrument.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* One sector (app/flash_layout.h): 16 records of 256 bytes (magic, slot, payload, CRC). */
/* Version 1 ('FMX1') held 120 bytes; version 2 ('FMX2') adds LFO 2, CHAIN
 * repeats and SEQ variations. Old records still load (new parts = defaults). */
enum { EXTRAS_OFFSET = GROOVE_EXTRAS_OFFSET, RECORD = 256, MAGIC_V1 = 0x31584d46U, MAGIC = 0x32584d46U /* 'FMX2' */ };
_Static_assert(4 + 4 + EXTRAS_BYTES + 4 <= RECORD, "extras record");
_Static_assert(RECORD * GROOVE_PROJECT_SLOTS <= FM1_FLASH_SECTOR_BYTES, "extras sector");
_Static_assert(EXTRAS_OFFSET >= GROOVE_SAMPLE_OFFSET + GROOVE_SAMPLE_LIMIT, "extras overlap the sample bank");
struct record { uint32_t magic, slot; uint8_t data[EXTRAS_BYTES]; uint32_t crc; };
int groove_flash_tail_blocked(void); /* project.c: an old oversized sample bank */

static void collect(uint8_t *d)
{
    memset(d, 0, EXTRAS_BYTES);
    mfx_state_get(d);            /* 0..31 */
    lfo_state_get(d + 32);       /* 32..35 */
    arp_state_get(d + 36);       /* 36..91 */
    groove_screen_extras_get(d + 92); /* 92..117 */
    lfo2_state_get(d + 118);     /* 118..121 */
    groove_screen_extras2_get(d + 122); /* 122..175 */
}
static void spread(const uint8_t *d)
{
    mfx_state_put(d);
    lfo_state_put(d + 32);
    arp_state_put(d + 36);
    groove_screen_extras_put(d + 92);
    lfo2_state_put(d + 118);
    groove_screen_extras2_put(d + 122);
}
void extras_defaults(void)
{
    uint8_t d[EXTRAS_BYTES]; memset(d, 0xff, sizeof d); /* 0xff = keep module defaults */
    spread(d);
}
int extras_save(unsigned slot)
{
    if (slot >= GROOVE_PROJECT_SLOTS) return -1;
    /* A sample bank larger than the new limit would overlap this sector. */
    if (groove_flash_tail_blocked()) return -2;
    uint8_t *sector = malloc(FM1_FLASH_SECTOR_BYTES); /* read-modify-write */
    if (!sector) return -3;
    int r = fm1_flash_read(EXTRAS_OFFSET, sector, FM1_FLASH_SECTOR_BYTES);
    if (r) { free(sector); return r; }
    struct record rec; rec.magic = MAGIC; rec.slot = slot; collect(rec.data);
    rec.crc = groove_crc32(~0U, &rec, offsetof(struct record, crc));
    memcpy(sector + slot * RECORD, &rec, sizeof rec);
    r = fm1_flash_erase(EXTRAS_OFFSET);
    if (!r) r = fm1_flash_write(EXTRAS_OFFSET, sector, FM1_FLASH_SECTOR_BYTES);
    free(sector);
    return r;
}
int extras_load(unsigned slot)
{
    uint8_t raw[RECORD];
    if (slot >= GROOVE_PROJECT_SLOTS) return -1;
    int r = fm1_flash_read(EXTRAS_OFFSET + slot * RECORD, raw, sizeof raw);
    uint32_t magic, at, crc;
    memcpy(&magic, raw, 4); memcpy(&at, raw + 4, 4);
    unsigned n = magic == MAGIC ? EXTRAS_BYTES : magic == MAGIC_V1 ? EXTRAS_BYTES_V1 : 0;
    if (n) memcpy(&crc, raw + 8 + n, 4);
    if (r || !n || at != slot || crc != groove_crc32(~0U, raw, 8 + n)) { extras_defaults(); return r ? r : 1; }
    uint8_t d[EXTRAS_BYTES]; memset(d, 0xff, sizeof d); /* 0xff = keep module defaults */
    memcpy(d, raw + 8, n);
    spread(d);
    return 0;
}
