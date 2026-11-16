/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Phase G-B: the v13 SOUND pages (docs/mockups/FM1-v13-screens.html: md, knmd, t303, kn303, tsnd, knsnd, smp, env).
 * Included by screen.c after the shared G-A helpers; everything here is static and draws through the banded painter.
 * Rule of every picture: it shows the REAL values of the knob touched last (K1 on entering); turning a knob changes it
 * and, while the knob is hot (1.5 s), draws its part thicker. Curves are cached per (tag, key): computed once, drawn per band. */
enum { SP_X = 14, SP_W = 212, SP_GRID = 0x39a8, SP_BASE = 0x2123, SP_BOX = 0x1082 };
static int16_t sp_y[SP_W + 1], sp_y2[SP_W + 1];
static uint32_t sp_key, sp_tag;
static unsigned sp_need(unsigned tag, uint32_t key)
{
    if (sp_tag == tag && sp_key == key) return 0;
    sp_tag = tag; sp_key = key;
    return 1;
}
static unsigned sp_f(float n) { return n <= 0.0f ? 0U : n >= 1.0f ? 256U : (unsigned)(n * 256.0f + 0.5f); }
static float sp_n(unsigned v, unsigned m) { return m ? (float)v / (float)m : 0.0f; }
static float sp_clamp1(float v) { return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v; }
static int sp_i(float v) { return (int)(v + (v < 0.0f ? -0.5f : 0.5f)); }
/* A line of t pixels thickness through y[0..n-1] (one column each) from x0. */
static void sp_curve(int x0, const int16_t *y, unsigned n, uint16_t c, unsigned t)
{
    for (unsigned i = 0; i < n; ++i) {
        int a = y[i ? i - 1 : 0], b = y[i], lo = (a < b ? a : b) - (int)(t / 2U), hi = (a < b ? b : a) - (int)(t / 2U) + (int)t;
        if (lo < 0) lo = 0;
        if (hi > lo && x0 + (int)i >= 0) paint_fill((unsigned)(x0 + (int)i), (unsigned)lo, 1, (unsigned)(hi - lo), c);
    }
}
static void sp_dash_h(int x0, int x1, int y, uint16_t c, int on, int off)
{
    for (int x = x0; x < x1; x += on + off) paint_fill((unsigned)x, (unsigned)y, (unsigned)(x + on > x1 ? x1 - x : on), 1, c);
}
static void sp_dash_v(int x, int y0, int y1, uint16_t c, int on, int off)
{
    for (int y = y0; y < y1; y += on + off) paint_fill((unsigned)x, (unsigned)y, 1, (unsigned)(y + on > y1 ? y1 - y : on), c);
}
/* Filled dot with a ring (mockup circ: fill + stroke); r in 1/10 px. */
static void sp_dot(int cx, int cy, int r10, uint16_t fill, uint16_t edge)
{
    fill_ellipse(cx * 10, cy * 10, r10 + 8, r10 + 8, edge);
    fill_ellipse(cx * 10, cy * 10, r10 - 8, r10 - 8, fill);
}
/* Arrow head: tip at (tipx, y), pointing right (dir 1) or left (-1), s px long and tall. */
static void sp_arrow(int tipx, int y, int dir, int s, uint16_t c)
{
    fill_tri((tipx - dir * s) * 10, y * 10 - s * 5, (tipx - dir * s) * 10, y * 10 + s * 5, tipx * 10, y * 10, c);
}
/* The painted under-curve tint of a per-column curve (flat alpha a of 256). */
/* user 13.11 (mockup FM1-fx-cleanup s.11): no fills or gradients under any curve. */

