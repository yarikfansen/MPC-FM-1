/* Host test for app/boot_guard.c. Freestanding: no C library needed.
 * Flash model follows the SDK contract: writes only clear bits (else
 * NEEDS_ERASE), erase sets one aligned sector to 0xff.
 * Build (Windows, any clang):  tests/run_boot_guard.sh
 * Exit code 0 = all passed, otherwise the number of the first failed check. */
#include "fm1.h"
#include "boot_guard.h"
#include "flash_layout.h"

void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

static unsigned char flash[GROOVE_FLASH_BYTES];
static unsigned flash_size = GROOVE_FLASH_BYTES, writes, erases;
static uint32_t clock_ms;
uint32_t fm1_millis(void) { return clock_ms; }
uint32_t fm1_flash_size(void) { return flash_size; }
int fm1_flash_read(uint32_t at, void *data, size_t bytes)
{
    if (at > flash_size || bytes > flash_size - at) return FM1_FLASH_INVALID;
    memcpy(data, flash + at, bytes); return 0;
}
extern unsigned boot_guard_test_rom;
/* Power cut: Flash operation number cut_at (1 = the first one after arming) is
 * torn in one of four ways and nothing after it happens. The firmware cannot
 * tell (it is dead), so later calls simply do nothing.
 *   mode 0: the operation did not start; 1: it finished; 2 / 3: half done
 *   (a byte: only its lowest / highest bit programmed; several bytes: the
 *   first half; an erase: the first / second half of the sector).
 * The same silence applies once the chip is in its USB download mode: the
 * firmware no longer runs there (rom_returns = 1 tests the case that it does). */
static unsigned ops, cut_at, cut_mode, cut_hit, rom0, rom_returns;
static int dead(void) { return cut_hit || (!rom_returns && boot_guard_test_rom != rom0); }
static int cut_now(void) { if (cut_at && ++ops == cut_at) cut_hit = 1; return (int)cut_hit; }
int fm1_flash_write(uint32_t at, const void *data, size_t bytes)
{
    const unsigned char *s = data;
    if (at > flash_size || bytes > flash_size - at) return FM1_FLASH_INVALID;
    if (dead()) return 0;
    if (cut_now()) {
        if (cut_mode == 1) memcpy(flash + at, data, bytes);
        else if (cut_mode >= 2 && bytes > 1) memcpy(flash + at, data, bytes / 2);
        else if (cut_mode >= 2) {
            unsigned clear = flash[at] & ~(unsigned)s[0], bit = 0; /* bits this write programs */
            for (unsigned b = 0; b < 8; ++b) if (clear & (1U << b)) { bit = 1U << b; if (cut_mode == 2) break; }
            flash[at] = (unsigned char)(flash[at] & ~bit);
        }
        return 0;
    }
    for (size_t i = 0; i < bytes; ++i) if ((flash[at + i] & s[i]) != s[i]) return FM1_FLASH_NEEDS_ERASE;
    memcpy(flash + at, data, bytes); ++writes; return 0;
}
int fm1_flash_erase(uint32_t at)
{
    if (at % FM1_FLASH_SECTOR_BYTES || at > flash_size - FM1_FLASH_SECTOR_BYTES) return FM1_FLASH_INVALID;
    if (dead()) return 0;
    if (cut_now()) {
        if (cut_mode == 1) memset(flash + at, 0xff, FM1_FLASH_SECTOR_BYTES);
        else if (cut_mode == 2) memset(flash + at, 0xff, FM1_FLASH_SECTOR_BYTES / 2);
        else if (cut_mode == 3) memset(flash + at + FM1_FLASH_SECTOR_BYTES / 2, 0xff, FM1_FLASH_SECTOR_BYTES / 2);
        return 0;
    }
    memset(flash + at, 0xff, FM1_FLASH_SECTOR_BYTES); ++erases; return 0;
}
/* audio_output.c wraps these for the audio ISR; no ISR here. */
int flash_write_safe(uint32_t at, const void *data, size_t bytes) { return fm1_flash_write(at, data, bytes); }
int flash_erase_safe(uint32_t at) { return fm1_flash_erase(at); }
void boot_guard_test_reset(void);

static int failed;
#define CHECK(n, c) do { if (!(c) && !failed) failed = (n); } while (0)

/* One power-on: start, power-on keys, then run for `run_ms` (0 = crash at once). */
static void boot(uint32_t keys, uint32_t run_ms, int ready)
{
    boot_guard_test_reset();
    rom0 = boot_guard_test_rom;
    clock_ms = 1000;
    boot_guard_start();
    boot_guard_keys(keys);
    for (uint32_t t = 0; t < run_ms; t += 100) { clock_ms += 100; boot_guard_poll(ready); }
}
static const unsigned char *journal(void) { return flash + GROOVE_BOOT_JOURNAL_OFFSET; }

/* The power-ons of one life of the journal; stops at the one that loses power.
 * near_full: the sector has room for a few starts only, so it also wraps. */
