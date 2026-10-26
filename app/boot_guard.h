/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_BOOT_GUARD_H
#define FM1_BOOT_GUARD_H
#include <stdint.h>

/* Boot protection. The SDK runs our init hooks before it starts USB and the
 * firmware update handler, so a crash or hang while starting (for example on
 * damaged user data) used to hide the instrument from every updater.
 *
 * A journal sector counts starts that never reached a healthy state:
 *   2 failed starts in a row  -> SAFE mode: no project, settings, samples or
 *                                Bluetooth are read; USB and updates work;
 *   4 failed starts in a row  -> the chip's USB download mode (mask ROM).
 * At power-on: OCT- held = SAFE mode, OCT- and OCT+ held = USB download mode.
 * A start becomes healthy after BOOT_GUARD_ALIVE_MS of normal running.
 * Control task only. */
enum { BOOT_GUARD_SAFE_AFTER = 2, BOOT_GUARD_ROM_AFTER = 4, BOOT_GUARD_ALIVE_MS = 6000,
       BOOT_GUARD_KEY_MS = 1500 /* OCT- may be pressed up to 1.5 s after power-on */ };
enum boot_guard_reason { BOOT_GUARD_NORMAL, BOOT_GUARD_FAILED_STARTS, BOOT_GUARD_KEYS };

void boot_guard_start(void);               /* first app hook (groove_samples_init) */
void boot_guard_keys(uint32_t buttons);     /* after the panel scan runs */
int boot_guard_safe(void);
int boot_guard_settling(void);              /* first BOOT_GUARD_KEY_MS: keys still count, user area not read yet */
enum boot_guard_reason boot_guard_reason(void);
unsigned boot_guard_failures(void);         /* failed starts before this one */
void boot_guard_poll(int ready);            /* ready: normal start finished loading */
int boot_guard_clear(void);                 /* SAFE screen: forget the failures */
void boot_guard_enter_rom(void);            /* does not return on the device */

/* Journal logic, exposed for host tests. */
enum { BOOT_SLOT_EMPTY = 0xff, BOOT_SLOT_STARTED = 0xfe, BOOT_SLOT_ALIVE = 0xfc,
       BOOT_SLOT_SAFE_STARTED = 0xfa, BOOT_SLOT_SAFE_ALIVE = 0xf8, BOOT_SLOT_CLEARED = 0x00 };
unsigned boot_journal_failures(const uint8_t *slots, unsigned count);
#endif
