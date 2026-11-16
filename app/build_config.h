/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_BUILD_CONFIG_H
#define FM1_BUILD_CONFIG_H
/* 1: the MICRO D engine is in the firmware (FM1d builds); 0: the same firmware
 * without it (the branch stable-no-micro-d sets 0). */
#define FM1_MICRO_D 1
/* 1: BINAURAL (GLOBAL > BINAURAL: beats + noise bed, headphone crossfeed),
 * app/binaural.c; 0: built without it (no page, no audio cost). */
#define FM1_BINAURAL 1
#endif