/* ---- 303 FILTER / MICRO D BRIGHT: the filter curve over the frequency zones ---- */
static void sp_fcurve(int16_t *y, unsigned fc, float res)
{
    float lfc = vlog2((float)fc);
    for (unsigned i = 0; i <= SP_W; ++i) {
        unsigned f = freq_at(SP_X, SP_W, SP_X + i);
        float l2 = vlog2((float)(f ? f : 1U)) - lfc, db = -3.0103f * vlog2(1.0f + fexp2_ui(8.0f * l2)); /* 4 poles */
        float t = l2 * 2.0069f; /* log10(f / fc) / 0.15 */
        db += res * 12.0f * fexp2_ui(-t * t * 1.4427f);
        if (db > 12.0f) db = 12.0f;
        if (db < -48.0f) db = -48.0f;
        y[i] = (int16_t)(120.0f + (12.0f - db) * 64.0f / 60.0f + 0.5f);
    }
}
/* 303 FILTER / MICRO D BRIGHT (user 13.11, mockup FM1-fx-cleanup s.11): the filter response as one still green line,
 * no zones, fill, ghost curve or markers. */
static void sp_filter(unsigned fc, float res, float env, float dec, int hl, int anim)
{
    (void)env; (void)dec; (void)hl; (void)anim;
    if (sp_need(1, fc * 31U + sp_f(res))) sp_fcurve(sp_y, fc, res);
    sp_curve(SP_X, sp_y, SP_W + 1, MX_G, 3U);
}

