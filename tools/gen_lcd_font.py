#!/usr/bin/env python3
"""Generate app/font_lcd.h: 2-bit anti-aliased Barlow Condensed for the 240x240 LCD.

Barlow Condensed is SIL OFL 1.1 (c) 2017 The Barlow Project Authors.
Get the TTFs from https://github.com/google/fonts/tree/main/ofl/barlowcondensed
Usage: python tools/gen_lcd_font.py <dir with BarlowCondensed-*.ttf> [out.h]
       python tools/gen_lcd_font.py <dir> app/font_chord.h --chord   (chord-name fonts only)
Needs Pillow. Glyphs are rendered 8x larger and box-filtered down, so the
coverage matches the unhinted canvas text of docs/mockups/FM1-v6-screens.html.
Coordinates follow canvas textBaseline='top': y is the top of the em box,
the baseline sits at 1000/1200 em (Barlow hhea ascent 1000, descent 200).
"""
import sys, os
from PIL import Image, ImageDraw, ImageFont

SS = 8
FULL = [chr(c) for c in range(32, 96)] + ['·']   # space.._ then middle dot ('`' in C strings)
# Russian (second language): capitals А..Я after the dot, written as bytes
# 0x80..0x9F in C strings (app/lang_ru.h). Letters that look like Latin ones
# reuse the Barlow glyph; the others come from Windows' Bahnschrift (variable:
# weight 700/600, width 80 = about Barlow Condensed's width), scaled to its cap height.
CYR = [chr(0x410 + i) for i in range(32)]
LOOKALIKE = dict(zip('АВЕКМНОРСТХ', 'ABEKMHOPCTX'))
FULL = FULL + CYR
BAHN = os.environ.get('FM1_BAHNSCHRIFT', r'C:\Windows\Fonts\bahnschrift.ttf')
DIGITS = [chr(c) for c in range(45, 58)]   # - . / 0..9
# name, weight file, px, charset. Sizes are the ones the v6 mockup uses.
FONTS = [
    ('lcd44', 'Bold', 44, DIGITS),      # ARP octave
    ('lcd30', 'Bold', 30, FULL),        # EDIT patch name
    ('lcd26', 'Bold', 26, FULL),        # MASTER FX effect name
    ('lcd24', 'Bold', 24, FULL),        # BPM, GLOBAL drum centre row, ARP mode
    ('lcd22', 'Bold', 22, FULL),        # battery percent, FX name
    ('lcd20', 'Bold', 20, FULL),        # knob values
    ('lcd17', 'Bold', 17, FULL),        # GLOBAL drum, ARP PRESETS
    ('lcd16', 'Bold', 16, FULL),        # knob values
    ('lcd15', 'Bold', 15, FULL),        # SEL active row name
    ('lcd14', 'Bold', 14, FULL),        # SEL active items
    ('lcd13', 'Bold', 13, FULL),        # object line, mixer numbers, GLOBAL drum
    ('lcd12', 'Bold', 12, FULL),        # knob labels, mixer names
    ('lcd11', 'Bold', 11, FULL),        # section line, badges, CHAIN
    ('lcd10', 'SemiBold', 10, FULL),    # FM lab labels
    ('lcd9', 'SemiBold', 9, FULL),      # BPM label, OCT buttons
    ('lcd8', 'Bold', 8, FULL),          # ON/OFF lamp
]

# Chord names on the ARP / SEQ STEP plates (user 09.11, round 4): real lower case ("Gm7", "Cmaj7", "Bbm7b5").
# One contiguous range '#'..'u' so lcd_glyph() can index it; unused codes are empty glyphs (5 bytes each).
CHORD_USED = set('#-0123456789ABCDEFG' + 'abdgijmsu')
CHORD = [chr(c) if chr(c) in CHORD_USED else '' for c in range(ord('#'), ord('u') + 1)]
CHORD_FONTS = [
    ('lcd_chord40', 'Bold', 40, CHORD),  # up to 3 letters
    ('lcd_chord32', 'Bold', 32, CHORD),  # 4 letters and more
    ('lcd_chord26', 'Bold', 26, CHORD),  # when 32 px is still too wide (C#maj7)
]

def render(path, px, ch, weight='Bold'):
    ch = LOOKALIKE.get(ch, ch)
    if 'А' <= ch <= 'Я':
        font = ImageFont.truetype(BAHN, round(px * SS * 70 / 71))
        font.set_variation_by_axes([700 if weight == 'Bold' else 600, 80])
    else:
        font = ImageFont.truetype(path, px * SS)
    adv = font.getlength(ch) / SS
    base = px * 1000 / 1200
    W, H = (px * 2 + 8) * SS, (px * 2) * SS
    img = Image.new('L', (W, H), 0)
    ImageDraw.Draw(img).text((4 * SS, round(base) * SS), ch, font=font, fill=255, anchor='ls')
    w, h = W // SS, H // SS
    small = img.resize((w, h), Image.BOX)
    cov = [[small.getpixel((x, y)) for x in range(w)] for y in range(h)]
    return round(adv), cov