static void life(unsigned near_full)
{
    memset(flash + GROOVE_BOOT_JOURNAL_OFFSET, 0x5a, 4096); /* foreign data: the first start formats */
    if (near_full) {
        const uint32_t header[2] = {0x47424d46U, ~0x47424d46U};
        memset(flash + GROOVE_BOOT_JOURNAL_OFFSET, 0xff, 4096);
        memcpy(flash + GROOVE_BOOT_JOURNAL_OFFSET, header, sizeof header);
        memset(flash + GROOVE_BOOT_JOURNAL_OFFSET + 8, BOOT_SLOT_ALIVE, 4088 - near_full);
    }
    static const struct { uint8_t keys, healthy, clear; } starts[] = {
        {0, 1, 0}, {0, 0, 0}, {0, 0, 0}, /* healthy, two crashes */
        {0, 0, 0}, {0, 0, 0},            /* SAFE starts crash too */
        {0, 0, 0},                       /* download mode */
        {0, 0, 0},                       /* the SAFE try after it crashes */
        {0, 0, 0},                       /* download mode again */
        {0, 1, 0}, {0, 1, 1},            /* SAFE try lives; again, and the user clears */
        {0, 1, 0}, {0, 0, 0}, {0, 1, 0}, /* normal, one crash, normal */
        {1, 1, 0}, {0, 1, 0} };          /* OCT- held, normal */
    for (unsigned i = 0; i < sizeof starts / sizeof *starts && !cut_hit; ++i) {
        boot(starts[i].keys, starts[i].healthy ? 7000 : 0, 1);
        if (starts[i].clear && !cut_hit && boot_guard_test_rom == rom0) (void)boot_guard_clear();
    }
}
/* After any power cut: the download mode is never entered at two power-ons in
 * a row (was: at every one), and living starts plus CLEAR lead back to a
 * normal start with no failures. Returns 0 when both hold. */