/* ---- the small pictures (knob colour c, value n 0..1 unless said) ---- */
static void sp_needle(uint16_t c, float pos, int cy)
{
    for (int i = -12; i <= 12; ++i) {
        unsigned h = i == 0 ? 24U : i % 4 == 0 ? 14U : 8U;
        paint_fill((unsigned)(120 + i * 8), (unsigned)cy - h / 2U, 1, h, i == 0 ? TEXT : V_DIM);
    }
    int nx = 120 + sp_i(sp_clamp1(pos) * 96.0f);
    fill_tri((nx - 3) * 10, (cy - 28) * 10, (nx + 3) * 10, (cy - 28) * 10, nx * 10, (cy - 22) * 10, c);
    paint_fill((unsigned)nx, (unsigned)cy - 20U, 2, 40, c);
}
static void sp_bar(uint16_t c, float n, int y) { glass_bar_h(SP_X, (unsigned)y, SP_W, 18, sp_f(n), c); }
static void sp_wave(uint16_t c, unsigned shape, float periods, int amp, int y0, unsigned t)
{
    if (sp_need(2, shape * 4099U + (unsigned)(periods * 16.0f) * 31U + (unsigned)amp * 977U + (unsigned)y0)) {
        for (unsigned x = 0; x <= SP_W; ++x) {
            float u = (float)x / (float)SP_W * periods, ph = u - (float)(int)u, v;
            v = shape == 0 ? 1.0f - 2.0f * ph : shape == 1 ? (ph < 0.5f ? 1.0f : -1.0f) : vsin(ph);
            sp_y[x] = (int16_t)(y0 - sp_i(v * (float)amp));
        }
    }
    sp_curve(SP_X, sp_y, SP_W + 1, c, t);
}
static void sp_fall_curve(uint16_t c, float st, float fl, int h, int base, int hl)
{
    unsigned s = sp_f(st), f = sp_f(fl);
    if (sp_need(3, (s * 513U + f) * 131U + (unsigned)h * 7U + (unsigned)base)) {
        for (unsigned x = 0; x <= SP_W; ++x) {
            float u = (float)x / (float)SP_W, e = u < st ? 0.0f : fexp2_ui(-(u - st) / (fl < 0.02f ? 0.02f : fl) * 3.0f);
            sp_y[x] = (int16_t)(base - sp_i((float)h * e));
        }
    }
    sp_curve(SP_X, sp_y, SP_W + 1, c, hl ? 4U : 3U);
}
static void sp_pan(uint16_t c, float pos, int cy)
{
    paint_fill(SP_X, (unsigned)cy, SP_W, 2, V_DIM);
    paint_fill(119, (unsigned)cy - 10U, 2, 20, V_DIM);
    int cx = 120 + sp_i(sp_clamp1(pos) * 96.0f);
    fill_ellipse(cx * 10, cy * 10, 80, 80, c);
}
static void sp_harm(uint16_t c, float n)
{
    for (unsigned i = 0; i < 8; ++i) {
        float h = fexp2_ui(-(1.8f - 1.4f * n) * vlog2((float)(i + 1U)));
        glass_bar_v(30U + i * 24U, 108, 12, 72, sp_f(h), c);
    }
}
static void sp_string(uint16_t c, float n)
{
    unsigned s = sp_f(n);
    if (sp_need(4, s)) {
        float tau = 20.0f + n * 120.0f;
        for (unsigned k = 0; k <= SP_W; ++k)
            sp_y[k] = (int16_t)(146 - sp_i(vsin((float)k / 2.2f * 0.159155f) * fexp2_ui(-(float)k / tau * 1.4427f) * 34.0f));
    }
    sp_curve(SP_X, sp_y, SP_W + 1, c, 2);
}
static void sp_sample(uint16_t c, unsigned from_start, float frac)
{
    if (sp_need(5, 5U)) {
        for (unsigned x = 0; x <= SP_W; ++x) {
            float a = vsin((float)x * 0.9f * 0.159155f), b = vsin((float)x / 40.0f * 0.159155f);
            sp_y[x] = (int16_t)(146 + sp_i(a * (b < 0.0f ? -b : b) * 30.0f));
        }
    }
    int m = sp_i(frac * 212.0f);
    sp_curve(SP_X, sp_y, SP_W + 1, 0x5aab, 2);
    for (int x = 0; x <= SP_W; ++x) if (from_start ? x >= m : x <= m) sp_curve(SP_X + x, &sp_y[x], 1, c, 2);
    sp_dash_v(SP_X + m, 112, 180, c, 2, 2);
}
static void sp_glide(uint16_t c, float n)
{
    unsigned s = sp_f(n);
    if (sp_need(6, s)) {
        float gl = 0.01f + 0.4f * n;
        for (unsigned x = 0; x <= SP_W; ++x) {
            float u = (float)x / (float)SP_W, k = u < 0.45f ? 0.0f : u > 0.45f + gl ? 1.0f : (u - 0.45f) / gl;
            sp_y[x] = (int16_t)(176 - sp_i(70.0f * k));
        }
    }
    sp_dash_v(SP_X + 95, 106, 176, 0x5aab, 2, 3);
    sp_curve(SP_X, sp_y, SP_W + 1, c, 3);
}
static void sp_drive(uint16_t c, float n)
{
    float gn = 1.0f + 3.0f * n;
    unsigned s = sp_f(n);
    if (sp_need(7, s)) {
        for (unsigned x = 0; x <= SP_W; ++x) {
            float v = vsin((float)x / 213.0f * 2.0f);
            sp_y[x] = (int16_t)(146 - sp_i(v * 34.0f / gn));
            sp_y2[x] = (int16_t)(146 - sp_i(sp_clamp1(v * gn) * 34.0f));
        }
    }
    sp_curve(SP_X, sp_y, SP_W + 1, V_DIM, 2);
    sp_curve(SP_X, sp_y2, SP_W + 1, c, 3);
}
static void sp_type(uint16_t c, unsigned idx)
{
    for (unsigned t = 0; t < 3; ++t) {
        unsigned x0 = SP_X + t * 71U, on = t == idx;
        rbox(x0, 108, 64, 76, 2, SP_BOX, on ? c : SP_GRID);
        int16_t y[61];
        for (int k = 0; k < 61; ++k) {
            float in = (float)k / 30.0f - 1.0f, o = t == 0 ? in : t == 1 ? in / (1.0f + (in < 0 ? -in : in)) * 1.6f
                       : (in > 0.35f ? 0.35f : in < -0.35f ? -0.35f : in) * 2.5f;
            if (o > 0.9f) o = 0.9f;
            if (o < -0.9f) o = -0.9f;
            y[k] = (int16_t)(146 - sp_i(o * 30.0f));
        }
        sp_curve((int)x0 + 2, y, 61, on ? c : 0x5aab, on ? 3U : 2U);
    }
}
static void sp_acc(uint16_t c, float n)
{
    paint_fill(SP_X, 180, SP_W, 1, SP_GRID);
    for (unsigned b = 0; b < 2; ++b) {
        int x0 = SP_X + (b ? 117 : 25), h = b ? (int)(20.0f + n * 70.0f > 76.0f ? 76.0f : 20.0f + n * 70.0f) : 26;
        int16_t y[70];
        for (unsigned k = 0; k < 70; ++k) y[k] = (int16_t)(180 - sp_i((float)h * fexp2_ui(-(float)k / 69.0f * 4.0f)));
        sp_curve(x0, y, 70, c, 3);
    }
}
static void sp_keytrack(uint16_t c, unsigned on)
{
    sp_dash_h(30, 198, on ? 176 : 146, 0x5aab, 2, 3);
    for (unsigned i = 0; i < 8; ++i) fill_ellipse((30 + (int)i * 24) * 10, (on ? 176 - (int)i * 9 : 146) * 10, 34, 34, c);
}
static void sp_detune(uint16_t c, float n)
{
    unsigned s = sp_f(n);
    if (sp_need(8, s)) {
        for (unsigned x = 0; x <= SP_W; ++x) {
            float u0 = (float)x / 36.0f, u1 = u0 + n * 0.45f;
            sp_y[x] = (int16_t)(146 - sp_i(30.0f * (1.0f - 2.0f * (u0 - (float)(int)u0))));
            sp_y2[x] = (int16_t)(146 - sp_i(30.0f * (1.0f - 2.0f * (u1 - (float)(int)u1))));
        }
    }
    sp_curve(SP_X, sp_y, SP_W + 1, c, 2);
    sp_curve(SP_X, sp_y2, SP_W + 1, mix565(MD_PANEL, c, 24), 2);
}