def quant(v):
    return min(3, (v * 3 + 127) // 255)

def build(name, weight, px, chars, src):
    path = os.path.join(src, 'BarlowCondensed-%s.ttf' % weight)
    glyphs = []
    for ch in chars:
        adv, cov = render(path, px, ch, weight)
        q = [[quant(v) for v in row] for row in cov]
        cols = [x for x in range(len(q[0])) if any(r[x] for r in q)]
        rows = [y for y in range(len(q)) if any(q[y])]
        glyphs.append((ch, adv, q, cols, rows))
    top = min(g[4][0] for g in glyphs if g[4])
    digits = [g[4] for g in glyphs if g[0].isdigit()]
    dtop, dbot = min(r[0] for r in digits), max(r[-1] for r in digits) + 1
    bot = max(g[4][-1] for g in glyphs if g[4]) + 1
    widths, advs, xoffs, offs, bits = [], [], [], [], []
    for ch, adv, q, cols, rows in glyphs:
        x0, x1 = (cols[0], cols[-1] + 1) if cols else (4, 4)
        widths.append(x1 - x0); advs.append(adv); xoffs.append(x0 - 4); offs.append(len(bits))
        acc, n = 0, 0
        for y in range(top, bot):
            for x in range(x0, x1):
                acc |= q[y][x] << n; n += 2
                if n == 8: bits.append(acc); acc, n = 0, 0
        if n: bits.append(acc)
    c = lambda a: ','.join(str(v) for v in a)
    first = ord(chars[0]) if chars[0] else ord('#')
    out = ['static const uint8_t %s_w[%d] = {%s};' % (name, len(widths), c(widths)),
           'static const uint8_t %s_adv[%d] = {%s};' % (name, len(advs), c(advs)),
           'static const int8_t %s_x[%d] = {%s};' % (name, len(xoffs), c(xoffs)),
           'static const uint16_t %s_off[%d] = {%s};' % (name, len(offs), c(offs)),
           'static const uint8_t %s_bits[%d] = {%s};' % (name, len(bits), c(bits)),
           'static const struct lcd_font %s = {%d, %d, %d, %d, %d, %d, %s_w, %s_adv, %s_x, %s_off, %s_bits};' % (
               name, top, bot - top, dtop, dbot - dtop, first, len(chars), name, name, name, name, name)]
    size = len(bits) + 5 * len(chars)
    return '\n'.join(out), size

def main_chord(src, dst):
    parts, total, report = [], 0, []
    for name, weight, px, chars in CHORD_FONTS:
        text, size = build(name, weight, px, chars, src)
        parts.append(text); total += size; report.append('%s %d B' % (name, size))
    head = ('/* Generated by tools/gen_lcd_font.py --chord from Barlow Condensed Bold (SIL OFL 1.1,\n'
            ' * (c) 2017 The Barlow Project Authors), as app/font_lcd.h. Chord names on the ARP / SEQ STEP\n'
            ' * plates (mockup round 4: 40 px, 32 px from 4 letters, 26 px when too wide), with real lower\n'
            ' * case: drawn with keep_case > 0 (lcd_glyph). Codes # .. u; the unused ones are empty. */\n'
            '#ifndef FM1_FONT_CHORD_H\n#define FM1_FONT_CHORD_H\n#include "font_lcd.h"\n')
    with open(dst, 'w', newline='\n') as f:
        f.write(head + '\n'.join(parts) + '\n#endif\n')
    print('font_chord.h: %d B (%s)' % (total, ', '.join(report)))

def main():
    src = sys.argv[1]
    if '--chord' in sys.argv[2:]:
        rest = [a for a in sys.argv[2:] if a != '--chord']
        return main_chord(src, rest[0] if rest else os.path.join(os.path.dirname(__file__), '..', 'app', 'font_chord.h'))
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', 'app', 'font_lcd.h')
    parts, total, report = [], 0, []
    for name, weight, px, chars in FONTS:
        text, size = build(name, weight, px, chars, src)
        parts.append(text); total += size; report.append('%s %d B' % (name, size))
    head = ('/* Generated by tools/gen_lcd_font.py from Barlow Condensed (SIL OFL 1.1,\n'
            ' * (c) 2017 The Barlow Project Authors). 2-bit anti-aliased, packed LSB first,\n'
            ' * row-major. y of a string = top of the em box, as canvas textBaseline top.\n'
            ' * Full sets: ASCII 32..95, a middle dot written as \'`\', then Russian capitals as\n'
            ' * bytes 0x80..0x9F (unique letters from Bahnschrift); lower case = upper. */\n'
            '#ifndef FM1_FONT_LCD_H\n#define FM1_FONT_LCD_H\n#include <stdint.h>\n'
            'struct lcd_font { uint8_t top, height, digit_top, digit_height, first, count; const uint8_t *w, *adv; const int8_t *x;\n'
            '    const uint16_t *off; const uint8_t *bits; };\n')
    with open(dst, 'w', newline='\n') as f:
        f.write(head + '\n'.join(parts) + '\n#endif\n')
    print('font_lcd.h: %d B (%s)' % (total, ', '.join(report)))

if __name__ == '__main__':
    main()
