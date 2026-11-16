/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#ifndef FM1_LANG_H
#define FM1_LANG_H
/* Screen language: 0 English, 1 Russian (GLOBAL > LANGUAGE). Control task. */
unsigned lang_get(void);
void lang_set(unsigned language);
/* The text to draw for an English screen text: itself in English; in Russian
 * the whole text from app/lang_ru.h, else word by word (texts built at run
 * time). The result may point to a small rotating buffer. */
const char *lang_tr(const char *s);
/* Screen theme: 0 ORGANIC (moss, default), 1 CLASSIC (black), 2 MATRIX (green phosphor). */
unsigned ui_theme_get(void);
void ui_theme_set(unsigned theme);
#endif
