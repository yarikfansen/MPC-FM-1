/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "boot_guard.h"
#include "flash_layout.h"
#include "fm1.h"
#ifndef FM1_HOST_TEST
#include "fm1_device.h"
#endif

/* Journal sector: an 8-byte header, then one byte per start. Bytes are only
 * ever programmed 1 -> 0, so a start costs one byte write and the sector is
 * erased once every ~4000 starts:
 *   0xff empty, 0xfe started, 0xfc started+alive,
 *   0xfa safe start, 0xf8 safe start+alive, 0x00 failures cleared by the user.
 * A torn or unknown byte ends the count, like a healthy start. */
enum { HEADER = 8, SLOTS = FM1_FLASH_SECTOR_BYTES - HEADER, TAIL = 32 };
static const uint32_t MAGIC = 0x47424d46U; /* 'FMBG' */
_Static_assert(GROOVE_BOOT_JOURNAL_OFFSET % FM1_FLASH_SECTOR_BYTES == 0, "journal sector alignment");
_Static_assert(GROOVE_BOOT_JOURNAL_OFFSET >= GROOVE_SAMPLE_OFFSET + GROOVE_SAMPLE_LIMIT, "journal overlaps the sample bank");

static unsigned started, journal_ok, safe, alive_done, slot, failures;
static enum boot_guard_reason reason;
static uint32_t start_ms;

unsigned boot_journal_failures(const uint8_t *slots, unsigned count)
{
    unsigned n = 0;
    while (count--) {
        uint8_t v = slots[count];
        if (v == BOOT_SLOT_STARTED || v == BOOT_SLOT_SAFE_STARTED) { if (n < 255) ++n; }
        else if (v != BOOT_SLOT_SAFE_ALIVE) break; /* healthy, cleared or unknown */
    }
    return n;
}
static int retry(int r) { return r == FM1_FLASH_BUSY; }
static int put(unsigned index, uint8_t value)
{
    int r;
    unsigned tries = 0;
    do r = fm1_flash_write(GROOVE_BOOT_JOURNAL_OFFSET + HEADER + index, &value, 1);
    while (retry(r) && ++tries < 3);
    return r;
}
static int format(void)
{
    const uint32_t header[2] = {MAGIC, ~MAGIC};
    int r = fm1_flash_erase(GROOVE_BOOT_JOURNAL_OFFSET);
    if (!r) r = fm1_flash_write(GROOVE_BOOT_JOURNAL_OFFSET, header, sizeof header);
    return r;
}
/* First written-but-unknown position: the first empty byte. */
static int find_next(unsigned *next)
{
    uint8_t chunk[256];
    for (unsigned at = 0; at < SLOTS; at += sizeof chunk) {
        unsigned n = SLOTS - at < sizeof chunk ? SLOTS - at : sizeof chunk;
        int r = fm1_flash_read(GROOVE_BOOT_JOURNAL_OFFSET + HEADER + at, chunk, n);
        if (r) return r;
        for (unsigned i = 0; i < n; ++i)
            if (chunk[i] == BOOT_SLOT_EMPTY) { *next = at + i; return 0; }
    }
    *next = SLOTS;
    return 0;
}
static int open_journal(unsigned *next, unsigned *count)
{
    uint32_t header[2];
    int r = fm1_flash_read(GROOVE_BOOT_JOURNAL_OFFSET, header, sizeof header);
    if (r) return r;
    *count = 0;
    if (header[0] != MAGIC || header[1] != ~MAGIC) { *next = 0; return format(); }
    r = find_next(next);
    if (r) return r;
    uint8_t tail[TAIL];
    unsigned n = *next < TAIL ? *next : TAIL;
    if (n) r = fm1_flash_read(GROOVE_BOOT_JOURNAL_OFFSET + HEADER + *next - n, tail, n);
    if (r) return r;
    *count = boot_journal_failures(tail, n);
    if (*next >= SLOTS) { /* full: start a fresh sector, keep the count */
        r = format();
        unsigned keep = *count < BOOT_GUARD_ROM_AFTER ? *count : BOOT_GUARD_ROM_AFTER;
        for (*next = 0; !r && *next < keep; ++*next) r = put(*next, BOOT_SLOT_STARTED);
    }
    return r;
}
void boot_guard_start(void)
{
    if (started) return;
    started = 1;
    start_ms = fm1_millis();
    if (fm1_flash_size() < GROOVE_FLASH_BYTES) return; /* no journal: never block a start */
    unsigned next = 0, count = 0;
    if (open_journal(&next, &count)) return;
    failures = count;
    if (failures >= BOOT_GUARD_ROM_AFTER) boot_guard_enter_rom();
    if (failures >= BOOT_GUARD_SAFE_AFTER) { safe = 1; reason = BOOT_GUARD_FAILED_STARTS; }
    slot = next;
    journal_ok = !put(slot, safe ? BOOT_SLOT_SAFE_STARTED : BOOT_SLOT_STARTED);
}
void boot_guard_keys(uint32_t buttons)
{
    const uint32_t oct_down = 1U << 0, oct_up = 1U << 1; /* panel buttons 0 and 1 */
    if ((buttons & (oct_down | oct_up)) == (oct_down | oct_up)) boot_guard_enter_rom();
    if ((buttons & oct_down) && !safe) {
        safe = 1;
        reason = BOOT_GUARD_KEYS;
        if (journal_ok) (void)put(slot, BOOT_SLOT_SAFE_STARTED); /* 0xfe -> 0xfa */
    }
}
int boot_guard_safe(void) { return safe; }
int boot_guard_settling(void) { return started && fm1_millis() - start_ms < BOOT_GUARD_KEY_MS; }
enum boot_guard_reason boot_guard_reason(void) { return safe ? reason : BOOT_GUARD_NORMAL; }
unsigned boot_guard_failures(void) { return failures; }
void boot_guard_poll(int ready)
{
    if (!journal_ok || alive_done || fm1_millis() - start_ms < BOOT_GUARD_ALIVE_MS || !(safe || ready))
        return;
    int r = put(slot, safe ? BOOT_SLOT_SAFE_ALIVE : BOOT_SLOT_ALIVE);
    if (!retry(r)) alive_done = 1; /* a failed write must not be retried forever */
}
int boot_guard_clear(void)
{
    if (!journal_ok) return -1;
    int r = 0;
    if (slot + 1 >= SLOTS) { r = format(); slot = 0; }
    else ++slot;
    if (!r) r = put(slot, BOOT_SLOT_CLEARED);
    if (!r) { failures = 0; alive_done = 1; }
    return r;
}
#ifdef FM1_HOST_TEST
unsigned boot_guard_test_rom;
void boot_guard_test_reset(void) { started = journal_ok = safe = alive_done = slot = failures = 0; reason = BOOT_GUARD_NORMAL; }
#endif
void boot_guard_enter_rom(void)
{
#ifndef FM1_HOST_TEST
    fm1_device_recovery(); /* chip mask-ROM USB download mode */
#else
    ++boot_guard_test_rom;
#endif
}