/* ---- coloured envelopes: four stretches in the four knob colours, soft gradient below, no glass strips ---- */
static void sp_seg(int x0, int y0, int x1, int y1, uint16_t c, unsigned t)
{
    int dx = x1 - x0;
    for (int x = 0; x <= dx; ++x) {
        int ya = y0 + (dx ? (y1 - y0) * x / dx : 0), yb2 = y0 + (dx ? (y1 - y0) * (x + 1) / dx : 0);
        if (x == dx && dx) yb2 = ya;
        int lo = (ya < yb2 ? ya : yb2) - (int)(t / 2U), hi = (ya < yb2 ? yb2 : ya) - (int)(t / 2U) + (int)t;
        if (lo >= 0) paint_fill((unsigned)(x0 + x), (unsigned)lo, 1, (unsigned)(hi - lo), c);
    }
}
static void sp_poly4(const int *px, const int *py, int yb, int ytop, int hl, unsigned dim)
{
    paint_fill(12, (unsigned)yb, 216, 1, SP_BASE);
    for (unsigned i = 0; i < 4; ++i) {
        sp_seg(px[i], py[i], px[i + 1], py[i + 1], dim ? 0x5aab : MX_G, 3U); /* user 13.11: one green line */
        (void)hl; (void)ytop;
    }
}
/* ATTACK / DECAY / SUSTAIN / RELEASE as 0..100 percents. */
static void sp_adsr(unsigned a, unsigned d, unsigned s, unsigned r, int hl, unsigned dim, int yb, int hh)
{
    int aw = 8 + (int)a / 2, dw = 8 + (int)d * 7 / 10, rw = 8 + (int)r * 6 / 10, ys = yb - hh * (int)s / 100;
    int px[5] = {16, 16 + aw, 16 + aw + dw, 16 + aw + dw + 50, 16 + aw + dw + 50 + rw}, py[5] = {yb, yb - hh, ys, ys, yb};
    sp_poly4(px, py, yb, yb - hh, hl, dim);
    for (unsigned i = 1; i < 4; ++i) sp_dot(px[i], py[i], 34, TEXT, dim ? 0x5aab : knob_colors[i - 1U]);
}

