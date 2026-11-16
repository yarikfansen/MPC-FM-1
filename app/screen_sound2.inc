/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Phase G-B part 2: the page functions (MICRO D, TONE_EDIT / SOURCE ENV, TRACK LFO, AMP ENV). Included by screen.c after screen_sound.inc. */

/* ================= MICRO D ================= */
static void micro_draw(void)
{
    unsigned pg = tone_pages[track][tone_slot()] % MD_PAGES, col = knob_shown(), hot = knob_hot(col);
    uint8_t p[MD_PARAMS], notes[4] = {0, 0, 0, 0};
    for (unsigned i = 0; i < MD_PARAMS; ++i) p[i] = (uint8_t)micro_d_value(track, i);
    unsigned voices = pg == 4 ? micro_d_voices(track, notes) : 0U;
    uint32_t title = track | (pg << 7) | (7U << 12) | (1U << 19);
    if (cache->title_cache != title) {
        cache->title_cache = title;
        paint_fill(42, 26, 198, 36, BACK);
        paint_fill(0, 62, 240, 133, BACK);
        memset(cache->tile_cache, 0xff, sizeof cache->tile_cache);
        cache->row_cache[0] = ~0U;
    }
    /* user 13.11: a voice's note no longer repaints the page: only that voice's box (MOVE page, 46 x 22 at
     * x 14 + 54 i, y 162). row_cache[1] = the shown voice bits, [2] = their notes. */
    uint32_t sig = signature(pg | (col << 4) | (hot << 6) | (6U << 24), p, sizeof p);
    uint32_t ns = (uint32_t)notes[0] | (uint32_t)notes[1] << 8 | (uint32_t)notes[2] << 16 | (uint32_t)notes[3] << 24;
    if (sig == cache->row_cache[0]) {
        if (pg == 4 && !paint_rendering)
            for (unsigned i = 0; i < 4; ++i) {
                unsigned on = (voices >> i) & 1U, was = (cache->row_cache[1] >> i) & 1U;
                if (on != was || (on && ((ns ^ cache->row_cache[2]) >> (8U * i) & 255U))) paint_mark(14 + i * 54, 162, 46, 22);
            }
        cache->row_cache[1] = voices; cache->row_cache[2] = ns;
        return;
    }
    cache->row_cache[0] = sig; cache->row_cache[1] = voices; cache->row_cache[2] = ns;
    paint_fill(42, 26, 198, 36, BACK);
    paint_fill(0, 62, 240, 132, BACK);
    icon_row(5, pg, md_icons);
    static const char *const page_title[MD_PAGES] = {"WAVES", "THICK", "BRIGHT", "SHAPE", "MOVE"};
    window_draw(page_title[pg], PAL_AMBER);
    int hl = hot ? (int)col : -1;
    float sq = (float)p[MD_SQR], width = 0.5f - (sq > 64.0f ? (sq - 64.0f) * (0.4f / 63.0f) : 0.0f);
    if (pg == 0) {
        float lv[4], tot = 0.0f;
        for (unsigned i = 0; i < 4; ++i) { lv[i] = i == 2 ? (sq > 64.0f ? 1.0f : sq / 64.0f) : (float)p[i] / 127.0f; tot += lv[i]; }
        if (tot < 0.5f) tot = 0.5f;
        for (unsigned i = 0; i < 4; ++i) {
            int x = 16 + (int)i * 30;
            int16_t y[21];
            for (int j = 0; j < 21; ++j) y[j] = (int16_t)(110 - sp_i(5.0f * md_wave(i, (float)(j % 10) / 10.0f, width)));
            sp_curve(x + 2, y, 21, knob_colors[i], 2);
            glass_bar_v((unsigned)x + 4U, 121, 18, 63, sp_f((float)p[i] / 127.0f), knob_colors[i]);
        }
        rbox(140, 104, 86, 80, 2, SP_BOX, MD_EDGE);
        lcd_center(183, 106, "OSCILLATOR", &lcd12, PAL_AMBER, SP_BOX);
        if (sp_need(9, signature(1, p, 4) * 31U + (unsigned)(width * 100.0f))) {
            for (unsigned c = 0; c < 78; ++c) {
                unsigned j = c * 20U / 21U;
                float u = (float)(j % 37U) / 37.0f, sum = 0.0f;
                for (unsigned i = 0; i < 4; ++i) sum += md_wave(i, u, width) * lv[i];
                sp_y[c] = (int16_t)(150 - sp_i(28.0f * sum / tot));
            }
        }
        sp_curve(145, sp_y, 78, PAL_AMBER, 2);
    } else if (pg == 1) {
        if (col == 0) sp_detune(knob_colors[0], (float)p[MD_DETUNE] / 127.0f);
        else if (col == 1) {
            int v = (int)p[MD_NOISE] - 64;
            noise_dots(16, 110, 206, 72, (unsigned)((v < 0 ? -v : v) * 256 / 64), knob_colors[1]);
        } else if (col == 2) sp_glide(knob_colors[2], (float)p[MD_GLIDE] / 127.0f);
        else sp_keytrack(knob_colors[3], p[MD_VCA] != 0);
    } else if (pg == 2) {
        if (col < 3) {
            float hz = 30.0f * fexp2_ui((float)p[MD_CUTOFF] * (1.0f / 127.0f) * 8.7f);
            unsigned fc = hz < 20.0f ? 20U : hz > 12000.0f ? 12000U : (unsigned)hz;
            sp_filter(fc, (float)p[MD_RESO] / 127.0f, ((float)p[MD_CONTOUR] - 64.0f) / 63.0f, 0.0f, hl, -1);
        } else sp_drive(knob_colors[3], (float)p[MD_DRIVE] / 127.0f);
    } else if (pg == 3) {
        float a = (float)p[MD_ATTACK] / 127.0f, d = (float)p[MD_DECAY] / 127.0f, s = (float)p[MD_SUSTAIN] / 127.0f;
        int yb = 184, ys = yb - sp_i(72.0f * s), x1 = 16 + 6 + sp_i(a * 60.0f), x2 = x1 + 8 + sp_i(d * 100.0f);
        if (x2 > 204) x2 = 204;
        int px[5] = {16, x1, x2, 204, 224}, py[5] = {yb, 112, ys, ys, yb};
        sp_poly4(px, py, yb, 112, hl, 0);
        if (p[MD_RETRIG]) {
            sp_dash_h(16, 224, 187, knob_colors[3], 4, 3);
            sp_arrow(11, 187, -1, 6, knob_colors[3]);
        }
    } else {
        float spd = (float)p[MD_RATE] / 127.0f, n = 2.0f + spd * 8.0f;
        int amp = sp_i((float)((int)p[MD_AMOUNT] > 63 ? (int)p[MD_AMOUNT] - 63 : 63 - (int)p[MD_AMOUNT]) / 63.0f * 22.0f);
        unsigned shape = p[MD_WAVE] & 3U;
        uint16_t c = hl >= 0 && hl < 3 ? knob_colors[hl] : knob_colors[0];
        if (sp_need(10, (sp_f(spd) * 5U + shape) * 64U + (unsigned)amp)) {
            for (unsigned x = 0; x <= SP_W; ++x) {
                float u = (float)x / (float)SP_W * n, ph = u - (float)(int)u, v;
                if (shape == 0) v = vsin(ph);
                else if (shape == 1) v = 1.0f - 4.0f * (ph < 0.5f ? 0.5f - ph : ph - 0.5f);
                else if (shape == 2) v = ph < 0.5f ? 1.0f : -1.0f;
                else { unsigned k = (unsigned)u * 2654435761U; v = (float)((k >> 24) & 255U) / 127.5f - 1.0f; }
                sp_y[x] = (int16_t)(132 - sp_i(v * (float)amp));
            }
        }
        paint_fill(14, 132, 212, 1, SP_BASE);
        if (hl == 2) { sp_dash_h(14, 226, 132 - amp, knob_colors[2], 3, 3); sp_dash_h(14, 226, 132 + amp, knob_colors[2], 3, 3); }
        sp_curve(SP_X, sp_y, SP_W + 1, c, hl == 0 ? 4U : 3U);
        unsigned used_n = p[MD_MODE] == 0 ? 1U : p[MD_MODE] == 4 ? 4U : p[MD_MODE] + 1U;
        for (unsigned i = 0; i < 4; ++i) {
            unsigned x = 14 + i * 54, used = i < used_n, on = (voices >> i) & 1U;
            rbox(x, 162, 46, 22, 2, MD_PANEL, used ? knob_colors[3] : MD_EDGE);
            char nn[12]; if (on) note_name(notes[i], nn); else strcpy(nn, used ? "-" : "");
            lcd_center((int)x + 23, 165, nn, &lcd15, on ? TEXT : V_DIM, MD_PANEL);
        }
    }
}

