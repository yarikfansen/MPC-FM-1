/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_FLASH_LAYOUT_H
#define FM1_FLASH_LAYOUT_H
/* Layout of the user Flash partition (offsets from its start).
 * Project sectors stay at 0..68 KiB; one shared sample bank follows. The
 * partition is smaller than in stock V14 (GROOVE_USER_BYTES, also set in
 * sdk/Build.ps1 and sdk/Package.cs) so that our larger program fits. */
enum { GROOVE_SAMPLE_OFFSET = 69632, GROOVE_SAMPLE_BYTES = 131072,
       GROOVE_FLASH_BYTES = GROOVE_SAMPLE_OFFSET + GROOVE_SAMPLE_BYTES };
/* Tail, from the end: tuning/session preferences (1 sector, V14), four DX7
 * banks (V14), our pattern extras (app/extras.c) and our boot journal
 * (app/boot_guard.c). New sample transfers stop before them. A larger legacy
 * bank past GROOVE_SAMPLE_TAIL overlaps extras or the V14 storage. */
enum { GROOVE_TUNING_OFFSET = GROOVE_FLASH_BYTES - 4096,
       GROOVE_LIBRARY_OFFSET = GROOVE_TUNING_OFFSET - 4 * 4096,
       GROOVE_EXTRAS_OFFSET = GROOVE_LIBRARY_OFFSET - 4096,
       GROOVE_BOOT_JOURNAL_OFFSET = GROOVE_EXTRAS_OFFSET - 4096,
       GROOVE_SAMPLE_TAIL = GROOVE_EXTRAS_OFFSET - GROOVE_SAMPLE_OFFSET,
       GROOVE_SAMPLE_LIMIT = GROOVE_BOOT_JOURNAL_OFFSET - GROOVE_SAMPLE_OFFSET };
#endif
