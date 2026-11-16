/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
#include "lang.h"
#include "lang_ru.h"
#include <string.h>

enum { TEXTS = sizeof lang_ru / sizeof lang_ru[0] };
static unsigned language;
static unsigned theme = 1; /* v13: CLASSIC unless a saved setting says otherwise (extras byte 222) */
unsigned ui_theme_get(void) { return theme; }
void ui_theme_set(unsigned t) { theme = t < 3 ? t : 0; }
unsigned lang_get(void) { return language; }
void lang_set(unsigned l) { language = l != 0; }
/* Binary search for the n characters at s (the table is in strcmp order). */
static const char *find(const char *s, unsigned n)
{
    int lo = 0, hi = TEXTS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strncmp(s, lang_ru[mid][0], n);
        if (!c && lang_ru[mid][0][n]) c = -1; /* s is a prefix of the key: s sorts first */
        if (!c) return lang_ru[mid][1];
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return 0;
}
const char *lang_tr(const char *s)
{
    if (!language || !s || !*s) return s;
    unsigned n = 0;
    for (const char *p = s; *p; ++p, ++n) if ((unsigned char)*p >= 0x80) return s; /* already Russian */
    const char *t = find(s, n);
    if (t) return t;
    static char buf[4][64];
    static unsigned turn;
    char *o = buf[turn++ & 3U];
    unsigned w = 0, changed = 0;
    for (const char *p = s; *p && w < 63;) {
        if (*p == ' ') { o[w++] = *p++; continue; }
        /* the longest run of up to four words that is a known text ("TRACK ROLE  >") */
        unsigned len = 0, end = 0, words = 0;
        const char *word = 0;
        while (p[end] && words < 4) {
            while (p[end] && p[end] != ' ') ++end;
            ++words;
            const char *t = find(p, end);
            if (t) { word = t; len = end; }
            if (p[end] != ' ' || p[end + 1] == ' ') break; /* phrases have single spaces */
            ++end;
        }
        if (!word) { len = 0; while (p[len] && p[len] != ' ') ++len; }
        const char *src = word ? word : p;
        unsigned m = word ? (unsigned)strlen(word) : len;
        changed |= word != 0;
        for (unsigned i = 0; i < m && w < 63; ++i) o[w++] = src[i];
        p += len;
    }
    o[w] = 0;
    return changed ? o : s;
}