/* ---- header pieces ---- */
static void sp_ico(int cx, int cy, const int16_t (*p)[2], unsigned n, uint16_t c)
{
    for (unsigned i = 0; i + 1 < n; ++i) aa_stroke(cx * 10 + p[i][0], cy * 10 + p[i][1], cx * 10 + p[i + 1][0], cy * 10 + p[i + 1][1], c);
}
static void sp_sine_ico(int cx, int cy, int w, int amp10, float periods, uint16_t c)
{
    int px = 0, py = 0;
    for (int k = 0; k <= w; ++k) {
        int x = cx * 10 - w * 5 + k * 10, y = cy * 10 - sp_i(vsin((float)k / (float)w * periods) * (float)amp10);
        if (k) aa_stroke(px, py, x, y, c);
        px = x; py = y;
    }
}
static void md_ico0(int cx, int cy, uint16_t c) { static const int16_t p[5][2] = {{-120, 60}, {-24, -60}, {-24, 60}, {72, -60}, {72, 60}}; sp_ico(cx, cy, p, 5, c); }
static void md_ico1(int cx, int cy, uint16_t c)
{
    static const int16_t a[4][2] = {{-120, 48}, {-12, -60}, {-12, 48}, {96, -60}}, b[4][2] = {{-96, 72}, {12, -36}, {12, 72}, {120, -36}};
    sp_ico(cx, cy, a, 4, c); sp_ico(cx, cy, b, 4, mix565(0x10a3, c, 26));
}
static void md_ico2(int cx, int cy, uint16_t c) { static const int16_t p[5][2] = {{-120, -24}, {0, -24}, {36, -72}, {60, 12}, {120, 72}}; sp_ico(cx, cy, p, 5, c); }
static void md_ico3(int cx, int cy, uint16_t c) { static const int16_t p[5][2] = {{-120, 72}, {-60, -72}, {0, -12}, {60, -12}, {120, 72}}; sp_ico(cx, cy, p, 5, c); }
static void md_ico4(int cx, int cy, uint16_t c) { sp_sine_ico(cx, cy, 24, 60, 1.0f, c); }
static const icon_draw_fn md_icons[5] = {md_ico0, md_ico1, md_ico2, md_ico3, md_ico4};
static void t303_ico0(int cx, int cy, uint16_t c) { static const int16_t p[5][2] = {{-120, 30}, {-30, 30}, {0, -80}, {40, 30}, {120, 100}}; sp_ico(cx, cy, p, 5, c); }
static void t303_ico1(int cx, int cy, uint16_t c) { sp_sine_ico(cx, cy, 18, 55, 1.0f, c); }
static void t303_ico2(int cx, int cy, uint16_t c) { static const int16_t p[4][2] = {{-90, 60}, {-40, -60}, {40, -60}, {90, 60}}; sp_ico(cx, cy, p, 4, c); }
static const icon_draw_fn t303_icons[3] = {t303_ico0, t303_ico1, t303_ico2};

