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
int fm1_flash_write(uint32_t at, const void *data, size_t bytes)
{
    const unsigned char *s = data;
    if (at > flash_size || bytes > flash_size - at) return FM1_FLASH_INVALID;
    for (size_t i = 0; i < bytes; ++i) if ((flash[at + i] & s[i]) != s[i]) return FM1_FLASH_NEEDS_ERASE;
    memcpy(flash + at, data, bytes); ++writes; return 0;
}
int fm1_flash_erase(uint32_t at)
{
    if (at % FM1_FLASH_SECTOR_BYTES || at > flash_size - FM1_FLASH_SECTOR_BYTES) return FM1_FLASH_INVALID;
    memset(flash + at, 0xff, FM1_FLASH_SECTOR_BYTES); ++erases; return 0;
}
extern unsigned boot_guard_test_rom;
void boot_guard_test_reset(void);

static int failed;
#define CHECK(n, c) do { if (!(c) && !failed) failed = (n); } while (0)

/* One power-on: start, power-on keys, then run for `run_ms` (0 = crash at once). */
static void boot(uint32_t keys, uint32_t run_ms, int ready)
{
    boot_guard_test_reset();
    clock_ms = 1000;
    boot_guard_start();
    boot_guard_keys(keys);
    for (uint32_t t = 0; t < run_ms; t += 100) { clock_ms += 100; boot_guard_poll(ready); }
}
static const unsigned char *journal(void) { return flash + GROOVE_BOOT_JOURNAL_OFFSET; }

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
    boot(0, 7000, 1); CHECK(14, boot_guard_test_rom == 2); /* stays there until reflashed */
    CHECK(15, boot_guard_clear() == 0);

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

    /* 11. Wear: about two byte writes per start, nothing else. */
    unsigned w0 = writes, e1 = erases;
    boot(0, 7000, 1);
    CHECK(24, writes - w0 == 2 && erases == e1);
    return failed;
}