/* ================= TONE_EDIT / SOURCE ENV of 303, 808, PSG, PLUCK, SAMPLER ================= */
static int sp_is(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a, y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y) return 0;
    }
    return !*a && !*b;
}
/* The picture of one knob. nm = the engine's name for it, n = value 0..1, raw = value. */
/* 303 FILTER, ENVMOD / DECAY shown: a note every 1.2 s throws the cutoff up, DECAY sets how fast it falls back (live). */
static void sp_knob_picture(unsigned type, unsigned pg, unsigned col, const char *nm, float n, unsigned raw, const uint8_t *values, int hl)
{
    uint16_t c = knob_colors[col];
    if (type == GROOVE_303) {
        if (pg == 0) {
            float nv[4];
            for (unsigned k = 0; k < 4; ++k) {
                const fm1_param_t *kp = groove_tone_parameter(track, 0, k);
                nv[k] = kp && kp->max ? (float)values[k] / (float)kp->max : 0.0f;
            }
            float hz = 1500.0f * fexp2_ui((nv[0] - 0.5f) * 4.5f);
            sp_filter((unsigned)(hz < 40.0f ? 40.0f : hz > 12000.0f ? 12000.0f : hz), nv[1], nv[2] > 0.0f ? nv[2] : 0.0f, nv[2] > 0.0f ? nv[3] : 0.0f, hl, -1);
        } else if (pg == 1) {
            if (col == 0) sp_acc(c, n);
            else if (col == 1) sp_wave(c, raw ? 1U : 0U, 2.0f, 30, 146, 3);
            else if (col == 2) sp_needle(c, n * 2.0f - 1.0f, 146);
            else sp_bar(c, n, 136);
        } else {
            if (col == 0) sp_drive(c, n);
            else if (col == 1) sp_type(c, raw);
            else if (col == 2) sp_glide(c, n);
            else sp_fall_curve(c, 0.15f, 0.03f + 0.7f * n, 70, 180, hl >= 0);
        }
    } else if (type == GROOVE_PSG) {
        if (col == 0) {
            if (raw == 2U) noise_dots(16, 110, 206, 72, 230, c);
            else { sp_wave(c, 1U, 3.0f, 26, 146, 3); if (raw == 3U) noise_dots(16, 110, 206, 72, 90, c); }
        } else if (col == 1) noise_dots(16, 110, 206, 72, sp_f(n), c);
        else if (col == 2) sp_needle(c, n * 2.0f - 1.0f, 146);
        else sp_fall_curve(c, 0.1f, 0.1f + 0.8f * n, 64, 180, hl >= 0);
    } else if (type == GROOVE_PLUCK) {
        if (col == 0) sp_string(c, n);
        else if (col == 1) sp_harm(c, n);
        else if (col == 2) sp_needle(c, n * 2.0f - 1.0f, 146);
        else sp_fall_curve(c, 0.1f, 0.1f + 0.8f * n, 64, 180, hl >= 0);
    } else if (type == GROOVE_808) {
        if (sp_is(nm, "Tune")) sp_needle(c, n * 2.0f - 1.0f, 160);
        else if (sp_is(nm, "Decay")) sp_fall_curve(c, 0.1f, 0.1f + 0.8f * n, 40, 182, hl >= 0);
        else if (sp_is(nm, "Pan")) sp_pan(c, n * 2.0f - 1.0f, 160);
        else sp_bar(c, n, 152);
    } else {
        if (sp_is(nm, "Level")) sp_bar(c, n, 136);
        else if (sp_is(nm, "Tune")) sp_needle(c, n * 2.0f - 1.0f, 146);
        else if (sp_is(nm, "Length") || sp_is(nm, "Start")) sp_sample(c, sp_is(nm, "Start"), n);
        else if (sp_is(nm, "Choke")) {
            for (unsigned i = 0; i < 4; ++i) {
                unsigned x = 30 + i * 50, on = raw == i + 1U;
                rbox(x, 122, 34, 34, 2, on ? mix565(MD_PANEL, c, 14) : MD_PANEL, on ? c : SP_GRID);
                if (on) { aa_stroke((int)(x + 8) * 10, 1300, (int)(x + 26) * 10, 1480, c); aa_stroke((int)(x + 26) * 10, 1300, (int)(x + 8) * 10, 1480, c); }
            }
        } else if (sp_is(nm, "Variation")) {
            for (unsigned i = 0; i < 4; ++i) {
                unsigned on = raw == i, x0 = 16 + i * 56;
                for (unsigned k = 0; k < 40; ++k) {
                    unsigned h = (unsigned)(fexp2_ui(-(float)k / (8.0f + (float)i * 3.0f)) * (float)(34 - (int)i * 5)) + 1U;
                    paint_fill(x0 + k, 147 - h, 1, 2 * h, on ? c : V_FAINT);
                }
            }
        } else {
            for (unsigned side = 0; side < 2; ++side)
                for (unsigned k = 0; k < 96; ++k) {
                    float amp = side ? 30.0f : 30.0f * (0.35f + 0.65f * (1.0f - n));
                    unsigned h = (unsigned)(fexp2_ui(-(float)k / 18.0f) * amp) + 1U;
                    paint_fill(16 + side * 112 + k, 147 - h, 1, 2 * h, side ? c : mix565(BACK, c, 14));
                }
        }
    }
}
static __attribute__((noinline)) void tone_draw(const uint8_t values[GROOVE_TONE_PARAMS])
{
    if (micro_d_track(track)) { /* its path strip sits in the header, from y 27 */
        if (!(paint_rendering && (paint_top >= 195 || paint_top + PAINT_ROWS <= 26))) micro_draw();
        return;
    }
    if (paint_rendering && (paint_top >= 195 || paint_top + PAINT_ROWS <= 26)) return;
    unsigned type = groove_source_type(track), part = tone_part(), env = page == FM_ENV,
             pg = env ? 0 : tone_pages[track][tone_slot()], col = knob_shown(), hot = knob_hot(col);
    unsigned owns = (type == GROOVE_303 && !env) || type == GROOVE_SAMPLE;
    const struct groove_view view = frame_views[track];
    uint32_t title = track | (part << 3) | (pg << 7) | (type << 12) | (view.mono << 16) | (env << 17);
    if (cache->title_cache != title) {
        cache->title_cache = title;
        paint_fill(0, 62, 240, 133, BACK);
        memset(cache->tile_cache, 0xff, sizeof cache->tile_cache);
        cache->row_cache[0] = ~0U;
    }
    uint32_t sig = signature(part | (pg << 4) | (col << 8) | (hot << 10) | (env << 11) | 0x7000U, values, GROOVE_TONE_PARAMS);
    sig += view.next_length * 7919U + (unsigned)chain_on;
    if (type == GROOVE_SAMPLE) sp_pad_sig(&sig);
    if (sig == cache->row_cache[0]) return;
    cache->row_cache[0] = sig;
    if (owns) paint_fill(42, 26, 198, 36, BACK);
    paint_fill(0, 62, 240, 133, BACK);
    {
        char num[12]; decimal(num, (int)view.next_length, 1);
        state_plate(num, chain_on != 0, view.next_length != view.sequence_length ? V_ORANGE : TEXT);
    }
    if (type == GROOVE_303 && !env) icon_row(3, pg, t303_icons);
    if (type == GROOVE_SAMPLE) sp_pad_row(part, 1, part, knob_colors[0]);
    int hl = hot ? (int)col : -1;
    char nm_buf[12];
    const char *name = "";
    if (env) name = type == GROOVE_303 ? "ENVELOPE" : "SOURCE ENV";
    else if (type == GROOVE_303) name = pg == 0 ? "FILTER" : pg == 1 ? "OSC" : "DRIVE";
    else if (type == GROOVE_PSG) name = "PSG";
    else if (type == GROOVE_PLUCK) name = "PLUCK";
    else if (type == GROOVE_SAMPLE) { strcpy(nm_buf, "PAD "); decimal(nm_buf + 4, (int)part + 1, 1); name = nm_buf; }
    else name = part < GROOVE_DRUMS ? groove_pad_name(track, part) : "KIT";
    window_draw(name, PAL_AMBER);
    if (type == GROOVE_808) sp_strip808(GROOVE_DRUMS + (env ? 0U : 1U), part);
    if (env) { /* by analogy with the DX7 envelope: the engine's four time / shape knobs as A D S R */
        unsigned v[4];
        for (unsigned k = 0; k < 4; ++k) {
            unsigned pi = tone_parameter(k);
            const fm1_param_t *kp = pi == 255 ? 0 : groove_tone_parameter(track, part, pi);
            v[k] = kp && kp->max ? values[pi] * 100U / kp->max : 0U;
        }
        if (type == GROOVE_303) /* user 13.11 (mockup): height = ENVMOD, length of the fall = DECAY, one green line */
            sp_fall_curve(MX_G, 0.05f, 0.05f + 0.6f * (float)v[1] / 100.0f, 8 + (int)v[0] * 60 / 100, 184, hl == 0 || hl == 1);
        else sp_adsr(v[0], v[1], v[2], v[3], hl, 0, 178, type == GROOVE_808 ? 44 : 68);
        return;
    }
    unsigned parameter = tone_parameter(col);
    const fm1_param_t *ap = groove_tone_parameter(track, part, parameter);
    if (!ap) return;
    sp_knob_picture(type, pg, col, ap->name, ap->max ? (float)values[parameter] / (float)ap->max : 0.0f, values[parameter], values, hl);
}