/* Rotated text, read bottom to top, centred on (cx, cy): the mockup's lcdRot (pad names). */
static void sp_text_up(int cx, int cy, const char *s, const struct lcd_font *f, uint16_t c, uint16_t bg)
{
    unsigned w = 0;
    for (const char *p = s; *p; ++p) { unsigned g = lcd_glyph(f, *p); if (g != ~0U) w += f->adv[g]; }
    int xl = cx - (int)f->digit_top - (int)f->digit_height / 2, ys = cy + (int)w / 2, pos = 0;
    for (; *s; ++s) {
        unsigned g = lcd_glyph(f, *s);
        if (g == ~0U) continue;
        unsigned gw = f->w[g];
        const uint8_t *bits = f->bits + f->off[g];
        int gx = pos + f->x[g];
        pos += f->adv[g];
        for (unsigned row = 0; row < f->height; ++row)
            for (unsigned col = 0; col < gw; ++col) {
                unsigned i = row * gw + col, a = (bits[i >> 2] >> (2U * (i & 3U))) & 3U;
                if (a) paint_fill((unsigned)(xl + (int)row), (unsigned)(ys - (gx + (int)col) - 1), 1, 1, blend565(bg, c, a));
            }
    }
}
/* A pad's label: its number (returns 0) or the first three letters of its name (returns 1). */
static unsigned sp_pad_label(unsigned i, char out[4])
{
    const char *nm = groove_pad_name(track, i);
    unsigned n = 0;
    for (; nm && *nm && n < 3; ++nm) {
        char ch = *nm >= 'a' && *nm <= 'z' ? (char)(*nm - 32) : *nm;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) out[n++] = ch;
    }
    out[n] = 0;
    if (n == 5 && !memcmp(groove_pad_name(track, i), "EMPTY", 5)) n = 0;
    if (!n || !strcmp(out, "EMP")) { out[0] = (char)('1' + i); out[1] = 0; return 0; }
    return 1;
}
/* The eight header pads (x 44 + 19 n, 17 x 34): lit = target(s), edge = holds a sample, glass cursor = chosen. */
static void sp_pad_row(unsigned lit_first, unsigned lit_count, unsigned sel, uint16_t lit_c)
{
    for (unsigned i = 0; i < 8; ++i) {
        unsigned x = 44 + i * 19, lit = i >= lit_first && i < lit_first + lit_count, has = groove_samples_pad_frames(i) != 0;
        uint16_t bg = lit ? mix565(0x10a3, lit_c, 8) : 0x10a3, tc = lit ? lit_c : has ? TEXT : V_DIM;
        rbox(x, HDR_FY, 17, 34, 3, bg, lit ? lit_c : has ? PAL_AMBER : SP_GRID);
        char lab[4];
        ++keep_names;
        if (sp_pad_label(i, lab)) sp_text_up((int)x + 8, HDR_FY + 17, lab, &lcd16, tc, bg);
        else lcd_center((int)x + 8, HDR_FY + 9, lab, &lcd16, tc, bg);
        --keep_names;
        if (i == sel) glass_cursor(x, HDR_FY, 17, 34, 8, MD_AMBER);
    }
}
static void sp_pad_sig(uint32_t *sig)
{
    char lab[4];
    for (unsigned i = 0; i < 8; ++i) {
        unsigned named = sp_pad_label(i, lab);
        *sig = signature(*sig, lab, 3) + named + (groove_samples_pad_frames(i) != 0) * 77U;
    }
}
/* 808: 11 drums + KIT in a narrow strip (mockup stripPads, y 108, 20 px). */
static void sp_strip808(unsigned count, unsigned sel)
{
    static const char *const nm[12] = {"KIC", "SNA", "LTM", "MTM", "HTM", "RIM", "CLP", "COW", "CYM", "OHT", "CHT", "KIT"};
    unsigned w = (212U - (count - 1U) * 2U) / count;
    for (unsigned i = 0; i < count; ++i) {
        unsigned x = 14 + i * (w + 2U);
        uint16_t c = i < GROOVE_DRUMS ? pad_colors[i % 12U] : TEXT;
        rbox(x, 108, w, 20, 2, MD_PANEL, c);
        ++keep_names; lcd_center((int)(x + w / 2U), 112, nm[i], &lcd8, c, MD_PANEL); --keep_names;
        if (i == sel) glass_cursor(x, 108, w, 20, 5, MD_AMBER);
    }
}