static unsigned settles(void)
{
    unsigned before = 0, bad = 0;
    cut_at = cut_hit = ops = 0;
    for (unsigned i = 0; i < 12; ++i) {
        boot(0, 0, 1);
        unsigned visit = boot_guard_test_rom != rom0;
        if (visit && before) bad |= 1;
        before = visit;
    }
    for (unsigned i = 0; i < 4; ++i) {
        boot(0, 7000, 1);
        if (boot_guard_test_rom == rom0 && boot_guard_safe()) (void)boot_guard_clear();
    }
    boot(0, 7000, 1);
    if (boot_guard_test_rom != rom0 || boot_guard_safe() || boot_guard_failures()) bad |= 2;
    return bad;
}
int main(void)
{
    /* 1. Foreign data in the sector (stock firmware, samples): formatted, normal start. */
    memset(flash, 0x5a, sizeof flash);
    boot(0, 7000, 1);
    CHECK(1, !boot_guard_safe() && boot_guard_failures() == 0);
    CHECK(2, journal()[8] == BOOT_SLOT_ALIVE && journal()[9] == 0xff);
    CHECK(3, flash[GROOVE_BOOT_JOURNAL_OFFSET - 1] == 0x5a && flash[GROOVE_BOOT_JOURNAL_OFFSET + 4096] == 0x5a);

    /* 2. Two crashes in a row -> SAFE on the third start. */
    boot(0, 0, 1);  CHECK(4, !boot_guard_safe());
    boot(0, 0, 1);  CHECK(5, !boot_guard_safe() && boot_guard_failures() == 1);
    boot(0, 7000, 1); CHECK(6, boot_guard_safe() && boot_guard_failures() == 2 && boot_guard_reason() == BOOT_GUARD_FAILED_STARTS);

    /* 3. A healthy SAFE session does not reset the count: SAFE again next time. */
    boot(0, 7000, 1); CHECK(7, boot_guard_safe() && boot_guard_failures() == 2);

    /* 4. SAVE-hold clears it: the next start is normal. */
    CHECK(8, boot_guard_clear() == 0);
    boot(0, 7000, 1); CHECK(9, !boot_guard_safe() && boot_guard_failures() == 0);

    /* 5. Power off within 6 s, or a start that never finishes loading, counts as a failure. */
    boot(0, 3000, 1); boot(0, 60000, 0);
    boot(0, 7000, 1); CHECK(10, boot_guard_safe() && boot_guard_failures() == 2);
    CHECK(11, boot_guard_clear() == 0);

    /* 6. Four failed starts (SAFE ones crash too) -> USB download mode. */
    boot_guard_test_rom = 0;
    for (int i = 0; i < 4; ++i) boot(0, 0, 1);
    CHECK(12, boot_guard_test_rom == 0);
    boot(0, 0, 1);  CHECK(13, boot_guard_test_rom == 1);
    /* Not for good (a reinstall keeps this sector): the next power-on is one SAFE try ... */
    boot(0, 0, 1);  CHECK(14, boot_guard_test_rom == 1 && boot_guard_safe() && boot_guard_failures() == 3 &&
                              boot_guard_reason() == BOOT_GUARD_FAILED_STARTS);
    boot(0, 0, 1);  CHECK(26, boot_guard_test_rom == 2);            /* ... it crashed too: download mode again */
    boot(0, 7000, 1); CHECK(27, boot_guard_test_rom == 2 && boot_guard_safe()); /* this SAFE try lives */
    boot(0, 7000, 1); CHECK(28, boot_guard_test_rom == 2 && boot_guard_safe() && boot_guard_failures() == 3); /* SAFE until cleared */
    CHECK(15, boot_guard_clear() == 0);
    boot(0, 7000, 1); CHECK(29, boot_guard_test_rom == 2 && !boot_guard_safe() && boot_guard_failures() == 0);
    /* The visit is one programmed bit and counts as three failed starts. */
    { unsigned char t[4] = {BOOT_SLOT_ALIVE, BOOT_SLOT_ROM, BOOT_SLOT_SAFE_ALIVE, BOOT_SLOT_SAFE_STARTED};
      CHECK(30, boot_journal_failures(t, 2) == 3 && boot_journal_failures(t, 3) == 3 && boot_journal_failures(t, 4) == 4 &&
                (BOOT_SLOT_ROM ^ 0xff) == 0x80); }
    /* If the download mode call ever returned, the start goes on as a SAFE one. */
    for (int i = 0; i < 4; ++i) boot(0, 0, 1);
    rom_returns = 1; boot_guard_test_rom = 0;
    boot(0, 7000, 1); CHECK(31, boot_guard_test_rom == 1 && boot_guard_safe());
    rom_returns = 0;
    boot(0, 7000, 1); CHECK(32, boot_guard_test_rom == 1 && boot_guard_safe() && boot_guard_failures() == 3);
    CHECK(33, boot_guard_clear() == 0);

    /* 7. Power-on keys. */
    boot_guard_test_rom = 0;
    boot(1U << 0, 7000, 1); CHECK(16, boot_guard_safe() && boot_guard_reason() == BOOT_GUARD_KEYS);
    boot(0, 7000, 1);  CHECK(17, !boot_guard_safe());             /* keys do not stick */
    boot((1U << 0) | (1U << 1), 0, 1); CHECK(18, boot_guard_test_rom == 1);
    boot(1U << 1, 7000, 1); CHECK(19, !boot_guard_safe() && boot_guard_test_rom == 1); /* OCT+ alone: normal */

    /* 8. The sector fills up: erased once, the failure count survives. */
    unsigned e0 = erases;
    for (int i = 0; i < 4200; ++i) boot(0, 7000, 1);
    CHECK(20, erases == e0 + 1 && !boot_guard_safe());
    /* Sector exactly full and ending in two failed starts: wraps and keeps them. */
    { unsigned char *slot = flash + GROOVE_BOOT_JOURNAL_OFFSET + 8;
      memset(slot, BOOT_SLOT_ALIVE, 4088); slot[4086] = slot[4087] = BOOT_SLOT_STARTED;
      unsigned e2 = erases;
      boot(0, 7000, 1);
      CHECK(21, erases == e2 + 1 && boot_guard_safe() && boot_guard_failures() == 2 &&
                slot[0] == BOOT_SLOT_STARTED && slot[1] == BOOT_SLOT_STARTED &&
                slot[2] == BOOT_SLOT_SAFE_ALIVE && slot[3] == 0xff);
      CHECK(25, boot_guard_clear() == 0); }

    /* 9. No journal possible (wrong Flash size): never blocks a start. */
    flash_size = 4096;
    boot(1U << 1, 7000, 1); CHECK(22, !boot_guard_safe());
    flash_size = GROOVE_FLASH_BYTES;

    /* 10. A torn slot (unknown value) ends the count like a healthy start. */
    { unsigned char t[3] = {BOOT_SLOT_STARTED, 0x7e, BOOT_SLOT_STARTED};
      CHECK(23, boot_journal_failures(t, 3) == 1); }

    /* 12. Power cut at every Flash operation of a whole life of the journal,
     * torn four ways, with a fresh and with an almost full sector. */
    { unsigned runs = 0;
      for (unsigned fill = 0; fill < 3; ++fill)
          for (unsigned mode = 0; mode < 4; ++mode)
              for (unsigned at = 1; at < 200; ++at) {
                  cut_at = at; cut_mode = mode; cut_hit = ops = 0;
                  life(fill == 0 ? 0 : fill == 1 ? 6 : 13);
                  if (!cut_hit) break; /* the life has fewer operations than this */
                  ++runs;
                  unsigned bad = settles();
                  CHECK(34, !(bad & 1)); /* download mode at two power-ons in a row */
                  CHECK(35, !(bad & 2)); /* no way back to a normal start */
              }
      cut_at = cut_hit = ops = 0;
      CHECK(36, runs > 150); /* the cuts really happened */
      CHECK(37, flash[GROOVE_BOOT_JOURNAL_OFFSET - 1] == 0x5a && flash[GROOVE_BOOT_JOURNAL_OFFSET + 4096] == 0x5a);
      memset(flash + GROOVE_BOOT_JOURNAL_OFFSET, 0x5a, 4096); boot(0, 7000, 1); }

    /* 11. Wear: about two byte writes per start, nothing else. */
    unsigned w0 = writes, e1 = erases;
    boot(0, 7000, 1);
    CHECK(24, writes - w0 == 2 && erases == e1);
    return failed;
}