/* ================= TRACK LFO (non-DX7) and AMP ENV (DX7) ================= */
static __attribute__((noinline)) void track_lfo_draw(void)
{
    const uint8_t *v = frame_patch;
    const struct groove_view *lv = &frame_views[track];
    unsigned col = knob_shown(), hot = knob_hot(col);
    uint32_t sig = signature(track | (lv->type << 8) | (col << 12) | (hot << 14) | (lv->next_length << 16) | ((unsigned)chain_on << 28), v, 4);
    if (cache->title_cache != sig) {
        cache->title_cache = sig;
        paint_fill(0, 62, 240, 133, BACK);
        char num[12]; decimal(num, (int)lv->next_length, 1);
        state_plate(num, chain_on != 0, lv->next_length != lv->sequence_length ? V_ORANGE : TEXT);
        window_draw("LFO", PAL_AMBER);
        unsigned shape = v[3] & 3U; /* TRI, SQUARE, SAW UP, SAW DOWN */
        float cycles = 1.0f + (float)v[0] * 0.04f;
        int amp = sp_i((float)v[1] * 0.3f);
        if (sp_need(11, (shape * 128U + v[0]) * 128U + v[1])) {
            for (unsigned x = 0; x <= SP_W; ++x) {
                float u = (float)x / (float)SP_W * cycles, ph = u - (float)(int)u, w;
                w = shape == 0 ? 1.0f - 4.0f * (ph < 0.5f ? 0.5f - ph : ph - 0.5f) : shape == 1 ? (ph < 0.5f ? 1.0f : -1.0f)
                  : shape == 2 ? 2.0f * ph - 1.0f : 1.0f - 2.0f * ph;
                sp_y[x] = (int16_t)(142 - sp_i(w * (float)amp));
            }
        }
        paint_fill(14, 142, 212, 1, SP_BASE);
        sp_curve(SP_X, sp_y, SP_W + 1, knob_colors[0], hot ? 4U : 3U);
    }
    static const char *const labels[] = {"RATE Hz", "DEPTH", "TARGET", "WAVE"};
    static const char *const waves[] = {"TRI", "SQUARE", "SAW UP", "SAW DOWN"};
    for (unsigned i = 0; i < 4; ++i) {
        char value[12];
        if (!i) {
            unsigned rate = groove_lfo_rate(v[0]);
            char fraction[12]; decimal(value, rate / 100, 1); decimal(fraction, rate % 100, 2);
            strcat(value, "."); strcat(value, fraction);
        } else if (i == 1) { decimal(value, v[i], 1); strcat(value, "%"); }
        else strcpy(value, i == 2 ? (v[i] ? "PAN" : "VOLUME") : waves[v[i] & 3U]);
        knob(i, labels[i], value, colors[i]);
    }
}
static __attribute__((noinline)) void amp_env_draw(const struct groove_fm_controls *controls)
{
    unsigned col = knob_shown(), hot = knob_hot(col);
    uint32_t sig = signature(track | (hot << 8) | (col << 10) | ((unsigned)controls->enabled << 12), controls->envelope, 4) + (frame_patch[134] & 31U);
    if (cache->middle_cache == sig) return;
    cache->middle_cache = sig;
    paint_fill(0, 62, 240, 133, BACK);
    char num[12]; decimal(num, (int)(frame_patch[134] & 31U) + 1, 1);
    state_plate_cap(num, "ALGO");
    window_draw("AMP ENV", PAL_AMBER);
    sp_adsr(controls->envelope[0], controls->envelope[1], controls->envelope[2], controls->envelope[3], hot ? (int)col : -1, !controls->enabled, 178, 68);
}
